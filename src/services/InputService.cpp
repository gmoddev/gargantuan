#include "gargantuan/services/InputService.hpp"
#include "gargantuan/classes/InputAction.hpp"
#include "gargantuan/classes/InputBinding.hpp"
#include "gargantuan/classes/InputContext.hpp"
#include "gargantuan/input/InputConstruction.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		const char *ReasonName(CompiledInputGraph::Reason Reason) {
			using R = CompiledInputGraph::Reason;
			switch (Reason) {
			case R::Released:
				return "Released";
			case R::FocusLost:
				return "FocusLost";
			case R::DefinitionChanged:
				return "DefinitionChanged";
			case R::Disabled:
				return "Disabled";
			case R::Suppressed:
				return "Suppressed";
			case R::GuiConsumed:
				return "GuiConsumed";
			default:
				return "None";
			}
		}
		void PushId(lua_State *L, ObjectId Id) {
			lua_createtable(L, 0, 2);
			lua_pushnumber(L, Id.Slot);
			lua_setfield(L, -2, "Slot");
			lua_pushnumber(L, Id.Generation);
			lua_setfield(L, -2, "Generation");
		}
		void Number(lua_State *L, const char *Name, double Value) {
			lua_pushnumber(L, Value);
			lua_setfield(L, -2, Name);
		}
		void Boolean(lua_State *L, const char *Name, bool Value) {
			lua_pushboolean(L, Value);
			lua_setfield(L, -2, Name);
		}
	}

	InputService::~InputService() {
		for (auto &[Id, Observer] : DefinitionObservers)
			Disconnect(Observer);
		for (const auto &Connection : HierarchyConnections)
			Connection->Disconnect();
	}
	void InputService::Disconnect(const DefinitionObserver &Observer) {
		for (auto &[Signal, Connection] : Observer.Connections) {
			Connection->Disconnect();
			std::erase(Signal->Connections, Connection);
		}
	}

	void InputService::Observe() {
		if (Observing) return;
		Observing = true;
		HierarchyConnections.push_back(DescendantAdded->Connect([this](auto) { Dirty = true; }));
		HierarchyConnections.push_back(DescendantRemoved->Connect([this](auto) { Dirty = true; }));
	}

	void InputService::Watch(const std::shared_ptr<Instance> &Object, std::initializer_list<const char *> Properties) {
		auto [Entry, Added] = DefinitionObservers.try_emplace(Object->GetObjectId());
		Entry->second.Revision = DefinitionRevision;
		if (!Added) return;
		for (const auto *Property : Properties) {
			auto Signal = Object->GetPropertyChangedSignal(Property);
			Entry->second.Connections.emplace_back(Signal, Signal->Connect([this](auto) { Dirty = true; }));
		}
		Entry->second.Connections.emplace_back(Object->Destroying, Object->Destroying->Connect([this](auto) {
			Dirty = true;
		}));
	}

	void InputService::RegisterLegacyContext(ObjectId Context) {
		LegacyContext = Context;
		Dirty = true;
	}

	void InputService::EnsureCompiled() {
		AssertAuthoritativeMutation("Input compilation");
		if (Publishing || GetDestroyed()) return;
		Observe();
		if (!Dirty) return;
		Dirty = false;
		++DefinitionRevision;
		ValidationErrors.clear();
		FrameActions.clear();
		CompiledActions.clear();
		using G = CompiledInputGraph;
		std::vector<G::ContextDefinition> Contexts;
		std::vector<G::ActionDefinition> Actions;
		std::vector<G::BindingDefinition> Bindings;
		auto Error = [&](std::string Message) {
			if (ValidationErrors.size() < 64) ValidationErrors.push_back(std::move(Message));
		};
		bool Overflow = false;
		std::size_t Visited = 0;
		for (const auto &Child : Children) {
			if (++Visited > G::MaximumContexts + G::MaximumActions + G::MaximumBindings) {
				Overflow = true;
				break;
			}
			auto Context = std::dynamic_pointer_cast<InputContext>(Child);
			if (!Context || Context->GetDestroyed()) {
				Error("InputService children must be InputContext Instances");
				continue;
			}
			if (Contexts.size() == G::MaximumContexts) {
				Overflow = true;
				break;
			}
			Watch(Context, {"Name", "Enabled", "Priority", "Consume"});
			Contexts.push_back(
				{Context->GetObjectId(),
				 Context->GetPriority(),
				 Context->GetEnabled(),
				 Context->GetConsume(),
				 Context->GetObjectId() == LegacyContext}
			);
			for (const auto &ActionChild : Context->Children) {
				if (++Visited > G::MaximumContexts + G::MaximumActions + G::MaximumBindings) {
					Overflow = true;
					break;
				}
				auto Action = std::dynamic_pointer_cast<InputAction>(ActionChild);
				if (!Action || Action->GetDestroyed()) {
					Error("InputContext children must be InputAction Instances");
					continue;
				}
				if (Actions.size() == G::MaximumActions) {
					Overflow = true;
					break;
				}
				Watch(Action, {"Name", "Enabled", "Type"});
				Actions.push_back(
					{Action->GetObjectId(),
					 Context->GetObjectId(),
					 static_cast<G::Type>(Action->GetType()),
					 Action->GetEnabled()}
				);
				CompiledActions.push_back(Action);
				if (Action->GetType() == Enums::InputActionType::Vector2) FrameActions.push_back(Action);
				for (const auto &BindingChild : Action->Children) {
					if (++Visited > G::MaximumContexts + G::MaximumActions + G::MaximumBindings) {
						Overflow = true;
						break;
					}
					auto Binding = std::dynamic_pointer_cast<InputBinding>(BindingChild);
					if (!Binding || Binding->GetDestroyed()) {
						Error("InputAction children must be InputBinding Instances");
						continue;
					}
					if (Bindings.size() == G::MaximumBindings) {
						Overflow = true;
						break;
					}
					Watch(
						Binding,
						{"Enabled",
						 "Type",
						 "KeyCode",
						 "InputType",
						 "Modifier1",
						 "Modifier2",
						 "Modifier3",
						 "Modifier4",
						 "ExactModifiers",
						 "Scale",
						 "VectorScale",
						 "Priority",
						 "Consume"}
					);
					if (auto Message = Binding->ValidateDefinition(); !Message.empty()) {
						Error(std::move(Message));
						continue;
					}
					if (Binding->GetType() == Enums::InputBindingType::PointerDelta &&
						Action->GetType() != Enums::InputActionType::Vector2) {
						Error("PointerDelta bindings require a Vector2 action");
						continue;
					}
					std::uint8_t Modifiers = 0;
					for (auto Modifier : Binding->GetModifiers())
						Modifiers |= G::ModifierBit(static_cast<std::uint16_t>(Modifier));
					Bindings.push_back({
						.Id = Binding->GetObjectId(),
						.Action = Action->GetObjectId(),
						.InputKind = static_cast<G::Kind>(Binding->GetType()),
						.Code = static_cast<std::uint16_t>(
							Binding->GetType() == Enums::InputBindingType::MouseButton
								? static_cast<int>(Binding->GetInputType())
								: static_cast<int>(Binding->GetKeyCode())
						),
						.Modifiers = Modifiers,
						.ExactModifiers = Binding->GetExactModifiers(),
						.Enabled = Binding->GetEnabled(),
						.Consume = Binding->GetConsume(),
						.Priority = Binding->GetPriority(),
						.Scale = Binding->GetScale(),
						.VectorX = Binding->GetVectorScale().GetX(),
						.VectorY = Binding->GetVectorScale().GetY(),
					});
				}
				if (Overflow) break;
			}
			if (Overflow) break;
		}
		if (Overflow) {
			Error("Input graph capacity exceeded (64 contexts, 256 actions, 2048 bindings); evaluation disabled");
			Contexts.clear();
			Actions.clear();
			Bindings.clear();
			CompiledActions.clear();
			FrameActions.clear();
		}
		try {
			Graph.Compile(Contexts, Actions, Bindings);
		} catch (const std::exception &Failure) {
			Error(std::string("[Input:Compile] ") + Failure.what());
			CompiledActions.clear();
			FrameActions.clear();
			Graph.Compile({}, {}, {});
		}
		std::erase_if(DefinitionObservers, [this](auto &Entry) {
			if (Entry.second.Revision == DefinitionRevision) return false;
			Disconnect(Entry.second);
			return true;
		});
		Publish();
	}

	void InputService::Publish() {
		if (Publishing) return;
		Publishing = true;
		struct Guard {
			bool &Value;
			~Guard() {
				Value = false;
			}
		} ResetPublishing{Publishing};
		const auto Transitions = Graph.GetTransitions();
		std::array<bool, CompiledInputGraph::MaximumActions * 2> PreviousDown{};
		std::size_t Index = 0;
		// Commit every value before any callback can inspect another action.
		for (const auto &Transition : Transitions) {
			if (auto Action = std::dynamic_pointer_cast<InputAction>(ObjectRegistry::Get().Lookup(Transition.Action))) {
				PreviousDown[Index] = Action->IsDown();
				Action->CommitState(
					Action->GetType() == Enums::InputActionType::Vector2 ? Vector2(Transition.X, Transition.Y)
																		 : Vector2(Transition.Value, 0),
					Transition.Down,
					Transition.Value
				);
			}
			++Index;
		}
		Index = 0;
		for (const auto &Transition : Transitions) {
			const bool WasDown = PreviousDown[Index++];
			auto Action = std::dynamic_pointer_cast<InputAction>(ObjectRegistry::Get().Lookup(Transition.Action));
			if (!Action) continue;
			if (Transition.Down && !WasDown) Action->Pressed->Fire({});
			Action->Changed->Fire(Action->GetVector());
			if (!Transition.Down && WasDown) {
				if (Transition.Cancellation != CompiledInputGraph::Reason::Released &&
					Transition.Cancellation != CompiledInputGraph::Reason::None)
					Action->Canceled->Fire(ReasonName(Transition.Cancellation));
				Action->Released->Fire({});
			}
			Transitioned->Fire(Transition);
		}
		Graph.ClearTransitions();
	}

	bool InputService::ProcessEvent(const HostEvent &Event, bool GuiConsumed) {
		if (Publishing || GetDestroyed()) return false;
		EnsureCompiled();
		const bool Consumed = Graph.ProcessEvent(Event, GuiConsumed);
		Publish();
		return Consumed;
	}
	void InputService::EndFrame() {
		if (Publishing || GetDestroyed()) return;
		EnsureCompiled();
		Graph.EndFrame();
		Publish();
		for (const auto &Weak : FrameActions)
			if (auto Action = Weak.lock(); Action && !Action->GetDestroyed())
				Action->CommitState(Vector2(), Action->IsDown(), Action->GetValue());
	}
	void InputService::Reset(CompiledInputGraph::Reason Reason) {
		if (Publishing || GetDestroyed()) return;
		EnsureCompiled();
		Graph.Reset(Reason);
		Publish();
	}
	CompiledInputGraph::Snapshot InputService::Inspect() {
		EnsureCompiled();
		return Graph.Inspect();
	}

	int InputService::AddContext(lua_State *L, Instance *InstanceValue) {
		auto *Self = static_cast<InputService *>(InstanceValue);
		RequireInputMutation();
		Self->AssertCanMutate();
		auto Name = ReadInputValue<std::string>(L, 2);
		ValidateInputName(Name);
		ValidateInputOptions(L, 3, {"Enabled", "Priority", "Consume"});
		auto Context = std::make_shared<InputContext>();
		Context->SetName(std::move(Name));
		if (auto Value = ReadInputOption<bool>(L, 3, "Enabled")) Context->SetEnabled(*Value);
		if (auto Value = ReadInputOption<int>(L, 3, "Priority")) Context->SetPriority(*Value);
		if (auto Value = ReadInputOption<bool>(L, 3, "Consume")) Context->SetConsume(*Value);
		Context->SetParent(Self->shared_from_this());
		return StackValue<std::shared_ptr<InputContext>>::Push(L, Context);
	}

	int InputService::GetRuntimeSnapshot(lua_State *L, Instance *InstanceValue) {
		auto *Self = static_cast<InputService *>(InstanceValue);
		const auto Snapshot = Self->Inspect();
		lua_createtable(L, 0, 10);
		Number(L, "Version", 1);
		Number(L, "Revision", Snapshot.Stats.Revision);
		Number(L, "MemoryBytes", Snapshot.Stats.MemoryBytes);
		Number(L, "Modifiers", Snapshot.Modifiers);
		PushId(L, Self->GetReplicationScopeId());
		lua_setfield(L, -2, "Scope");
		lua_createtable(L, static_cast<int>(Snapshot.PressedKeys.size()), 0);
		for (std::size_t I = 0; I < Snapshot.PressedKeys.size(); ++I) {
			lua_pushnumber(L, Snapshot.PressedKeys[I]);
			lua_rawseti(L, -2, static_cast<int>(I + 1));
		}
		lua_setfield(L, -2, "PressedKeys");
		lua_createtable(L, static_cast<int>(Snapshot.Contexts.size()), 0);
		int Index = 1;
		for (const auto &Context : Snapshot.Contexts) {
			lua_createtable(L, 0, 4);
			PushId(L, Context.Id);
			lua_setfield(L, -2, "Id");
			Number(L, "Priority", Context.Priority);
			Boolean(L, "Enabled", Context.Enabled);
			Boolean(L, "Consume", Context.Consume);
			lua_rawseti(L, -2, Index++);
		}
		lua_setfield(L, -2, "Contexts");
		lua_createtable(L, static_cast<int>(Snapshot.Actions.size()), 0);
		Index = 1;
		for (const auto &Action : Snapshot.Actions) {
			lua_createtable(L, 0, 9);
			PushId(L, Action.Definition.Id);
			lua_setfield(L, -2, "Id");
			PushId(L, Action.Definition.Context);
			lua_setfield(L, -2, "Context");
			Boolean(L, "Enabled", Action.Definition.Enabled);
			Boolean(L, "Down", Action.State.Down);
			Number(L, "Value", Action.State.Value);
			Number(L, "LastTransition", Action.State.LastTransition);
			StackValue<Vector2>::Push(L, Vector2(Action.State.X, Action.State.Y));
			lua_setfield(L, -2, "Vector");
			lua_pushstring(L, ReasonName(Action.State.LastReason));
			lua_setfield(L, -2, "LastReason");
			lua_rawseti(L, -2, Index++);
		}
		lua_setfield(L, -2, "Actions");
		lua_createtable(L, static_cast<int>(Snapshot.Bindings.size()), 0);
		Index = 1;
		for (const auto &Binding : Snapshot.Bindings) {
			lua_createtable(L, 0, 8);
			PushId(L, Binding.Definition.Id);
			lua_setfield(L, -2, "Id");
			PushId(L, Binding.Definition.Action);
			lua_setfield(L, -2, "Action");
			Boolean(L, "Matched", Binding.Matched);
			Boolean(L, "Active", Binding.Active);
			Boolean(L, "Suppressed", Binding.Suppressed);
			Boolean(L, "Eligible", Binding.Eligible);
			PushId(L, Binding.WinningContext);
			lua_setfield(L, -2, "WinningContext");
			lua_rawseti(L, -2, Index++);
		}
		lua_setfield(L, -2, "Bindings");
		lua_createtable(L, static_cast<int>(Self->ValidationErrors.size()), 0);
		Index = 1;
		for (const auto &Error : Self->ValidationErrors) {
			lua_pushlstring(L, Error.data(), Error.size());
			lua_rawseti(L, -2, Index++);
		}
		lua_setfield(L, -2, "ValidationErrors");
		return 1;
	}
}

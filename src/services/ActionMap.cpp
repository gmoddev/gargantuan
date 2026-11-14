#include "gargantuan/services/ActionMap.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/InputAction.hpp"
#include "gargantuan/classes/InputBinding.hpp"
#include "gargantuan/classes/InputContext.hpp"
#include "gargantuan/input/InputConstruction.hpp"
#include "gargantuan/services/InputService.hpp"
#include "gargantuan/services/UserInputService.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gargantuan {
	namespace {
		void ValidateLegacyBinding(const std::string &Name, float Scale) {
			ValidateInputName(Name);
			if (!std::isfinite(Scale)) throw std::invalid_argument("[Input:ActionMap] Scale must be finite");
		}
	}
	ActionMap::~ActionMap() {
		if (TransitionConnection) TransitionConnection->Disconnect();
	}

	std::shared_ptr<InputService> ActionMap::GetInput() {
		if (auto Existing = RuntimeInput.lock(); Existing && !Existing->GetDestroyed()) return Existing;
		std::shared_ptr<InputService> Service;
		if (auto World = GetDataModel())
			Service = std::dynamic_pointer_cast<InputService>(World->GetService("InputService"));
		else {
			LocalInput = std::make_shared<InputService>();
			Service = LocalInput;
		}
		if (!Service) throw std::runtime_error("[Input:ActionMap] Canonical InputService is unavailable");
		RuntimeInput = Service;
		if (TransitionConnection) TransitionConnection->Disconnect();
		TransitionConnection = Service->Transitioned->Connect([this](CompiledInputGraph::Transition Transition) {
			auto Action = std::dynamic_pointer_cast<InputAction>(ObjectRegistry::Get().Lookup(Transition.Action));
			if (!Action || Action->GetDestroyed()) return;
			const auto Name = Action->GetName();
			RefreshNames(Transition.Action);
			const auto Found = NamedActions.find(Name);
			const auto Selected = Found == NamedActions.end() ? nullptr : Found->second.lock();
			const bool WasDown = PublishedNames.contains(Transition.Action);
			if (Selected != Action) {
				if (WasDown) {
					PublishedNames.erase(Transition.Action);
					if (!Selected || !Selected->IsDown()) ActionEnded->Fire(Name);
				}
				return;
			}
			if (Transition.Down)
				PublishedNames[Transition.Action] = Name;
			else
				PublishedNames.erase(Transition.Action);
			if (Transition.Down && !WasDown) ActionBegan->Fire(Name);
			ActionChanged->Fire(
				{Name, Transition.VectorChanged ? Vector2(Transition.X, Transition.Y) : Vector2(Transition.Value, 0.0f)}
			);
			if (!Transition.Down && WasDown) ActionEnded->Fire(Name);
		});
		CachedDefinitionRevision = std::numeric_limits<std::uint64_t>::max();
		return Service;
	}
	void ActionMap::AttachInputService(const std::shared_ptr<UserInputService> &Physical) {
		const bool Reopening = RuntimeStopping;
		if (Reopening && !Bindings.empty())
			throw std::logic_error("[Input:ActionMap] Cannot attach after incomplete runtime shutdown");
		auto Service = GetInput();
		Service->Reset();
		// Seed the existing physical authority once. Marking these samples consumed
		// records held state without inventing a fresh press for authored actions.
		if (Physical) {
			for (const auto &Input : Physical->GetKeysPressed()) {
				if (!Input) continue;
				(void)Service->ProcessEvent(
					KeyEvent{.Logical = static_cast<LogicalKey>(Input->GetKeyCode()), .State = ButtonState::Pressed}, true
				);
			}
			for (const auto &Input : Physical->GetMouseButtonsPressed()) {
				if (!Input) continue;
				PointerButton Button;
				switch (Input->GetUserInputType()) {
				case Enums::UserInputType::MouseButton1:
					Button = PointerButton::Left;
					break;
				case Enums::UserInputType::MouseButton2:
					Button = PointerButton::Right;
					break;
				case Enums::UserInputType::MouseButton3:
					Button = PointerButton::Middle;
					break;
				default:
					continue;
				}
				(void)Service->ProcessEvent(PointerButtonEvent{.Button = Button, .State = ButtonState::Pressed}, true);
			}
		}
		// Keep teardown admission closed through reset and native transition
		// callbacks. Only a completely retired adapter can start a new runtime.
		if (Reopening) RuntimeStopping = false;
	}

	std::shared_ptr<InputContext> ActionMap::GetLegacyContext() {
		if (auto Existing = LegacyContext.lock(); Existing && !Existing->GetDestroyed()) return Existing;
		auto Service = GetInput();
		auto Context = std::make_shared<InputContext>();
		Context->SetName("LegacyActionMap");
		Context->SetArchivable(false);
		Context->SetConsume(false);
		Context->SetParent(Service);
		Service->RegisterLegacyContext(Context->GetObjectId());
		LegacyContext = Context;
		return Context;
	}

	void ActionMap::RefreshNames(ObjectId CurrentTransition) {
		if (RefreshingNames) return;
		RefreshingNames = true;
		struct RefreshGuard {
			bool &Flag;
			~RefreshGuard() {
				Flag = false;
			}
		} Guard{RefreshingNames};
		auto Service = GetInput();
		const auto Revision = Service->GetDefinitionRevision();
		if (CurrentTransition.IsValid() && Revision == CachedDefinitionRevision) return;
		if (Revision != CachedDefinitionRevision) {
			CachedDefinitionRevision = Revision;
			NamesNeedPublication = true;
			NamedActions.clear();
			std::unordered_map<std::string, std::shared_ptr<InputContext>> Winners;
			for (const auto &Weak : Service->GetCompiledActions()) {
				auto Action = Weak.lock();
				if (!Action || Action->GetDestroyed() || !Action->GetEnabled()) continue;
				auto Parent = Action->GetParent();
				auto Context = Parent ? std::dynamic_pointer_cast<InputContext>(*Parent) : nullptr;
				if (!Context || Context->GetDestroyed() || !Context->GetEnabled()) continue;
				const auto Name = Action->GetName();
				auto Found = Winners.find(Name);
				bool Replace = Found == Winners.end();
				if (!Replace) {
					const auto &PreviousContext = Found->second;
					auto Previous = NamedActions[Name].lock();
					Replace = Context->GetPriority() > PreviousContext->GetPriority() ||
							  (Context->GetPriority() == PreviousContext->GetPriority() &&
							   (Context->GetObjectId() < PreviousContext->GetObjectId() ||
								(Context == PreviousContext &&
								 (!Previous || Action->GetObjectId() < Previous->GetObjectId()))));
				}
				if (Replace) {
					Winners[Name] = Context;
					NamedActions[Name] = Action;
				}
			}
		}
		if (!NamesNeedPublication) return;
		// Name selection is a facade projection. Commit its bounded state before
		// callbacks. Actual graph transitions retain their own priority order.
		std::vector<std::string> Ended;
		for (auto Entry = PublishedNames.begin(); Entry != PublishedNames.end();) {
			auto Found = NamedActions.find(Entry->second);
			auto Selected = Found == NamedActions.end() ? nullptr : Found->second.lock();
			const bool Same = Selected && !Selected->GetDestroyed() && Selected->GetObjectId() == Entry->first &&
							  Selected->GetName() == Entry->second;
			if (Same && (Selected->IsDown() || Entry->first == CurrentTransition)) {
				++Entry;
				continue;
			}
			Ended.push_back(Entry->second);
			Entry = PublishedNames.erase(Entry);
		}
		std::vector<std::pair<std::string, Vector2>> Began;
		if (!CurrentTransition.IsValid() && !Service->IsPublishing()) {
			for (const auto &[Name, Weak] : NamedActions) {
				auto Action = Weak.lock();
				if (!Action || Action->GetDestroyed() || !Action->IsDown() ||
					PublishedNames.contains(Action->GetObjectId()))
					continue;
				PublishedNames[Action->GetObjectId()] = Name;
				Began.emplace_back(
					Name,
					Action->GetType() == Enums::InputActionType::Vector2 ? Action->GetVector()
																		 : Vector2(Action->GetValue(), 0)
				);
			}
			NamesNeedPublication = false;
		}
		std::ranges::sort(Ended);
		std::ranges::sort(Began, {}, &std::pair<std::string, Vector2>::first);
		for (const auto &Name : Ended)
			ActionEnded->Fire(Name);
		for (const auto &[Name, Value] : Began) {
			ActionBegan->Fire(Name);
			ActionChanged->Fire({Name, Value});
		}
	}
	std::shared_ptr<InputAction> ActionMap::GetAction(const std::string &Name) {
		RefreshNames();
		const auto Found = NamedActions.find(Name);
		if (Found == NamedActions.end()) return nullptr;
		auto Action = Found->second.lock();
		return Action && !Action->GetDestroyed() ? Action : nullptr;
	}
	void ActionMap::CleanBindings() {
		std::erase_if(Bindings, [](const auto &Entry) {
			auto Binding = Entry.second.lock();
			return !Binding || Binding->GetDestroyed();
		});
		std::unordered_map<std::string, std::weak_ptr<InputAction>> Renamed;
		for (const auto &[Name, Weak] : OwnedActions) {
			auto Action = Weak.lock();
			if (!Action || Action->GetDestroyed()) continue;
			auto Found = Renamed.find(Action->GetName());
			auto Previous = Found == Renamed.end() ? nullptr : Found->second.lock();
			if (!Previous || Action->GetObjectId() < Previous->GetObjectId()) Renamed[Action->GetName()] = Action;
		}
		OwnedActions.swap(Renamed);
	}

	int ActionMap::AddBinding(const std::string &Name, const std::shared_ptr<InputBinding> &Binding, bool Pointer) {
		if (RuntimeStopping) return 0;
		RequireInputMutation();
		AssertCanMutate();
		if (auto Error = Binding->ValidateDefinition(); !Error.empty()) throw std::invalid_argument(Error);
		CleanBindings();
		if (Bindings.size() >= MaximumBindings) throw std::length_error("[Input:ActionMap] Binding limit reached");
		if (NextBindingId <= 0 || NextBindingId == std::numeric_limits<int>::max())
			throw std::overflow_error("[Input:ActionMap] Binding identity is exhausted");
		auto Found = OwnedActions.find(Name);
		auto Action = Found != OwnedActions.end() ? Found->second.lock() : nullptr;
		if (!Action) {
			if (OwnedActions.size() >= MaximumActions)
				throw std::length_error("[Input:ActionMap] Action limit reached");
			Action = std::make_shared<InputAction>();
			Action->SetName(Name);
			Action->SetType(Pointer ? Enums::InputActionType::Vector2 : Enums::InputActionType::Scalar);
			Action->SetParent(GetLegacyContext());
			OwnedActions[Name] = Action;
		} else if (Pointer && Action->GetType() != Enums::InputActionType::Vector2) {
			Action->SetType(Enums::InputActionType::Vector2);
		}
		const auto Id = NextBindingId++;
		Binding->SetName("Binding" + std::to_string(Id));
		Binding->SetParent(Action);
		Bindings.emplace(Id, Binding);
		GetInput()->EnsureCompiled();
		return Id;
	}
	int ActionMap::BindKey(std::string Name, Enums::KeyCode Key, float Scale, int Priority, bool Consume) {
		if (RuntimeStopping) return 0;
		ValidateLegacyBinding(Name, Scale);
		if (Key == Enums::KeyCode::None) throw std::invalid_argument("[Input:ActionMap] Key binding requires a key");
		auto Binding = std::make_shared<InputBinding>();
		Binding->SetKeyCode(Key);
		Binding->SetScale(Scale);
		Binding->SetPriority(Priority);
		Binding->SetConsume(Consume);
		return AddBinding(Name, Binding, false);
	}
	int ActionMap::BindMouseButton(
		std::string Name, Enums::UserInputType InputType, float Scale, int Priority, bool Consume
	) {
		if (RuntimeStopping) return 0;
		ValidateLegacyBinding(Name, Scale);
		if (InputType != Enums::UserInputType::MouseButton1 && InputType != Enums::UserInputType::MouseButton2 &&
			InputType != Enums::UserInputType::MouseButton3)
			throw std::invalid_argument("[Input:ActionMap] Mouse binding requires a supported mouse button");
		auto Binding = std::make_shared<InputBinding>();
		Binding->SetType(Enums::InputBindingType::MouseButton);
		Binding->SetInputType(InputType);
		Binding->SetScale(Scale);
		Binding->SetPriority(Priority);
		Binding->SetConsume(Consume);
		return AddBinding(Name, Binding, false);
	}
	int ActionMap::BindPointerDelta(std::string Name, Vector2 Scale, int Priority, bool Consume) {
		if (RuntimeStopping) return 0;
		ValidateLegacyBinding(Name, Scale.GetX());
		if (!std::isfinite(Scale.GetY())) throw std::invalid_argument("[Input:ActionMap] Vector scale must be finite");
		auto Binding = std::make_shared<InputBinding>();
		Binding->SetType(Enums::InputBindingType::PointerDelta);
		Binding->SetVectorScale(Scale);
		Binding->SetPriority(Priority);
		Binding->SetConsume(Consume);
		return AddBinding(Name, Binding, true);
	}

	bool ActionMap::Unbind(int Id) {
		RequireInputMutation();
		AssertCanMutate();
		auto Found = Bindings.find(Id);
		if (Found == Bindings.end()) return false;
		auto Binding = Found->second.lock();
		Bindings.erase(Found);
		if (!Binding || Binding->GetDestroyed()) return false;
		auto Parent = Binding->GetParent();
		auto Action = Parent ? std::dynamic_pointer_cast<InputAction>(*Parent) : nullptr;
		Binding->Destroy();
		GetInput()->EnsureCompiled();
		if (Action && Action->Children.empty()) {
			const auto Owned = std::find_if(OwnedActions.begin(), OwnedActions.end(), [&](const auto &Entry) {
				return Entry.second.lock() == Action;
			});
			if (Owned != OwnedActions.end()) {
				OwnedActions.erase(Owned);
				if (auto Published = PublishedNames.find(Action->GetObjectId()); Published != PublishedNames.end()) {
					const auto PublishedName = Published->second;
					PublishedNames.erase(Published);
					ActionEnded->Fire(PublishedName);
				}
				Action->Destroy();
				GetInput()->EnsureCompiled();
			}
		}
		return true;
	}
	void ActionMap::UnbindAction(std::string Name) {
		RequireInputMutation();
		ValidateProtocolString(Name, MaximumActionNameBytes, "Action name");
		std::vector<int> Ids;
		for (const auto &[Id, Weak] : Bindings) {
			auto Binding = Weak.lock();
			if (!Binding || Binding->GetDestroyed()) continue;
			auto Parent = Binding->GetParent();
			if (Parent && (*Parent)->GetName() == Name) Ids.push_back(Id);
		}
		std::ranges::sort(Ids);
		for (const auto Id : Ids)
			(void)Unbind(Id);
	}
	bool ActionMap::IsDown(std::string Name) {
		auto Action = GetAction(Name);
		return Action && Action->IsDown();
	}
	float ActionMap::GetValue(std::string Name) {
		auto Action = GetAction(Name);
		return Action ? Action->GetValue() : 0.0f;
	}
	Vector2 ActionMap::GetVector(std::string Name) {
		auto Action = GetAction(Name);
		return Action && Action->GetType() == Enums::InputActionType::Vector2 ? Action->GetVector() : Vector2();
	}
	int ActionMap::GetBindingCount() {
		CleanBindings();
		return static_cast<int>(Bindings.size());
	}
	bool ActionMap::ProcessEvent(const HostEvent &Event) {
		return GetInput()->ProcessEvent(Event);
	}
	void ActionMap::ProcessConsumedRelease(const HostEvent &Event) {
		(void)GetInput()->ProcessEvent(Event, true);
	}
	void ActionMap::EndFrame() {
		GetInput()->EndFrame();
	}
	void ActionMap::Reset() {
		// Engine teardown has already stopped Lua callbacks. Release the native
		// legacy bindings here; a Script.Destroying cleanup cannot run afterward.
		// Native Destroying/ActionEnded observers can still run synchronously.
		RuntimeStopping = true;
		// The adapter admits at most MaximumBindings IDs. Process them in the
		// previous ascending order without allocating during teardown.
		for (std::size_t Index = 0; Index < MaximumBindings && !Bindings.empty(); ++Index) {
			const auto Next = std::min_element(Bindings.begin(), Bindings.end(),
				[](const auto &Left, const auto &Right) { return Left.first < Right.first; });
			(void)Unbind(Next->first);
		}
		if (!Bindings.empty()) throw std::logic_error("[Input:ActionMap] Shutdown binding bound exceeded");
		GetInput()->Reset();
	}
}

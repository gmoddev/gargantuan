#include "gargantuan/input/CompiledInputGraph.hpp"

#include "gargantuan/classes/InputObject.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace gargantuan {
	CompiledInputGraph::CompiledInputGraph() {
		Contexts.reserve(MaximumContexts);
		Actions.reserve(MaximumActions);
		Bindings.reserve(MaximumBindings);
		Stats.MemoryBytes = MemoryBytes();
	}

	std::uint8_t CompiledInputGraph::ModifierBit(std::uint16_t Code) {
		switch (static_cast<LogicalKey>(Code)) {
		case LogicalKey::LeftControl:
			return 1u << 0;
		case LogicalKey::RightControl:
			return 1u << 1;
		case LogicalKey::LeftShift:
			return 1u << 2;
		case LogicalKey::RightShift:
			return 1u << 3;
		case LogicalKey::LeftAlt:
			return 1u << 4;
		case LogicalKey::RightAlt:
			return 1u << 5;
		case LogicalKey::LeftMeta:
			return 1u << 6;
		case LogicalKey::RightMeta:
			return 1u << 7;
		default:
			return 0;
		}
	}

	std::size_t CompiledInputGraph::Bucket(const BindingDefinition &Definition) {
		if (Definition.InputKind == Kind::PointerDelta) return PointerCode;
		if (Definition.InputKind == Kind::MouseButton) {
			if (Definition.Code == static_cast<std::uint16_t>(Enums::UserInputType::MouseButton1)) return MouseBase;
			if (Definition.Code == static_cast<std::uint16_t>(Enums::UserInputType::MouseButton2)) return MouseBase + 1;
			if (Definition.Code == static_cast<std::uint16_t>(Enums::UserInputType::MouseButton3)) return MouseBase + 2;
			throw std::invalid_argument("[Input:Compiler] Unsupported mouse button");
		}
		if (Definition.InputKind != Kind::Key || Definition.Code == 0 || Definition.Code >= PhysicalCodeCount ||
			Definition.Code == static_cast<std::uint16_t>(Enums::KeyCode::None))
			throw std::invalid_argument("[Input:Compiler] Invalid physical key");
		// KeyCode is closed. Numeric holes must not become bindable hidden inputs.
		if (!Enum::fromType<Enums::KeyCode>()->FromValue(static_cast<int>(Definition.Code)))
			throw std::invalid_argument("[Input:Compiler] Unknown physical key");
		return Definition.Code;
	}

	bool CompiledInputGraph::Compile(
		std::span<const ContextDefinition> NewContexts,
		std::span<const ActionDefinition> NewActions,
		std::span<const BindingDefinition> NewBindings
	) {
		ClearTransitions();
		if (NewContexts.size() > MaximumContexts || NewActions.size() > MaximumActions ||
			NewBindings.size() > MaximumBindings)
			throw std::length_error("[Input:Compiler] Input graph capacity exceeded");
		std::unordered_set<ObjectId> Identities;
		auto ValidateIdentity = [&](ObjectId Id) {
			if (!Id.IsValid() || !Identities.insert(Id).second)
				throw std::invalid_argument("[Input:Compiler] Invalid or duplicate generation-safe identity");
		};
		std::unordered_map<ObjectId, std::uint16_t> ContextIndices;
		std::unordered_map<ObjectId, std::uint16_t> ActionIndices;
		std::unordered_map<ObjectId, std::size_t> OldContexts, OldActions, OldBindings;
		for (std::size_t Index = 0; Index < Contexts.size(); ++Index)
			OldContexts.emplace(Contexts[Index].Id, Index);
		for (std::size_t Index = 0; Index < Actions.size(); ++Index)
			OldActions.emplace(Actions[Index].Definition.Id, Index);
		for (std::size_t Index = 0; Index < Bindings.size(); ++Index)
			OldBindings.emplace(Bindings[Index].Definition.Id, Index);
		for (std::size_t Index = 0; Index < NewContexts.size(); ++Index) {
			ValidateIdentity(NewContexts[Index].Id);
			ContextIndices.emplace(NewContexts[Index].Id, static_cast<std::uint16_t>(Index));
		}
		std::vector<ActionRow> NextActions;
		NextActions.reserve(NewActions.size());
		for (const auto &Definition : NewActions) {
			ValidateIdentity(Definition.Id);
			const auto Context = ContextIndices.find(Definition.Context);
			if (Context == ContextIndices.end() || Definition.ValueType > Type::Vector2)
				throw std::invalid_argument("[Input:Compiler] Invalid action context or type");
			ActionIndices.emplace(Definition.Id, static_cast<std::uint16_t>(NextActions.size()));
			ActionRow Row{
				.Definition = Definition,
				.Context = Context->second,
				.PublicationPriority = std::numeric_limits<int>::min()
			};
			if (auto Old = OldActions.find(Definition.Id); Old != OldActions.end())
				Row.State = Actions[Old->second].State;
			NextActions.push_back(std::move(Row));
		}
		std::vector<BindingRow> NextBindings;
		NextBindings.reserve(NewBindings.size());
		std::bitset<BucketCount> ChangedCodes;
		std::bitset<MaximumActions> DisabledActions;
		std::size_t ChangedBindings = 0;
		for (const auto &Definition : NewBindings) {
			ValidateIdentity(Definition.Id);
			const auto Action = ActionIndices.find(Definition.Action);
			if (Action == ActionIndices.end() || !std::isfinite(Definition.Scale) ||
				!std::isfinite(Definition.VectorX) || !std::isfinite(Definition.VectorY) ||
				std::popcount(Definition.Modifiers) > 4 || (ModifierBit(Definition.Code) & Definition.Modifiers) != 0 ||
				(Definition.InputKind != Kind::Key && (Definition.Modifiers != 0 || Definition.ExactModifiers)))
				throw std::invalid_argument("[Input:Compiler] Invalid binding fields or modifiers");
			const auto Code = Bucket(Definition);
			const auto &ActionValue = NextActions[Action->second];
			const auto &Context = NewContexts[ActionValue.Context];
			if (Definition.InputKind == Kind::PointerDelta && ActionValue.Definition.ValueType != Type::Vector2)
				throw std::invalid_argument("[Input:Compiler] Pointer delta requires a Vector2 action");
			BindingRow Row{.Definition = Definition, .Action = Action->second};
			bool Unchanged = false;
			if (auto Old = OldBindings.find(Definition.Id); Old != OldBindings.end()) {
				const auto &Previous = Bindings[Old->second];
				const auto &PreviousAction = Actions[Previous.Action];
				const auto &PreviousContext = Contexts[PreviousAction.Context];
				Unchanged = Previous.Definition == Definition && PreviousAction.Definition == ActionValue.Definition &&
							PreviousContext == Context;
				if (Unchanged) {
					Row = Previous;
					Row.Action = Action->second;
				} else {
					ChangedCodes.set(Bucket(Previous.Definition));
					if (Previous.Active && !Definition.Enabled) DisabledActions.set(Action->second);
				}
				if (Old->second != NextBindings.size()) ChangedCodes.set(Code);
			}
			if (!Unchanged) {
				++ChangedBindings;
				ChangedCodes.set(Code);
				Row.Eligible = Context.LegacyPassthrough && PhysicalDown[Code];
			}
			NextActions[Action->second].Bindings.push_back(static_cast<std::uint16_t>(NextBindings.size()));
			NextActions[Action->second].PublicationPriority = std::max(
				NextActions[Action->second].PublicationPriority, Definition.Priority
			);
			NextBindings.push_back(Row);
		}
		for (const auto &Previous : Bindings) {
			if (!Identities.contains(Previous.Definition.Id)) {
				++ChangedBindings;
				ChangedCodes.set(Bucket(Previous.Definition));
			}
		}
		bool Same = NewContexts.size() == Contexts.size() && NewActions.size() == Actions.size() &&
					NewBindings.size() == Bindings.size();
		if (Same) {
			for (std::size_t Index = 0; Index < NewContexts.size(); ++Index)
				Same = Same && NewContexts[Index] == Contexts[Index];
			for (std::size_t Index = 0; Index < NewActions.size(); ++Index)
				Same = Same && NewActions[Index] == Actions[Index].Definition;
			for (std::size_t Index = 0; Index < NewBindings.size(); ++Index)
				Same = Same && NewBindings[Index] == Bindings[Index].Definition;
		}
		Stats.ChangedBindings = ChangedBindings;
		Stats.ReindexedBuckets = 0;
		if (Same) return false;

		// Moving dense slots invalidates old bucket indexes; repack occupied buckets only.
		bool LayoutChanged = NewBindings.size() != Bindings.size();
		for (std::size_t Index = 0; Index < std::min(NewBindings.size(), Bindings.size()); ++Index)
			LayoutChanged = LayoutChanged || NewBindings[Index].Id != Bindings[Index].Definition.Id;
		if (LayoutChanged) {
			for (const auto &Row : Bindings)
				ChangedCodes.set(Bucket(Row.Definition));
			for (const auto &Row : NextBindings)
				ChangedCodes.set(Bucket(Row.Definition));
		}
		// Allocate candidate index storage before changing the live projection.
		std::array<std::vector<std::uint16_t>, BucketCount> NextBuckets;
		std::array<std::size_t, BucketCount> BucketSizes{};
		for (const auto &Row : NextBindings)
			++BucketSizes[Bucket(Row.Definition)];
		for (std::size_t Code = 0; Code < BucketCount; ++Code)
			if (ChangedCodes[Code]) NextBuckets[Code].reserve(BucketSizes[Code]);
		for (std::size_t Index = 0; Index < NextBindings.size(); ++Index) {
			const auto Code = Bucket(NextBindings[Index].Definition);
			if (ChangedCodes[Code]) NextBuckets[Code].push_back(static_cast<std::uint16_t>(Index));
		}
		std::array<std::vector<std::uint16_t>, 8> NextModifierBuckets;
		std::array<std::bitset<BucketCount>, 8> SeenModifierCodes;
		for (const auto &Row : NextBindings) {
			const auto Code = Bucket(Row.Definition);
			for (std::size_t Bit = 0; Bit < 8; ++Bit) {
				if ((Row.Definition.ExactModifiers || (Row.Definition.Modifiers & (1u << Bit))) &&
					!SeenModifierCodes[Bit][Code]) {
					NextModifierBuckets[Bit].push_back(static_cast<std::uint16_t>(Code));
					SeenModifierCodes[Bit].set(Code);
				}
			}
		}
		std::vector<ContextDefinition> NextContexts(NewContexts.begin(), NewContexts.end());
		for (auto &Previous : Actions)
			if (!ActionIndices.contains(Previous.Definition.Id))
				Publish(Previous, 0.0f, 0.0f, 0.0f, Reason::DefinitionChanged);
		Contexts.swap(NextContexts);
		Actions.swap(NextActions);
		Bindings.swap(NextBindings);
		ModifierBuckets.swap(NextModifierBuckets);
		++Stats.Revision;
		DirtyActions.reset();
		DirtyCount = 0;
		for (std::size_t Code = 0; Code < BucketCount; ++Code) {
			if (!ChangedCodes[Code]) continue;
			Buckets[Code].swap(NextBuckets[Code]);
			std::ranges::sort(Buckets[Code], [&](auto Left, auto Right) { return Higher(Left, Right); });
			++Stats.ReindexedBuckets;
		}
		// A removed final binding also needs to release its still-existing action.
		for (std::size_t Index = 0; Index < Actions.size(); ++Index) {
			const auto &Action = Actions[Index];
			const bool Disabled = !Action.Definition.Enabled || !Contexts[Action.Context].Enabled ||
								  DisabledActions[Index];
			Dirty(static_cast<std::uint16_t>(Index), Disabled ? Reason::Disabled : Reason::DefinitionChanged);
		}
		for (std::size_t Code = 0; Code < BucketCount; ++Code)
			if (ChangedCodes[Code] && Code != PointerCode) (void)Evaluate(Code, Reason::DefinitionChanged);
		RefreshActions();
		Stats.MemoryBytes = MemoryBytes();
		return true;
	}

	bool CompiledInputGraph::Higher(std::uint16_t Left, std::uint16_t Right) const {
		const auto &LeftBinding = Bindings[Left];
		const auto &RightBinding = Bindings[Right];
		const auto &LeftContext = Contexts[Actions[LeftBinding.Action].Context];
		const auto &RightContext = Contexts[Actions[RightBinding.Action].Context];
		if (LeftContext.Priority != RightContext.Priority) return LeftContext.Priority > RightContext.Priority;
		if (LeftContext.Id != RightContext.Id) return LeftContext.Id < RightContext.Id;
		if (LeftBinding.Definition.Priority != RightBinding.Definition.Priority)
			return LeftBinding.Definition.Priority > RightBinding.Definition.Priority;
		return LeftBinding.Definition.Id < RightBinding.Definition.Id;
	}

	bool CompiledInputGraph::Chord(const BindingRow &Binding) const {
		const auto Required = Binding.Definition.Modifiers;
		const auto MainBit = Binding.Definition.InputKind == Kind::Key ? ModifierBit(Binding.Definition.Code) : 0;
		const auto Held = static_cast<std::uint8_t>(Modifiers & ~MainBit);
		return (Held & Required) == Required && (!Binding.Definition.ExactModifiers || Held == Required);
	}

	bool CompiledInputGraph::Enabled(const BindingRow &Binding) const {
		const auto &Action = Actions[Binding.Action];
		return Binding.Definition.Enabled && Action.Definition.Enabled && Contexts[Action.Context].Enabled;
	}

	void CompiledInputGraph::Dirty(std::uint16_t Action, Reason Cancellation, std::int64_t Priority) {
		if (!DirtyActions[Action]) {
			DirtyOrder[DirtyCount++] = Action;
			DirtyPriorities[Action] = Priority;
		} else {
			DirtyPriorities[Action] = std::max(DirtyPriorities[Action], Priority);
		}
		if (!DirtyActions[Action] || Cancellation != Reason::None) DirtyReasons[Action] = Cancellation;
		DirtyActions.set(Action);
	}

	bool CompiledInputGraph::Evaluate(std::size_t Code, Reason Cancellation) {
		bool Consumed = false;
		ObjectId Winner;
		for (auto Index : Buckets[Code]) {
			++Stats.EvaluatedBindings;
			auto &Row = Bindings[Index];
			const auto &Context = Contexts[Actions[Row.Action].Context];
			const bool Previous = Row.Active;
			Row.Matched = Enabled(Row) && (Code == PointerCode || PhysicalDown[Code]) && Chord(Row);
			Row.Suppressed = Row.Matched && Winner.IsValid() && Winner != Context.Id;
			Row.WinningContext = Row.Suppressed ? Winner : ObjectId{};
			if (Row.Suppressed || (!Row.Matched && !Context.LegacyPassthrough)) Row.Eligible = false;
			Row.Active = Row.Matched && !Row.Suppressed && (Code == PointerCode || Row.Eligible);
			const bool ClaimsInput = Row.Matched && !Row.Suppressed && !Context.LegacyPassthrough &&
									 Cancellation != Reason::GuiConsumed && (Context.Consume || Row.Definition.Consume);
			if (ClaimsInput) Winner = Context.Id;
			if ((ClaimsInput || Row.Active || Previous || (Context.LegacyPassthrough && Enabled(Row))) &&
				(Context.Consume || Row.Definition.Consume))
				Consumed = true;
			if (Previous != Row.Active) {
				const auto ReasonValue = Row.Suppressed	 ? Reason::Suppressed
										 : !Enabled(Row) ? Reason::Disabled
														 : Cancellation;
				Dirty(Row.Action, ReasonValue, Row.Definition.Priority);
			}
		}
		return Consumed;
	}

	void CompiledInputGraph::Publish(ActionRow &Action, float Value, float X, float Y, Reason Cancellation) {
		const bool Down = Value != 0.0f;
		if (Action.State.Down == Down && Action.State.Value == Value && Action.State.X == X && Action.State.Y == Y)
			return;
		const bool VectorChanged = Action.State.X != X || Action.State.Y != Y;
		Action.State = {
			.Down = Down,
			.Value = Value,
			.X = X,
			.Y = Y,
			.LastReason = Down ? Reason::None : Cancellation,
			.LastTransition = ++TransitionSequence,
		};
		Transitions[TransitionCount++] = {
			.Action = Action.Definition.Id,
			.Down = Down,
			.Value = Value,
			.X = X,
			.Y = Y,
			.Cancellation = Down ? Reason::None : Cancellation,
			.VectorChanged = VectorChanged,
		};
	}

	void CompiledInputGraph::RefreshActions() {
		auto Order = DirtyOrder;
		const auto Count = DirtyCount;
		std::sort(Order.begin(), Order.begin() + Count, [&](auto Left, auto Right) {
			const auto &LeftContext = Contexts[Actions[Left].Context];
			const auto &RightContext = Contexts[Actions[Right].Context];
			if (LeftContext.Priority != RightContext.Priority) return LeftContext.Priority > RightContext.Priority;
			if (LeftContext.Id != RightContext.Id) return LeftContext.Id < RightContext.Id;
			const auto LeftPriority = DirtyPriorities[Left] == std::numeric_limits<std::int64_t>::min()
										  ? Actions[Left].PublicationPriority
										  : DirtyPriorities[Left];
			const auto RightPriority = DirtyPriorities[Right] == std::numeric_limits<std::int64_t>::min()
										   ? Actions[Right].PublicationPriority
										   : DirtyPriorities[Right];
			if (LeftPriority != RightPriority) return LeftPriority > RightPriority;
			return Actions[Left].Definition.Id < Actions[Right].Definition.Id;
		});
		for (std::size_t Offset = 0; Offset < Count; ++Offset) {
			const auto Index = Order[Offset];
			auto &Action = Actions[Index];
			double Sum = 0.0;
			for (auto Binding : Action.Bindings) {
				const auto &Row = Bindings[Binding];
				if (Row.Active && Row.Definition.InputKind != Kind::PointerDelta)
					Sum += Action.Definition.ValueType == Type::Bool ? 1.0 : static_cast<double>(Row.Definition.Scale);
			}
			const float Value = Action.Definition.ValueType == Type::Bool
									? (Sum != 0.0 ? 1.0f : 0.0f)
									: static_cast<float>(std::clamp(Sum, -1.0, 1.0));
			if (Action.Definition.ValueType == Type::Vector2)
				Publish(Action, Value, Action.State.X, Action.State.Y, DirtyReasons[Index]);
			else
				Publish(Action, Value, 0.0f, 0.0f, DirtyReasons[Index]);
		}
		DirtyActions.reset();
		DirtyCount = 0;
	}

	bool CompiledInputGraph::ProcessEvent(const HostEvent &Event, bool GuiConsumed) {
		ClearTransitions();
		Stats.EvaluatedBindings = 0;
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused) {
			Reset(Reason::FocusLost);
			return false;
		}
		std::size_t Code = BucketCount;
		bool Down = false;
		bool Repeat = false;
		if (const auto *Key = std::get_if<KeyEvent>(&Event)) {
			if (Key->Logical == LogicalKey::Unknown) return false;
			Code = static_cast<std::uint16_t>(Key->Logical);
			Down = Key->State == ButtonState::Pressed;
			Repeat = Key->Repeat;
		} else if (const auto *Button = std::get_if<GamepadButtonEvent>(&Event)) {
			auto KeyCode = InputObject::GetGamepadKeyCode(Button->Button);
			if (!KeyCode) return false;
			Code = static_cast<std::uint16_t>(*KeyCode);
			Down = Button->State == ButtonState::Pressed;
		} else if (const auto *Button = std::get_if<PointerButtonEvent>(&Event)) {
			if (Button->Button > PointerButton::Middle) return false;
			Code = MouseBase + static_cast<std::size_t>(Button->Button);
			Down = Button->State == ButtonState::Pressed;
		} else if (const auto *Move = std::get_if<PointerMoveEvent>(&Event)) {
			if (GuiConsumed || !std::isfinite(Move->Delta.X) || !std::isfinite(Move->Delta.Y)) return false;
			const bool Consumed = Evaluate(PointerCode, Reason::None);
			std::array<double, MaximumActions> X{}, Y{};
			std::bitset<MaximumActions> Touched;
			for (auto Index : Buckets[PointerCode]) {
				const auto &Row = Bindings[Index];
				if (!Row.Active) continue;
				Touched.set(Row.Action);
				X[Row.Action] += static_cast<double>(Move->Delta.X) * Row.Definition.VectorX;
				Y[Row.Action] += static_cast<double>(Move->Delta.Y) * Row.Definition.VectorY;
			}
			for (std::size_t Index = 0; Index < Actions.size(); ++Index) {
				if (!Touched[Index]) continue;
				auto &Action = Actions[Index];
				constexpr double Limit = std::numeric_limits<float>::max();
				Publish(
					Action,
					Action.State.Value,
					static_cast<float>(std::clamp(static_cast<double>(Action.State.X) + X[Index], -Limit, Limit)),
					static_cast<float>(std::clamp(static_cast<double>(Action.State.Y) + Y[Index], -Limit, Limit)),
					Reason::None
				);
			}
			DirtyActions.reset();
			DirtyCount = 0;
			return Consumed;
		}
		if (Code >= BucketCount) return false;
		const bool WasDown = PhysicalDown[Code];
		PhysicalDown.set(Code, Down);
		const auto Bit = Code < PhysicalCodeCount ? ModifierBit(static_cast<std::uint16_t>(Code)) : 0;
		if (Down)
			Modifiers = static_cast<std::uint8_t>(Modifiers | Bit);
		else
			Modifiers = static_cast<std::uint8_t>(Modifiers & ~Bit);
		for (auto Index : Buckets[Code]) {
			auto &Row = Bindings[Index];
			if (!Down || GuiConsumed)
				Row.Eligible = false;
			else if (!WasDown && !Repeat)
				Row.Eligible = Enabled(Row) && Chord(Row);
		}
		const auto Cancellation = GuiConsumed ? Reason::GuiConsumed : Reason::Released;
		bool Consumed = Evaluate(Code, Cancellation);
		if (Bit) {
			const auto ModifierIndex = static_cast<std::size_t>(std::countr_zero(static_cast<unsigned>(Bit)));
			for (auto DependentCode : ModifierBuckets[ModifierIndex]) {
				if (DependentCode == Code) continue;
				if (!PhysicalDown[DependentCode]) continue;
				if (GuiConsumed) {
					for (auto Index : Buckets[DependentCode])
						Bindings[Index].Eligible = false;
				}
				Consumed = Evaluate(DependentCode, Cancellation) || Consumed;
			}
		}
		RefreshActions();
		return !GuiConsumed && Consumed;
	}

	void CompiledInputGraph::Reset(Reason Cancellation) {
		ClearTransitions();
		PhysicalDown.reset();
		Modifiers = 0;
		for (auto &Row : Bindings) {
			Row.Eligible = Row.Matched = Row.Active = Row.Suppressed = false;
			Row.WinningContext = {};
		}
		for (auto &Action : Actions)
			Publish(Action, 0.0f, 0.0f, 0.0f, Cancellation);
		DirtyActions.reset();
		DirtyCount = 0;
	}

	void CompiledInputGraph::EndFrame() {
		ClearTransitions();
		for (auto &Action : Actions)
			if (Action.Definition.ValueType == Type::Vector2) Action.State.X = Action.State.Y = 0.0f;
		for (auto Index : Buckets[PointerCode]) {
			Bindings[Index].Active = Bindings[Index].Matched = false;
			Bindings[Index].Suppressed = false;
			Bindings[Index].WinningContext = {};
		}
	}

	std::span<const CompiledInputGraph::Transition> CompiledInputGraph::GetTransitions() const {
		return {Transitions.data(), TransitionCount};
	}

	void CompiledInputGraph::ClearTransitions() {
		TransitionCount = 0;
	}

	CompiledInputGraph::ActionState CompiledInputGraph::GetState(ObjectId Action) const {
		for (const auto &Row : Actions)
			if (Row.Definition.Id == Action) return Row.State;
		return {};
	}

	std::size_t CompiledInputGraph::MemoryBytes() const {
		std::size_t Bytes = sizeof(*this) + Contexts.capacity() * sizeof(ContextDefinition) +
							Actions.capacity() * sizeof(ActionRow) + Bindings.capacity() * sizeof(BindingRow);
		for (const auto &Row : Actions)
			Bytes += Row.Bindings.capacity() * sizeof(std::uint16_t);
		for (const auto &Values : Buckets)
			Bytes += Values.capacity() * sizeof(std::uint16_t);
		for (const auto &Values : ModifierBuckets)
			Bytes += Values.capacity() * sizeof(std::uint16_t);
		return Bytes;
	}

	CompiledInputGraph::Statistics CompiledInputGraph::GetStatistics() const {
		return Stats;
	}

	CompiledInputGraph::Snapshot CompiledInputGraph::Inspect() const {
		Snapshot Result{.Stats = GetStatistics(), .Modifiers = Modifiers, .Contexts = Contexts};
		for (std::size_t Code = 0; Code < PhysicalCodeCount; ++Code)
			if (PhysicalDown[Code]) Result.PressedKeys.push_back(static_cast<std::uint16_t>(Code));
		Result.Actions.reserve(Actions.size());
		Result.Bindings.reserve(Bindings.size());
		for (const auto &Row : Actions)
			Result.Actions.push_back({Row.Definition, Row.State});
		for (const auto &Row : Bindings)
			Result.Bindings.push_back({
				Row.Definition,
				Row.Matched,
				Row.Active,
				Row.Suppressed,
				Row.Eligible,
				Row.WinningContext,
			});
		return Result;
	}
}

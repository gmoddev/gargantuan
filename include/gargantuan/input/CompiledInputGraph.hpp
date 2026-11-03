#pragma once

#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace gargantuan {
	// Disposable local projection. No Instance pointers, callbacks, or authoritative mutations.
	class CompiledInputGraph {
	  public:
		static constexpr std::size_t MaximumContexts = 64;
		static constexpr std::size_t MaximumActions = 256;
		static constexpr std::size_t MaximumBindings = 2048;
		static constexpr std::size_t PhysicalCodeCount = 4096;
		enum class Kind : std::uint8_t { Key, MouseButton, PointerDelta };
		enum class Type : std::uint8_t { Bool, Scalar, Vector2 };
		enum class Reason : std::uint8_t {
			None,
			Released,
			FocusLost,
			DefinitionChanged,
			Disabled,
			Suppressed,
			GuiConsumed
		};
		struct ContextDefinition {
			ObjectId Id;
			int Priority = 0;
			bool Enabled = true;
			bool Consume = false;
			bool LegacyPassthrough = false;
			bool operator==(const ContextDefinition &) const = default;
		};
		struct ActionDefinition {
			ObjectId Id;
			ObjectId Context;
			Type ValueType = Type::Bool;
			bool Enabled = true;
			bool operator==(const ActionDefinition &) const = default;
		};
		struct BindingDefinition {
			ObjectId Id;
			ObjectId Action;
			Kind InputKind = Kind::Key;
			std::uint16_t Code = 0;
			std::uint8_t Modifiers = 0;
			bool ExactModifiers = false;
			bool Enabled = true;
			bool Consume = false;
			int Priority = 0;
			float Scale = 1.0f;
			float VectorX = 1.0f;
			float VectorY = 1.0f;
			bool operator==(const BindingDefinition &) const = default;
		};
		struct ActionState {
			bool Down = false;
			float Value = 0.0f;
			float X = 0.0f;
			float Y = 0.0f;
			Reason LastReason = Reason::None;
			std::uint64_t LastTransition = 0;
		};
		struct Transition {
			ObjectId Action;
			bool Down = false;
			float Value = 0.0f;
			float X = 0.0f;
			float Y = 0.0f;
			Reason Cancellation = Reason::None;
			bool VectorChanged = false;
		};
		struct ActionDiagnostic {
			ActionDefinition Definition;
			ActionState State;
		};
		struct BindingDiagnostic {
			BindingDefinition Definition;
			bool Matched = false;
			bool Active = false;
			bool Suppressed = false;
			bool Eligible = false;
			ObjectId WinningContext;
		};
		struct Statistics {
			std::uint64_t Revision = 0;
			std::size_t ChangedBindings = 0;
			std::size_t ReindexedBuckets = 0;
			std::size_t EvaluatedBindings = 0;
			std::size_t MemoryBytes = 0;
		};
		struct Snapshot {
			Statistics Stats;
			std::uint8_t Modifiers = 0;
			std::vector<std::uint16_t> PressedKeys;
			std::vector<ContextDefinition> Contexts;
			std::vector<ActionDiagnostic> Actions;
			std::vector<BindingDiagnostic> Bindings;
		};

		CompiledInputGraph();
		// Each operation replaces the bounded transition batch. Publish it before the next operation.
		bool Compile(
			std::span<const ContextDefinition> Contexts,
			std::span<const ActionDefinition> Actions,
			std::span<const BindingDefinition> Bindings
		);
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event, bool GuiConsumed = false);
		void Reset(Reason Cancellation = Reason::FocusLost);
		void EndFrame();
		[[nodiscard]] std::span<const Transition> GetTransitions() const;
		void ClearTransitions();
		[[nodiscard]] ActionState GetState(ObjectId Action) const;
		[[nodiscard]] Snapshot Inspect() const;
		[[nodiscard]] Statistics GetStatistics() const;
		[[nodiscard]] static std::uint8_t ModifierBit(std::uint16_t LogicalCode);

	  private:
		static constexpr std::size_t MouseBase = PhysicalCodeCount;
		static constexpr std::size_t PointerCode = MouseBase + 3;
		static constexpr std::size_t BucketCount = PointerCode + 1;
		struct ActionRow {
			ActionDefinition Definition;
			std::uint16_t Context = 0;
			int PublicationPriority = 0;
			ActionState State;
			std::vector<std::uint16_t> Bindings;
		};
		struct BindingRow {
			BindingDefinition Definition;
			std::uint16_t Action = 0;
			bool Eligible = false;
			bool Matched = false;
			bool Active = false;
			bool Suppressed = false;
			ObjectId WinningContext;
		};
		std::vector<ContextDefinition> Contexts;
		std::vector<ActionRow> Actions;
		std::vector<BindingRow> Bindings;
		std::array<std::vector<std::uint16_t>, BucketCount> Buckets;
		std::array<std::vector<std::uint16_t>, 8> ModifierBuckets;
		std::bitset<BucketCount> PhysicalDown;
		std::bitset<MaximumActions> DirtyActions;
		std::array<std::uint16_t, MaximumActions> DirtyOrder{};
		std::size_t DirtyCount = 0;
		std::array<Reason, MaximumActions> DirtyReasons{};
		std::array<std::int64_t, MaximumActions> DirtyPriorities{};
		std::array<Transition, MaximumActions * 2> Transitions{};
		std::size_t TransitionCount = 0;
		std::uint8_t Modifiers = 0;
		std::uint64_t TransitionSequence = 0;
		Statistics Stats;

		[[nodiscard]] static std::size_t Bucket(const BindingDefinition &Definition);
		[[nodiscard]] bool Higher(std::uint16_t Left, std::uint16_t Right) const;
		[[nodiscard]] bool Chord(const BindingRow &Binding) const;
		[[nodiscard]] bool Enabled(const BindingRow &Binding) const;
		void Dirty(
			std::uint16_t Action, Reason Cancellation, std::int64_t Priority = std::numeric_limits<std::int64_t>::min()
		);
		bool Evaluate(std::size_t Code, Reason Cancellation);
		void RefreshActions();
		void Publish(ActionRow &Action, float Value, float X, float Y, Reason Cancellation);
		[[nodiscard]] std::size_t MemoryBytes() const;
	};
}

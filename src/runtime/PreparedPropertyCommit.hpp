#pragma once

// Internal synchronous authoring primitive, also used by EditorHost's bounded adapter.
#include "gargantuan/runtime/MutationGateway.hpp"
#include <span>

namespace gargantuan {
	class InstanceProperty;
	inline constexpr std::size_t MaximumPreparedPropertyWrites = 256;
	inline constexpr std::size_t MaximumPreparedRequestBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumPreparedValueBytes = 4 * 1024 * 1024;
	inline constexpr std::size_t MaximumPreparedJournalBytes = 2 * 1024 * 1024;

	struct PreparedPropertyWrite {
		ObjectId Object;
		SchemaId DeclaringSchemaId;
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Value;
	};

	enum class PreparedPropertyStage {
		AggregateReservation, PropertyValue, JournalEncoding, Notification,
		HistoryAction, HistoryRetention, FinalValidation, JournalReservation
	};

	struct PreparedPropertyResult {
		MutationStatus Status = MutationStatus::Rejected;
		TransactionId HistoryId;
		std::uint64_t StartingRevision = 0;
		std::uint64_t ResultingRevision = 0;
		std::size_t ChangedWrites = 0;
		std::size_t NotificationFailures = 0;
		std::size_t ValueBytes = 0;
		std::size_t JournalBytes = 0;
		std::size_t HistoryBytes = 0;
	};

	class PreparedPropertyCommit {
	  public:
		// Discovery only; Apply always revalidates authority, identity, and values.
		static bool SupportsProperty(const InstanceProperty &Property) noexcept;
		static PreparedPropertyResult Apply(DataModel &World, std::uint64_t ExpectedRevision,
			std::span<const PreparedPropertyWrite> Writes, const ScriptSecurityContext &Security);
		static TransactionResult Replay(DataModel &World, const ScriptSecurityContext &Security, bool Redo);

		// Qualification seams are thread-local, native-only, and never request data.
		// Stage hooks may throw before the boundary. Boundary hooks may only toggle
		// instrumentation and are noexcept; no observer callback uses these hooks.
		struct QualificationHooks {
			void (*Stage)(PreparedPropertyStage, std::size_t) = nullptr;
			void (*Boundary)(bool) noexcept = nullptr;
		};
		static void SetQualificationHooks(QualificationHooks Hooks) noexcept;

	  private:
		static MutationStatus PreparePropertyWireMutation(Instance &Target, const InstanceProperty &Property,
			const PreparedPropertyWrite &Write, const ScriptSecurityContext &Security, std::any &Native, WireValue &After);
		static PreparedPropertyResult Execute(DataModel &World, std::uint64_t ExpectedRevision,
			std::span<const PreparedPropertyWrite> Writes, const ScriptSecurityContext &Security,
			const CommittedTransaction *ReplayAction, bool Redo);
	};
}

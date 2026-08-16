#pragma once

#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gargantuan {
	class DataModel;

	inline constexpr std::size_t MaximumOpenAuthoringTransactions = 1;
	inline constexpr std::size_t MaximumTransactionLabelBytes = 128;
	inline constexpr std::size_t MaximumTransactionChanges = 4096;
	inline constexpr std::size_t MaximumTransactionSemanticBytes = 8 * 1024 * 1024;
	inline constexpr std::size_t MaximumTransactionSubtreeBytes = 4 * 1024 * 1024;
	inline constexpr std::size_t MaximumRetainedTransactions = 128;
	inline constexpr std::size_t MaximumRetainedTransactionBytes = 32 * 1024 * 1024;
	inline constexpr auto MaximumOpenTransactionLifetime = std::chrono::minutes(5);

	struct TransactionId {
		std::uint64_t Value = 0;
		[[nodiscard]] bool IsValid() const {
			return Value != 0;
		}
		auto operator<=>(const TransactionId &) const = default;
	};

	enum class TransactionOrigin { Studio, InternalAuthoring };
	enum class TransactionState { Open, Committed, Aborted };

	struct PropertyTransactionChange {
		ObjectId Object;
		SchemaId DeclaringSchemaId{};
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Before;
		WireValue After;
	};

	struct AttributeTransactionChange {
		ObjectId Object;
		std::string AttributeName;
		std::optional<WireValue> Before;
		std::optional<WireValue> After;
	};

	struct TagTransactionChange {
		ObjectId Object;
		std::string TagName;
		bool Before = false;
		bool After = false;
	};

	struct ExtensionTransactionChange {
		ObjectId Object;
		SchemaId ExtensionSchemaId{};
		std::uint32_t DefinitionVersion = 0;
		std::string PropertyName;
		WireValue Before;
		WireValue After;
	};

	struct ReparentTransactionChange {
		ObjectId Object;
		std::optional<ObjectId> BeforeParent;
		std::optional<ObjectId> AfterParent;
	};

	enum class SubtreeTransactionKind { Create, Destroy, Duplicate };
	struct SubtreeTransactionChange {
		SubtreeTransactionKind Kind = SubtreeTransactionKind::Create;
		ObjectId Root;
		std::optional<ObjectId> Parent;
		std::vector<ObjectId> Objects;
		std::string PersistentSnapshot;
	};

	using TransactionChange = std::variant<
		PropertyTransactionChange,
		AttributeTransactionChange,
		TagTransactionChange,
		ExtensionTransactionChange,
		ReparentTransactionChange,
		SubtreeTransactionChange>;

	struct CommittedTransaction {
		TransactionId Id;
		std::string Label;
		TransactionOrigin Origin = TransactionOrigin::Studio;
		std::uint64_t StartingRevision = 0;
		std::uint64_t ResultingRevision = 0;
		std::vector<TransactionChange> Changes;
		std::size_t SemanticBytes = 0;
		TransactionState State = TransactionState::Committed;
	};

	enum class TransactionStatus {
		Success,
		NoChanges,
		NotFound,
		WrongOwner,
		LimitExceeded,
		RevisionExhausted,
		InvalidState,
	};

	struct TransactionResult {
		TransactionStatus Status = TransactionStatus::InvalidState;
		TransactionId Id;
		std::uint64_t StartingRevision = 0;
		std::uint64_t ResultingRevision = 0;
		std::size_t ChangeCount = 0;
		std::string Message;
		[[nodiscard]] bool Succeeded() const {
			return Status == TransactionStatus::Success || Status == TransactionStatus::NoChanges;
		}
	};

	[[nodiscard]] std::size_t EstimateTransactionChangeBytes(const TransactionChange &Change);

	class AuthoritativeTransactionHistory {
	  public:
		explicit AuthoritativeTransactionHistory(
			std::size_t RetainedTransactionLimitValue = MaximumRetainedTransactions,
			std::size_t RetainedByteLimitValue = MaximumRetainedTransactionBytes
		);
		TransactionResult Begin(
			DataModel &World,
			std::uint64_t Owner,
			std::string Label,
			TransactionOrigin Origin = TransactionOrigin::Studio
		);
		TransactionResult BeginImplicit(
			DataModel &World,
			std::uint64_t Owner,
			std::string Label,
			TransactionOrigin Origin = TransactionOrigin::Studio
		);
		[[nodiscard]] TransactionResult ValidateMutation(
			TransactionId Id, std::uint64_t Owner, std::size_t AdditionalBytes, std::size_t AdditionalChanges = 1
		) const;
		void RecordMutation(
			TransactionId Id,
			std::uint64_t Owner,
			TransactionChange Change,
			std::vector<BufferedChangeRecord> JournalRecords
		);
		TransactionResult Commit(DataModel &World, TransactionId Id, std::uint64_t Owner);
		TransactionResult TerminateOwner(DataModel &World, std::uint64_t Owner);
		TransactionResult ExpireOwner(
			DataModel &World,
			std::uint64_t Owner,
			std::chrono::steady_clock::time_point Now = std::chrono::steady_clock::now()
		);
		void Reset();

		[[nodiscard]] bool IsOpen(TransactionId Id, std::uint64_t Owner) const;
		[[nodiscard]] std::size_t GetOpenCount() const {
			return Open ? 1 : 0;
		}
		[[nodiscard]] const std::deque<CommittedTransaction> &GetCommitted() const {
			return History;
		}
		[[nodiscard]] std::size_t GetRetainedBytes() const {
			return RetainedBytes;
		}

	  private:
		struct OpenTransaction {
			TransactionId Id;
			std::uint64_t Owner = 0;
			std::string Label;
			TransactionOrigin Origin = TransactionOrigin::Studio;
			std::uint64_t StartingRevision = 0;
			std::vector<TransactionChange> Changes;
			std::vector<BufferedChangeRecord> JournalRecords;
			std::size_t SemanticBytes = 0;
			bool Implicit = false;
			std::chrono::steady_clock::time_point OpenedAt;
		};

		TransactionResult BeginInternal(
			DataModel &World, std::uint64_t Owner, std::string Label, TransactionOrigin Origin, bool Implicit
		);
		void Retain(CommittedTransaction Transaction);

		std::optional<OpenTransaction> Open;
		std::deque<CommittedTransaction> History;
		std::size_t RetainedBytes = 0;
		std::size_t RetainedTransactionLimit;
		std::size_t RetainedByteLimit;
	};
}

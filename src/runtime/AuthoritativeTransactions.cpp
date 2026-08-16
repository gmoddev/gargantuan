#include "gargantuan/runtime/AuthoritativeTransactions.hpp"

#include "gargantuan/classes/DataModel.hpp"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace gargantuan {
	namespace {
		std::atomic<std::uint64_t> NextProcessTransactionId{1};

		TransactionId AllocateTransactionId() {
			auto Candidate = NextProcessTransactionId.load(std::memory_order_relaxed);
			for (;;) {
				if (Candidate == 0 || Candidate == std::numeric_limits<std::uint64_t>::max()) return {};
				if (NextProcessTransactionId.compare_exchange_weak(
						Candidate, Candidate + 1, std::memory_order_relaxed, std::memory_order_relaxed
					))
					return TransactionId{Candidate};
			}
		}
		std::size_t WireValueBytes(const WireValue &Value) {
			return std::visit(
				[](const auto &Typed) -> std::size_t {
					using ValueType = std::decay_t<decltype(Typed)>;
					if constexpr (std::is_same_v<ValueType, std::string>)
						return sizeof(ValueType) + Typed.size();
					else if constexpr (std::is_same_v<ValueType, WireEnumItem>)
						return sizeof(ValueType) + Typed.EnumType.size() + Typed.Item.size();
					else
						return sizeof(ValueType);
				},
				Value
			);
		}

		TransactionResult Failure(TransactionStatus Status, TransactionId Id, std::string Message) {
			return {.Status = Status, .Id = Id, .Message = std::move(Message)};
		}
	}

	std::size_t EstimateTransactionChangeBytes(const TransactionChange &Change) {
		return std::visit(
			[](const auto &Typed) -> std::size_t {
				using ChangeType = std::decay_t<decltype(Typed)>;
				if constexpr (std::is_same_v<ChangeType, PropertyTransactionChange>)
					return sizeof(Typed) + Typed.PropertyName.size() + WireValueBytes(Typed.Before) +
						   WireValueBytes(Typed.After);
				else if constexpr (std::is_same_v<ChangeType, AttributeTransactionChange>)
					return sizeof(Typed) + Typed.AttributeName.size() +
						   (Typed.Before ? WireValueBytes(*Typed.Before) : 0) +
						   (Typed.After ? WireValueBytes(*Typed.After) : 0);
				else if constexpr (std::is_same_v<ChangeType, TagTransactionChange>)
					return sizeof(Typed) + Typed.TagName.size();
				else if constexpr (std::is_same_v<ChangeType, ExtensionTransactionChange>)
					return sizeof(Typed) + Typed.PropertyName.size() + WireValueBytes(Typed.Before) +
						   WireValueBytes(Typed.After);
				else if constexpr (std::is_same_v<ChangeType, ReparentTransactionChange>)
					return sizeof(Typed);
				else
					return sizeof(Typed) + Typed.Objects.size() * sizeof(ObjectId) + Typed.PersistentSnapshot.size();
			},
			Change
		);
	}

	AuthoritativeTransactionHistory::AuthoritativeTransactionHistory(
		std::size_t RetainedTransactionLimitValue, std::size_t RetainedByteLimitValue
	)
		: RetainedTransactionLimit(RetainedTransactionLimitValue), RetainedByteLimit(RetainedByteLimitValue) {
		if (RetainedTransactionLimit == 0 || RetainedByteLimit == 0)
			throw std::invalid_argument("Transaction history limits must be nonzero");
	}

	TransactionResult AuthoritativeTransactionHistory::BeginInternal(
		DataModel &World, std::uint64_t Owner, std::string Label, TransactionOrigin Origin, bool Implicit
	) {
		if (Owner == 0) return Failure(TransactionStatus::WrongOwner, {}, "Transaction owner is invalid");
		if (Label.empty() || Label.size() > MaximumTransactionLabelBytes)
			return Failure(
				TransactionStatus::LimitExceeded, {}, "Transaction label is empty or exceeds its byte limit"
			);
		if (Open)
			return Failure(TransactionStatus::LimitExceeded, Open->Id, "The project already has an open transaction");
		World.EnsureAuthoritativeRevisionAvailable();
		const auto Id = AllocateTransactionId();
		if (!Id.IsValid()) return Failure(TransactionStatus::LimitExceeded, {}, "Transaction identity is exhausted");
		Open.emplace(
			OpenTransaction{
				.Id = Id,
				.Owner = Owner,
				.Label = std::move(Label),
				.Origin = Origin,
				.StartingRevision = World.GetAuthoritativeRevision(),
				.Implicit = Implicit,
				.OpenedAt = std::chrono::steady_clock::now(),
			}
		);
		return {
			.Status = TransactionStatus::Success,
			.Id = Id,
			.StartingRevision = Open->StartingRevision,
			.ResultingRevision = Open->StartingRevision,
		};
	}

	TransactionResult AuthoritativeTransactionHistory::Begin(
		DataModel &World, std::uint64_t Owner, std::string Label, TransactionOrigin Origin
	) {
		return BeginInternal(World, Owner, std::move(Label), Origin, false);
	}

	TransactionResult AuthoritativeTransactionHistory::BeginImplicit(
		DataModel &World, std::uint64_t Owner, std::string Label, TransactionOrigin Origin
	) {
		return BeginInternal(World, Owner, std::move(Label), Origin, true);
	}

	TransactionResult AuthoritativeTransactionHistory::ValidateMutation(
		TransactionId Id, std::uint64_t Owner, std::size_t AdditionalBytes, std::size_t AdditionalChanges
	) const {
		if (!Open || Open->Id != Id) return Failure(TransactionStatus::NotFound, Id, "Transaction is not open");
		if (Open->Owner != Owner)
			return Failure(TransactionStatus::WrongOwner, Id, "Transaction belongs to another session");
		if (AdditionalChanges > MaximumTransactionChanges - Open->Changes.size() ||
			AdditionalBytes > MaximumTransactionSemanticBytes - Open->SemanticBytes)
			return Failure(
				TransactionStatus::LimitExceeded, Id, "Transaction semantic history limit would be exceeded"
			);
		return {
			.Status = TransactionStatus::Success,
			.Id = Id,
			.StartingRevision = Open->StartingRevision,
			.ResultingRevision = Open->StartingRevision,
			.ChangeCount = Open->Changes.size(),
		};
	}

	void AuthoritativeTransactionHistory::RecordMutation(
		TransactionId Id,
		std::uint64_t Owner,
		TransactionChange Change,
		std::vector<BufferedChangeRecord> JournalRecords
	) {
		const auto Bytes = EstimateTransactionChangeBytes(Change);
		auto Validation = ValidateMutation(Id, Owner, Bytes);
		if (!Validation.Succeeded()) throw std::logic_error(Validation.Message);
		Open->SemanticBytes += Bytes;
		Open->Changes.push_back(std::move(Change));
		Open->JournalRecords.insert(
			Open->JournalRecords.end(),
			std::make_move_iterator(JournalRecords.begin()),
			std::make_move_iterator(JournalRecords.end())
		);
	}

	void AuthoritativeTransactionHistory::Retain(CommittedTransaction Transaction) {
		bool PruneMappings = false;
		while (History.size() > Cursor) {
			RetainedBytes -= History.back().SemanticBytes;
			History.pop_back();
			PruneMappings = true;
		}
		while (!History.empty() && (History.size() >= RetainedTransactionLimit ||
									Transaction.SemanticBytes > RetainedByteLimit - RetainedBytes)) {
			RetainedBytes -= History.front().SemanticBytes;
			History.pop_front();
			if (Cursor != 0) --Cursor;
			PruneMappings = true;
		}
		RetainedBytes += Transaction.SemanticBytes;
		History.push_back(std::move(Transaction));
		Cursor = History.size();
		if (PruneMappings && !IdentityMappings.empty()) {
			std::unordered_set<ObjectId> Referenced;
			auto IncludeWireReference = [&](const WireValue &Value) {
				if (const auto *Reference = std::get_if<WireObjectReference>(&Value))
					Referenced.insert(Reference->Object.ToObjectId());
			};
			for (const auto &Retained : History)
				for (const auto &Change : Retained.Changes)
					std::visit([&](const auto &Typed) {
						using ChangeType = std::decay_t<decltype(Typed)>;
						if constexpr (std::is_same_v<ChangeType, PropertyTransactionChange>) {
							Referenced.insert(Typed.Object); IncludeWireReference(Typed.Before); IncludeWireReference(Typed.After);
						} else if constexpr (std::is_same_v<ChangeType, AttributeTransactionChange>) {
							Referenced.insert(Typed.Object);
							if (Typed.Before) IncludeWireReference(*Typed.Before);
							if (Typed.After) IncludeWireReference(*Typed.After);
						} else if constexpr (std::is_same_v<ChangeType, TagTransactionChange> ||
							std::is_same_v<ChangeType, ExtensionTransactionChange>) {
							Referenced.insert(Typed.Object);
							if constexpr (std::is_same_v<ChangeType, ExtensionTransactionChange>) {
								IncludeWireReference(Typed.Before); IncludeWireReference(Typed.After);
							}
						} else if constexpr (std::is_same_v<ChangeType, ReparentTransactionChange>) {
							Referenced.insert(Typed.Object);
							if (Typed.BeforeParent) Referenced.insert(*Typed.BeforeParent);
							if (Typed.AfterParent) Referenced.insert(*Typed.AfterParent);
						} else {
							Referenced.insert(Typed.Root);
							if (Typed.Parent) Referenced.insert(*Typed.Parent);
							Referenced.insert(Typed.Objects.begin(), Typed.Objects.end());
						}
					}, Change);
			std::erase_if(IdentityMappings, [&](const auto &Entry) { return !Referenced.contains(Entry.first); });
		}
	}

	TransactionHistoryStatus AuthoritativeTransactionHistory::GetStatus() const {
		TransactionHistoryStatus Status{
			.CanUndo = Cursor != 0,
			.CanRedo = Cursor < History.size(),
			.RetainedCount = History.size(),
			.Cursor = Cursor,
			.SemanticBytes = RetainedBytes,
		};
		if (Status.CanUndo) {
			Status.UndoTransaction = History[Cursor - 1].Id;
			Status.UndoLabel = History[Cursor - 1].Label;
		}
		if (Status.CanRedo) {
			Status.RedoTransaction = History[Cursor].Id;
			Status.RedoLabel = History[Cursor].Label;
		}
		return Status;
	}

	const CommittedTransaction *AuthoritativeTransactionHistory::GetUndoTransaction() const {
		return Cursor == 0 ? nullptr : &History[Cursor - 1];
	}

	const CommittedTransaction *AuthoritativeTransactionHistory::GetRedoTransaction() const {
		return Cursor >= History.size() ? nullptr : &History[Cursor];
	}

	ObjectId AuthoritativeTransactionHistory::ResolveIdentity(ObjectId Historical) const {
		auto Current = Historical;
		for (std::size_t Depth = 0; Depth < IdentityMappings.size(); ++Depth) {
			auto Found = std::find_if(IdentityMappings.rbegin(), IdentityMappings.rend(), [&](const auto &Entry) {
				return Entry.first == Current;
			});
			if (Found == IdentityMappings.rend() || Found->second == Current) break;
			Current = Found->second;
		}
		return Current;
	}

	void AuthoritativeTransactionHistory::RemapIdentity(ObjectId Historical, ObjectId Current) {
		std::vector<ObjectId> Aliases;
		for (const auto &Entry : IdentityMappings)
			if (ResolveIdentity(Entry.first) == Historical) Aliases.push_back(Entry.first);
		for (auto Alias : Aliases) {
			auto Entry = std::find_if(IdentityMappings.begin(), IdentityMappings.end(),
				[&](const auto &Candidate) { return Candidate.first == Alias; });
			if (Entry != IdentityMappings.end()) Entry->second = Current;
		}
		auto Found = std::find_if(IdentityMappings.begin(), IdentityMappings.end(), [&](const auto &Entry) {
			return Entry.first == Historical;
		});
		if (Found == IdentityMappings.end()) IdentityMappings.emplace_back(Historical, Current);
		else Found->second = Current;
	}

	TransactionResult AuthoritativeTransactionHistory::CompleteUndo(DataModel &World) {
		if (Open) return Failure(TransactionStatus::InvalidState, {}, "Undo is unavailable while a transaction is open");
		if (Cursor == 0) return Failure(TransactionStatus::NothingToUndo, {}, "There is nothing to undo");
		World.EnsureAuthoritativeRevisionAvailable();
		const auto &Transaction = History[Cursor - 1];
		const auto StartingRevision = World.GetAuthoritativeRevision();
		World.AdvanceAuthoritativeRevision();
		--Cursor;
		return {.Status = TransactionStatus::Success, .Id = Transaction.Id,
			.StartingRevision = StartingRevision, .ResultingRevision = World.GetAuthoritativeRevision(),
			.ChangeCount = Transaction.Changes.size()};
	}

	TransactionResult AuthoritativeTransactionHistory::CompleteRedo(DataModel &World) {
		if (Open) return Failure(TransactionStatus::InvalidState, {}, "Redo is unavailable while a transaction is open");
		if (Cursor >= History.size()) return Failure(TransactionStatus::NothingToRedo, {}, "There is nothing to redo");
		World.EnsureAuthoritativeRevisionAvailable();
		const auto &Transaction = History[Cursor];
		const auto StartingRevision = World.GetAuthoritativeRevision();
		World.AdvanceAuthoritativeRevision();
		++Cursor;
		return {.Status = TransactionStatus::Success, .Id = Transaction.Id,
			.StartingRevision = StartingRevision, .ResultingRevision = World.GetAuthoritativeRevision(),
			.ChangeCount = Transaction.Changes.size()};
	}

	TransactionResult AuthoritativeTransactionHistory::Commit(DataModel &World, TransactionId Id, std::uint64_t Owner) {
		if (!Open || Open->Id != Id) return Failure(TransactionStatus::NotFound, Id, "Transaction is not open");
		if (Open->Owner != Owner)
			return Failure(TransactionStatus::WrongOwner, Id, "Transaction belongs to another session");
		if (Open->Changes.empty()) {
			const auto StartingRevision = Open->StartingRevision;
			Open.reset();
			return {
				.Status = TransactionStatus::NoChanges,
				.Id = Id,
				.StartingRevision = StartingRevision,
				.ResultingRevision = World.GetAuthoritativeRevision(),
			};
		}
		try {
			World.EnsureAuthoritativeRevisionAvailable();
			std::vector<std::pair<ObjectId, std::size_t>> ScopeCounts;
			for (const auto &Record : Open->JournalRecords) {
				auto Existing = std::find_if(ScopeCounts.begin(), ScopeCounts.end(), [&](const auto &Entry) {
					return Entry.first == Record.Scope;
				});
				if (Existing == ScopeCounts.end())
					ScopeCounts.emplace_back(Record.Scope, 1);
				else
					++Existing->second;
			}
			for (const auto &[Scope, Count] : ScopeCounts)
				ChangeJournal::Get().EnsureCanCommit(Scope, Count);
		} catch (const std::overflow_error &Error) {
			return Failure(TransactionStatus::RevisionExhausted, Id, Error.what());
		}

		World.AdvanceAuthoritativeRevision();
		const auto ResultingRevision = World.GetAuthoritativeRevision();
		for (std::size_t Index = 0; Index < Open->JournalRecords.size();) {
			const auto Scope = Open->JournalRecords[Index].Scope;
			std::vector<std::pair<ObjectId, ChangePayload>> Changes;
			while (Index < Open->JournalRecords.size() && Open->JournalRecords[Index].Scope == Scope) {
				Changes.emplace_back(
					Open->JournalRecords[Index].Object, std::move(Open->JournalRecords[Index].Payload)
				);
				++Index;
			}
			ChangeJournal::Get().CommitBatch(Scope, std::move(Changes));
		}

		const auto StartingRevision = Open->StartingRevision;
		const auto ChangeCount = Open->Changes.size();
		Retain(
			CommittedTransaction{
				.Id = Id,
				.Label = std::move(Open->Label),
				.Origin = Open->Origin,
				.StartingRevision = StartingRevision,
				.ResultingRevision = ResultingRevision,
				.Changes = std::move(Open->Changes),
				.SemanticBytes = Open->SemanticBytes,
			}
		);
		Open.reset();
		return {
			.Status = TransactionStatus::Success,
			.Id = Id,
			.StartingRevision = StartingRevision,
			.ResultingRevision = ResultingRevision,
			.ChangeCount = ChangeCount,
		};
	}

	TransactionResult AuthoritativeTransactionHistory::TerminateOwner(DataModel &World, std::uint64_t Owner) {
		if (!Open || Open->Owner != Owner)
			return Failure(TransactionStatus::NotFound, {}, "Session has no open transaction");
		return Commit(World, Open->Id, Owner);
	}

	TransactionResult AuthoritativeTransactionHistory::ExpireOwner(
		DataModel &World, std::uint64_t Owner, std::chrono::steady_clock::time_point Now
	) {
		if (!Open || Open->Owner != Owner)
			return Failure(TransactionStatus::NotFound, {}, "Session has no open transaction");
		if (Now - Open->OpenedAt < MaximumOpenTransactionLifetime)
			return {
				.Status = TransactionStatus::Success,
				.Id = Open->Id,
				.StartingRevision = Open->StartingRevision,
				.ResultingRevision = World.GetAuthoritativeRevision(),
				.ChangeCount = Open->Changes.size(),
			};
		return Commit(World, Open->Id, Owner);
	}

	bool AuthoritativeTransactionHistory::IsOpen(TransactionId Id, std::uint64_t Owner) const {
		return Open && Open->Id == Id && Open->Owner == Owner;
	}

	void AuthoritativeTransactionHistory::Reset() {
		Open.reset();
		History.clear();
		RetainedBytes = 0;
		Cursor = 0;
		IdentityMappings.clear();
	}
}

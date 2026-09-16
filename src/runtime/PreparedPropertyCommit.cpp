#include "PreparedPropertyCommit.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		thread_local PreparedPropertyCommit::QualificationHooks Hooks;
		thread_local bool Preparing = false;
		void Stage(PreparedPropertyStage Value, std::size_t Index = 0) {
			if (Hooks.Stage) Hooks.Stage(Value, Index);
		}
		void Account(std::size_t &Total, std::size_t Bytes, std::size_t Maximum) {
			if (Bytes > Maximum - Total) throw std::length_error("Prepared property resource limit");
			Total += Bytes;
		}
		std::size_t EscapedBytes(std::string_view Text) noexcept {
			std::size_t Bytes = 0;
			for (const unsigned char Character : Text) {
				if (Character == '"' || Character == '\\' || Character == '\b' || Character == '\f' ||
					Character == '\n' || Character == '\r' || Character == '\t') Bytes += 2;
				else Bytes += Character < 0x20 ? 6 : 1;
			}
			return Bytes;
		}
		std::size_t PreparedWireBytes(const WireValue &Value) noexcept {
			// No temporary JSON tree: its destructor itself allocates during OOM
			// unwinding. 512 covers the largest fixed WireValue (12 finite numeric
			// components at <=32 JSON bytes each plus its wrapper). Dynamic fields
			// are separately charged with the canonical JSON escaping rules.
			return 512 + std::visit([](const auto &Typed) noexcept -> std::size_t {
				using Type = std::decay_t<decltype(Typed)>;
				if constexpr (std::is_same_v<Type, std::string>) return EscapedBytes(Typed);
				else if constexpr (std::is_same_v<Type, WireEnumItem>) return EscapedBytes(Typed.EnumType) + EscapedBytes(Typed.Item);
				else return 0;
			}, Value);
		}
	}

	void PreparedPropertyCommit::SetQualificationHooks(QualificationHooks Value) noexcept { Hooks = Value; }

	MutationStatus PreparedPropertyCommit::PreparePropertyWireMutation(Instance &Target, const InstanceProperty &Property,
		const PreparedPropertyWrite &Write, const ScriptSecurityContext &Security, std::any &Native, WireValue &After) {
		// Shared by forward and replay; retain the ordinary wire path's canonical
		// permissions, domain and enum rules and its native decoder/validators.
		const auto Validation = Target.ValidatePropertyWireMutation(Write.PropertyName, Write.Value,
			Enums::Permission::LocalUser, Security);
		if (Validation != MutationStatus::Success) return Validation;
		if (const auto *Enum = std::get_if<WireEnumItem>(&Write.Value)) {
			if (!Property.PrepareEnum) return MutationStatus::ValidationFailed;
			const auto Item = Enums::GetEnums().at(Enum->EnumType)->FromName(Enum->Item);
			Native = Property.PrepareEnum(Item->Value);
			After = Write.Value;
		} else {
			auto Decoded = DecodeNativeWireValue(Write.Value);
			if (!Decoded) return MutationStatus::ValidationFailed;
			Native = std::move(*Decoded);
			auto Encoded = EncodeNativeWireValue(Native);
			if (!Encoded) return MutationStatus::ValidationFailed;
			After = std::move(*Encoded);
		}
		return Property.MatchesPreparedType(Native) && Property.IsValueValid(Native)
			? MutationStatus::Success : MutationStatus::ValidationFailed;
	}

	PreparedPropertyResult PreparedPropertyCommit::Apply(DataModel &World, std::uint64_t ExpectedRevision,
		std::span<const PreparedPropertyWrite> Writes, const ScriptSecurityContext &Security) {
		return Execute(World, ExpectedRevision, Writes, Security, nullptr, false);
	}

	PreparedPropertyResult PreparedPropertyCommit::Execute(DataModel &World, std::uint64_t ExpectedRevision,
		std::span<const PreparedPropertyWrite> Writes, const ScriptSecurityContext &Security,
		const CommittedTransaction *ReplayAction, bool Redo) {
		PreparedPropertyResult Result;
		Result.StartingRevision = Result.ResultingRevision = ExpectedRevision;
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) {
			Result.Status = MutationStatus::WrongExecutionDomain;
			return Result;
		}
		if (!Security.HasCapability(ScriptCapability::MutateDataModel)) {
			Result.Status = MutationStatus::Unauthorized;
			return Result;
		}
		if (Preparing || World.Transactions.GetOpenCount() || World.HasDeferredRevision()) return Result;
		if (Writes.empty() || Writes.size() > MaximumPreparedPropertyWrites) {
			Result.Status = MutationStatus::ResourceLimit;
			return Result;
		}
		if (World.GetAuthoritativeRevision() != ExpectedRevision) {
			Result.Status = MutationStatus::Conflict;
			return Result;
		}
		if (ExpectedRevision == std::numeric_limits<std::uint64_t>::max()) {
			Result.Status = MutationStatus::RevisionExhausted;
			return Result;
		}
		Preparing = true;
		struct Guard { ~Guard() { Preparing = false; } } GuardValue;
		try {
			const auto &Lifecycle = GetRuntimeSchemaLifecycle();
			const auto Generation = Lifecycle.GetActiveGeneration();
			auto Registry = Lifecycle.GetActiveRegistry();
			if (!Registry || !Registry->IsFrozen() || World.GetDestroyed() || World.IsDestroying()) return Result;
			const auto Scope = World.GetObjectId();
			struct Store {
				std::shared_ptr<Instance> Target;
				const InstanceClassDefinition *Class;
				const InstanceProperty *Property;
				const PreparedPropertyWrite *Request;
				std::any Native;
				std::shared_ptr<Signal<std::monostate>> Notification;
			};
			std::vector<Store> Stores;
			std::vector<BufferedChangeRecord> Records;
			CommittedTransaction Action;
			std::size_t RequestBytes = 128;
			for (std::size_t Index = 0; Index < Writes.size(); ++Index) {
				const auto &Write = Writes[Index];
				ValidateProtocolString(Write.PropertyName, MaximumProtocolIdentifierBytes, "Property");
				ValidateProtocolWireValue(Write.Value);
				// Conservative escaped identifier/metadata allowance for the eventual
				// envelope, without exposing or trusting a caller-supplied byte count.
				if (!ReplayAction) Account(RequestBytes, 512 + 6 * Write.PropertyName.size() + PreparedWireBytes(Write.Value),
					MaximumPreparedRequestBytes);
				for (std::size_t Previous = 0; Previous < Index; ++Previous)
					if (Writes[Previous].Object == Write.Object && Writes[Previous].PropertyName == Write.PropertyName)
						return Result;
			}
			Stage(PreparedPropertyStage::AggregateReservation);
			Stores.reserve(Writes.size());
			Records.reserve(Writes.size());
			Action.Changes.reserve(Writes.size());
			for (std::size_t Index = 0; Index < Writes.size(); ++Index) {
				Stage(PreparedPropertyStage::PropertyValue, Index);
				const auto &Write = Writes[Index];
				auto Target = ObjectRegistry::Get().Lookup(Write.Object);
				if (!Target || Target->GetDestroyed() || Target->IsDestroying()) {
					Result.Status = MutationStatus::StaleObject;
					return Result;
				}
				if (Target->GetDataModel().get() != &World) {
					Result.Status = MutationStatus::Unauthorized;
					return Result;
				}
				const auto *Class = InstanceClassRegistry::GetDefinition(Target.get());
				const auto *Property = Target->FindProperty(Write.PropertyName);
				if (!Class || !Property || Property->DeclaringSchemaId != Write.DeclaringSchemaId ||
					Property->DeclaringDefinitionVersion != Write.DefinitionVersion) {
					Result.Status = MutationStatus::InvalidProperty;
					return Result;
				}
				if (!Property->Editable || !Property->StorePrepared || Property->CustomSchemaPropertyType ||
					Property->SemanticType == InstanceProperty::DataType::Unsupported ||
					Property->SemanticType == InstanceProperty::DataType::ObjectReference || Write.PropertyName == "Source") {
					Result.Status = MutationStatus::ReadOnly;
					return Result;
				}
				std::any Native;
				WireValue After;
				const auto Validation = PreparePropertyWireMutation(*Target, *Property, Write, Security, Native, After);
				if (Validation != MutationStatus::Success) { Result.Status = Validation; return Result; }
				auto Before = Target->ReadPropertyWireValue(Write.PropertyName);
				if (!Before) return Result;
				ValidateProtocolWireValue(*Before);
				ValidateProtocolWireValue(After);
				if (ReplayAction) {
					const auto &Change = std::get<PropertyTransactionChange>(ReplayAction->Changes[Index]);
					if (*Before != (Redo ? Change.Before : Change.After)) {
						Result.Status = MutationStatus::Conflict;
						return Result;
					}
				}
				Account(Result.ValueBytes, PreparedWireBytes(*Before) + PreparedWireBytes(After),
					MaximumPreparedValueBytes);
				if (*Before == After) continue;
				Stage(PreparedPropertyStage::JournalEncoding, Index);
				Account(Result.JournalBytes, 512 + Write.PropertyName.size() * 6 + PreparedWireBytes(After),
					MaximumPreparedJournalBytes);
				const bool Replicated = Property->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated;
				Records.push_back({Replicated ? Scope : ObjectId{}, Write.Object,
					PropertyUpdatedChange{Write.PropertyName, After, Replicated}});
				Action.Changes.emplace_back(PropertyTransactionChange{Write.Object, Write.DeclaringSchemaId,
					Write.DefinitionVersion, Write.PropertyName, std::move(*Before), std::move(After)});
				Account(Result.HistoryBytes, EstimateTransactionChangeBytes(Action.Changes.back()), MaximumTransactionSemanticBytes);
				Stage(PreparedPropertyStage::Notification, Index);
				auto Signal = Target->PropertyChangedSignals.find(Write.PropertyName);
				Stores.push_back({Target, Class, Property, &Write, std::move(Native),
					Signal == Target->PropertyChangedSignals.end() ? nullptr : Signal->second});
			}
			if (Stores.empty()) { Result.Status = MutationStatus::Success; return Result; }
			Stage(PreparedPropertyStage::HistoryAction);
			Action.Id = ReplayAction ? ReplayAction->Id : AuthoritativeTransactionHistory::AllocatePreparedIdentity();
			if (!Action.Id.IsValid()) { Result.Status = MutationStatus::ResourceLimit; return Result; }
			Action.Label = "Set Properties";
			Action.StartingRevision = ExpectedRevision;
			Action.ResultingRevision = ExpectedRevision + 1;
			Action.SemanticBytes = Result.HistoryBytes;
			Action.ReplayPolicy = TransactionReplayPolicy::PreparedPropertyBatch;
			const auto ActionId = Action.Id;
			Stage(PreparedPropertyStage::HistoryRetention);
			auto Candidate = World.Transactions; // only shared immutable payloads are copied
			const auto Cursor = Candidate.Cursor;
			const auto IdentityMappings = Candidate.IdentityMappings;
			const auto Undo = World.Transactions.GetUndoTransaction();
			const auto NextRedo = World.Transactions.GetRedoTransaction();
			if (ReplayAction) {
				if ((Redo ? NextRedo : Undo) != ReplayAction) return Result;
				if (Redo) ++Candidate.Cursor; else --Candidate.Cursor;
			} else {
				if (Action.SemanticBytes > Candidate.RetainedByteLimit) {
					Result.Status = MutationStatus::ResourceLimit;
					return Result;
				}
				Candidate.Retain(std::move(Action));
			}
			// The invalidation seam is before acquiring the journal mutex: a test
			// may perform an independent mutation without deadlocking journal commit.
			Stage(PreparedPropertyStage::FinalValidation);
			Stage(PreparedPropertyStage::JournalReservation);
			ChangeJournal::PreparedJournalBatch Journal(ChangeJournal::Get(), Records);
			if (World.GetAuthoritativeRevision() != ExpectedRevision || World.GetDestroyed() || World.IsDestroying() ||
				ObjectRegistry::Get().Lookup(Scope).get() != &World ||
				Lifecycle.GetActiveGeneration() != Generation || !Registry->IsFrozen() ||
				World.Transactions.GetOpenCount() || World.Transactions.Cursor != Cursor ||
				World.Transactions.IdentityMappings != IdentityMappings ||
				World.HasDeferredRevision() ||
				World.Transactions.GetUndoTransaction() != Undo || World.Transactions.GetRedoTransaction() != NextRedo) {
				Result.Status = MutationStatus::Conflict;
				return Result;
			}
			for (const auto &Store : Stores) {
				const auto &Write = *Store.Request;
				const auto PropertyEntry = Store.Class->AllProperties.find(Write.PropertyName);
				if (ObjectRegistry::Get().Lookup(Write.Object) != Store.Target || Store.Target->GetDestroyed() ||
					Store.Target->IsDestroying() || Store.Target->GetDataModel().get() != &World ||
					InstanceClassRegistry::GetDefinition(Store.Target.get()) != Store.Class ||
					PropertyEntry == Store.Class->AllProperties.end() || PropertyEntry->second != Store.Property ||
					Store.Property->DeclaringSchemaId != Write.DeclaringSchemaId ||
					Store.Property->DeclaringDefinitionVersion != Write.DefinitionVersion) {
					Result.Status = MutationStatus::Conflict;
					return Result;
				}
			}
			// AUTHORITATIVE COMMIT BOUNDARY. Keep this body free of even diagnostic
			// string construction. Qualification rejects operator new at this point.
			auto Commit = [&]() noexcept {
				if (Hooks.Boundary) Hooks.Boundary(true);
				for (auto &Store : Stores) Store.Property->StorePrepared(*Store.Target, Store.Native);
				World.Transactions.InstallPrepared(Candidate);
				Journal.Install();
				++World.AuthoritativeRevision;
				Journal.Release();
				if (Hooks.Boundary) Hooks.Boundary(false);
			};
			Commit();
			Result.Status = MutationStatus::Success;
			Result.HistoryId = ActionId;
			Result.ResultingRevision = ExpectedRevision + 1;
			Result.ChangedWrites = Stores.size();
			for (const auto &Record : Records) {
				try { RenderDirtyAccumulator::Get().RecordChange(Record.Scope, Record.Object, Record.Payload); }
				catch (...) { ++Result.NotificationFailures; }
			}
			// Observers may initiate a later mutation. No result path below may
			// classify their allocation/exception as an authoritative failure.
			Preparing = false;
			for (const auto &Store : Stores) {
				if (!Store.Notification) continue;
				try { Store.Notification->Fire({}); }
				catch (...) { ++Result.NotificationFailures; }
			}
			return Result;
		} catch (const std::bad_alloc &) { Result.Status = MutationStatus::ResourceLimit; }
		catch (const std::length_error &) { Result.Status = MutationStatus::ResourceLimit; }
		catch (const std::overflow_error &) { Result.Status = MutationStatus::RevisionExhausted; }
		catch (...) { Result.Status = MutationStatus::ValidationFailed; }
		return Result;
	}

	TransactionResult PreparedPropertyCommit::Replay(DataModel &World, const ScriptSecurityContext &Security, bool Redo) {
		try {
			const auto *Action = Redo ? World.Transactions.GetRedoTransaction() : World.Transactions.GetUndoTransaction();
			if (!Action || Action->ReplayPolicy != TransactionReplayPolicy::PreparedPropertyBatch)
				return {.Status = TransactionStatus::InvalidState};
			std::vector<PreparedPropertyWrite> Writes;
			Writes.reserve(Action->Changes.size());
			for (const auto &Entry : Action->Changes) {
				const auto &Change = std::get<PropertyTransactionChange>(Entry);
				Writes.push_back({World.Transactions.ResolveIdentity(Change.Object), Change.DeclaringSchemaId,
					Change.DefinitionVersion, Change.PropertyName, Redo ? Change.After : Change.Before});
			}
			const auto Result = Execute(World, World.GetAuthoritativeRevision(), Writes, Security, Action, Redo);
			return {.Status = Result.Status == MutationStatus::Success ? TransactionStatus::Success : TransactionStatus::ExecutionFailed,
				.Id = Result.HistoryId, .StartingRevision = Result.StartingRevision,
				.ResultingRevision = Result.ResultingRevision, .ChangeCount = Result.ChangedWrites,
				.NotificationFailures = Result.NotificationFailures};
		} catch (...) { return {.Status = TransactionStatus::ExecutionFailed}; }
	}
}

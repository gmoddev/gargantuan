#include "gargantuan/runtime/MutationGateway.hpp"

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/LuaSourceContainer.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/WireCodec.hpp"

#include <algorithm>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	const char *GetMutationStatusDescription(MutationStatus Status) {
		switch (Status) {
		case MutationStatus::Success: return "success";
		case MutationStatus::WrongExecutionDomain: return "the mutation can only run on the Main execution domain";
		case MutationStatus::StaleObject: return "the target or referenced Instance has been destroyed or is stale";
		case MutationStatus::InvalidClass: return "the Instance class is invalid";
		case MutationStatus::InvalidProperty: return "the property does not exist";
		case MutationStatus::InvalidParent: return "the Parent target is invalid";
		case MutationStatus::ProtectedObject: return "the target is protected";
		case MutationStatus::ResourceLimit: return "the mutation would exceed a resource limit";
		case MutationStatus::RevisionExhausted: return "the authoritative revision is exhausted";
		case MutationStatus::ReadOnly: return "the property is read-only";
		case MutationStatus::Unauthorized: return "the current context is not authorized";
		case MutationStatus::ValidationFailed: return "the value or object reference failed validation";
		case MutationStatus::Conflict: return "the authoritative value changed";
		case MutationStatus::Rejected: return "the mutation was rejected";
		case MutationStatus::InternalError: return "an internal mutation error occurred";
		case MutationStatus::TransactionNotFound: return "the transaction is stale or missing";
		case MutationStatus::TransactionLimit: return "the transaction limit would be exceeded";
		}
		return "the mutation was rejected";
	}

	namespace {
		std::size_t SubtreeSize(const std::shared_ptr<Instance> &Root) {
			std::vector<std::shared_ptr<Instance>> Descendants;
			Root->CollectDescendants(Descendants);
			return Descendants.size() + 1;
		}

		std::size_t SubtreeDepth(const std::shared_ptr<Instance> &Root) {
			std::size_t Depth = 1;
			for (const auto &Child : Root->Children)
				Depth = std::max(Depth, std::size_t{1} + SubtreeDepth(Child));
			return Depth;
		}

		std::size_t AncestryDepth(const std::shared_ptr<Instance> &InstanceValue) {
			std::size_t Depth = 1;
			for (auto Current = InstanceValue->GetParent(); Current; Current = (*Current)->GetParent())
				++Depth;
			return Depth;
		}

		bool IsProtectedStructuralObject(
			const std::shared_ptr<DataModel> &DataModelValue, const std::shared_ptr<Instance> &InstanceValue
		) {
			if (!DataModelValue || DataModelValue->IsProtectedService(InstanceValue)) return true;
			auto *Definition = InstanceClassRegistry::GetDefinition(InstanceValue.get());
			return !Definition || !Definition->EditorVisible || !InstanceClassRegistry::IsConstructible(*Definition);
		}

		bool WouldCreateCycle(const std::shared_ptr<Instance> &Child, const std::shared_ptr<Instance> &Parent) {
			for (auto Current = Parent; Current;) {
				if (Current == Child) return true;
				auto Next = Current->GetParent();
				Current = Next ? *Next : nullptr;
			}
			return false;
		}

		WireValue ResolveWireReferences(const WireValue &Value, const AuthoritativeTransactionHistory &History) {
			if (const auto *Reference = std::get_if<WireObjectReference>(&Value)) {
				const auto Resolved = History.ResolveIdentity(Reference->Object.ToObjectId());
				return WireObjectReference{WireObjectId::FromObjectId(Resolved)};
			}
			return Value;
		}

		void PublishCapturedJournal(std::vector<BufferedChangeRecord> Records) {
			for (std::size_t Index = 0; Index < Records.size();) {
				const auto Scope = Records[Index].Scope;
				std::vector<std::pair<ObjectId, ChangePayload>> Changes;
				while (Index < Records.size() && Records[Index].Scope == Scope) {
					Changes.emplace_back(Records[Index].Object, std::move(Records[Index].Payload));
					++Index;
				}
				ChangeJournal::Get().CommitBatch(Scope, std::move(Changes));
			}
		}
	}
	MutationAuthorityContext::MutationAuthorityContext(
		MutationCommandOrigin OriginValue,
		ScriptSecurityContext SecurityContextValue,
		std::optional<ObjectId> ScopeValue,
		std::optional<TransactionId> TransactionValue,
		std::uint64_t TransactionOwnerValue
	)
		: Origin(OriginValue), SecurityContext(std::move(SecurityContextValue)), Scope(ScopeValue),
		  Transaction(TransactionValue), TransactionOwner(TransactionOwnerValue) {
		if (Scope && !Scope->IsValid()) throw std::invalid_argument("Mutation authority scope is invalid");
		if (Transaction && (!Transaction->IsValid() || TransactionOwner == 0))
			throw std::invalid_argument("Mutation transaction authority is invalid");
	}

	MutationAuthorityContext MutationAuthorityContext::Local(ScriptSecurityContext SecurityContext) {
		return {MutationCommandOrigin::LocalInternal, std::move(SecurityContext), std::nullopt};
	}

	MutationAuthorityContext MutationAuthorityContext::Studio(ScriptSecurityContext SecurityContext, ObjectId Scope,
		std::optional<TransactionId> Transaction,
		std::uint64_t TransactionOwner) {
		if (SecurityContext.Domain != ScriptExecutionDomain::Studio)
			throw std::invalid_argument("Studio mutation authority requires the Studio execution domain");
		return {MutationCommandOrigin::Studio, std::move(SecurityContext), Scope,
			Transaction,
			TransactionOwner,};
	}

	MutationAuthorityContext
	MutationAuthorityContext::AuthenticatedPeer(ScriptSecurityContext SecurityContext, ObjectId Scope) {
		if (SecurityContext.Domain != ScriptExecutionDomain::Client)
			throw std::invalid_argument("Peer mutation authority requires the Client execution domain");
		return {MutationCommandOrigin::AuthenticatedPeer, std::move(SecurityContext), Scope};
	}

	MutationGateway::~MutationGateway() {
		std::deque<PendingMutation> pending;
		{
			std::scoped_lock lock(Mutex);
			pending.swap(Pending);
		}
		for (auto &mutation : pending) {
			mutation.Completion->Complete(
				{MutationStatus::Rejected, std::nullopt, "Mutation gateway shut down before applying command"}
			);
		}
	}

	bool MutationCompletion::IsReady() const {
		std::scoped_lock lock(Mutex);
		return Ready;
	}

	MutationResult MutationCompletion::Wait() {
		std::unique_lock lock(Mutex);
		ReadySignal.wait(lock, [this] { return Ready; });
		return Result;
	}

	void MutationCompletion::Complete(MutationResult result) {
		{
			std::scoped_lock lock(Mutex);
			Result = std::move(result);
			Ready = true;
		}
		ReadySignal.notify_all();
	}

	std::shared_ptr<MutationCompletion>
	MutationGateway::Submit(MutationCommand command, ScriptSecurityContext securityContext) {
		return Submit(std::move(command), MutationAuthorityContext::Local(std::move(securityContext)));
	}

	std::shared_ptr<MutationCompletion>
	MutationGateway::Submit(MutationCommand command, MutationAuthorityContext Authority) {
		auto completion = std::make_shared<MutationCompletion>();
		{
			std::scoped_lock lock(Mutex);
			if (Pending.size() >= MaximumPendingMutations) {
				completion->Complete({MutationStatus::Rejected, std::nullopt, "Mutation queue is full"});
				return completion;
			}
			Pending.push_back({std::move(command), completion, std::move(Authority)});
		}
		return completion;
	}

	MutationResult MutationGateway::Apply(MutationCommand command, ScriptSecurityContext securityContext) {
		return Apply(std::move(command), MutationAuthorityContext::Local(std::move(securityContext)));
	}

	MutationResult MutationGateway::Apply(MutationCommand command, MutationAuthorityContext Authority) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) {
			return {MutationStatus::WrongExecutionDomain, std::nullopt, "Mutation application requires Main"};
		}
		const auto &securityContext = Authority.GetSecurityContext();
		if (!securityContext.HasCapability(ScriptCapability::MutateDataModel))
			return {MutationStatus::Unauthorized, std::nullopt, "Mutation requires MutateDataModel"};

		auto ApplyAuthoringTransaction = [&]<typename Operation, typename BuildChange>(
											 const std::shared_ptr<DataModel> &World,
											 std::string Label,
											 TransactionChange Prototype,
											 Operation &&ApplyOperation,
											 BuildChange &&BuildCommittedChange
										 ) -> MutationResult {
			if (Authority.GetOrigin() != MutationCommandOrigin::Studio)
				return std::forward<Operation>(ApplyOperation)();
			if (!World) return {MutationStatus::Rejected, std::nullopt, "Authoring target has no DataModel"};

			auto &Transactions = World->Transactions;
			const auto Owner = Authority.GetTransactionOwner();
			const bool Implicit = !Authority.GetTransactionId();
			auto Begin = Implicit ? Transactions.BeginImplicit(*World, Owner, std::move(Label))
								  : TransactionResult{
										.Status = Transactions.IsOpen(*Authority.GetTransactionId(), Owner)
													  ? TransactionStatus::Success
													  : TransactionStatus::NotFound,
										.Id = *Authority.GetTransactionId(),
									};
			if (!Begin.Succeeded())
				return {
					Begin.Status == TransactionStatus::LimitExceeded ? MutationStatus::TransactionLimit
																	 : MutationStatus::TransactionNotFound,
					std::nullopt,
					Begin.Message.empty() ? "Transaction is not open for this session" : Begin.Message,
				};
			const auto Id = Begin.Id;
			auto Admission = Transactions.ValidateMutation(Id, Owner, EstimateTransactionChangeBytes(Prototype));
			if (!Admission.Succeeded()) {
				if (Implicit) (void)Transactions.Commit(*World, Id, Owner);
				return {MutationStatus::TransactionLimit, std::nullopt, Admission.Message};
			}

			MutationResult Result;
			bool Changed = false;
			std::vector<BufferedChangeRecord> JournalRecords;
			{
				ScopedAuthoritativeRevisionDeferral RevisionDeferral(*World, Changed);
				ScopedChangeJournalCapture JournalCapture;
				try {
					Result = std::forward<Operation>(ApplyOperation)();
				} catch (...) {
					(void)JournalCapture.Take();
					if (Implicit) (void)Transactions.Commit(*World, Id, Owner);
					throw;
				}
				JournalRecords = JournalCapture.Take();
			}
			if (Result.Succeeded() && Changed)
				Transactions.RecordMutation(
					Id, Owner, std::forward<BuildChange>(BuildCommittedChange)(Result), std::move(JournalRecords)
				);
			if (Implicit) {
				auto Commit = Transactions.Commit(*World, Id, Owner);
				if (!Commit.Succeeded())
					return {
						Commit.Status == TransactionStatus::RevisionExhausted ? MutationStatus::RevisionExhausted
																			  : MutationStatus::InternalError,
						std::nullopt,
						Commit.Message,
					};
			}
			return Result;
		};

		try {
			return std::visit(
				[&](auto &CommandValue) -> MutationResult {
					using Command = std::decay_t<decltype(CommandValue)>;
					if constexpr (std::is_same_v<Command, RestoreSubtreeCommand>) {
						if (Authority.GetOrigin() != MutationCommandOrigin::LocalInternal)
							return {MutationStatus::Unauthorized, std::nullopt, "Subtree restoration is internal-only"};
						auto Parent = ObjectRegistry::Get().Lookup(CommandValue.Parent);
						if (!Parent) return {MutationStatus::StaleObject, std::nullopt, "Restore parent identity is stale"};
						auto DataModelValue = Parent->GetDataModel();
						if (!DataModelValue)
							return {MutationStatus::InvalidParent, std::nullopt, "Restore parent is not project-owned"};
						std::istringstream Input(CommandValue.PersistentSnapshot);
						InstanceSerialization::DeserializationState State;
						{
							ScopedChangeJournalSuppression Suppression;
							State = InstanceSerialization::DeserializeDetached(
								InstanceSerialization::InstanceFormat::Json, Input
							);
						}
						if (!State.Ok || !State.Instance)
							return {MutationStatus::ValidationFailed, std::nullopt, "History subtree snapshot is incompatible"};
						const auto Required = SubtreeSize(State.Instance);
						if (CommandValue.ExpectedObjectCount == 0 || Required != CommandValue.ExpectedObjectCount)
							return {MutationStatus::ValidationFailed, std::nullopt, "History subtree shape is incompatible"};
						const auto Existing = SubtreeSize(DataModelValue);
						if (Existing > MaximumPersistenceObjects || Required > MaximumPersistenceObjects - Existing)
							return {MutationStatus::ResourceLimit, std::nullopt, "Restore would exceed the object-count limit"};
						if (AncestryDepth(Parent) + SubtreeDepth(State.Instance) > MaximumProtocolJsonDepth)
							return {MutationStatus::ResourceLimit, std::nullopt, "Restore would exceed the hierarchy depth limit"};
						auto Root = State.Instance;
						Root->MarkPersistenceSubtreeArchivable();
						DataModelValue->BeginAuthoritativeRevisionBatch();
						try {
							{
								ScopedChangeJournalSuppression Suppression;
								Root->SetParent(Parent);
								for (const auto &[Node, Tags] : State.PendingTags)
									for (const auto &Tag : Tags)
										(void)DataModelValue->Tags.Add(
											DataModelValue->GetObjectId(), Node->GetObjectId(), Tag,
											ScriptSecurityContext::CoreTrusted()
										);
							}
							Root->PublishReplicationSubtree(DataModelValue->GetObjectId());
							DataModelValue->CommitAuthoritativeRevisionBatch();
							return {MutationStatus::Success, Root->GetObjectId(), {}};
						} catch (...) {
							{
								ScopedChangeJournalSuppression Suppression;
								Root->Destroy();
							}
							DataModelValue->CancelAuthoritativeRevisionBatch();
							throw;
						}
					} else if constexpr (std::is_same_v<Command, CreateObjectCommand>) {
						if (CommandValue.InitialProperties.size() > MaximumCreateInitialProperties)
							return {
								MutationStatus::ResourceLimit,
								std::nullopt,
								"Create initial-property count exceeds its bound"
							};
						for (std::size_t Left = 0; Left < CommandValue.InitialProperties.size(); ++Left)
							for (std::size_t Right = Left + 1; Right < CommandValue.InitialProperties.size(); ++Right)
								if (CommandValue.InitialProperties[Left].PropertyName ==
									CommandValue.InitialProperties[Right].PropertyName)
									return {
										MutationStatus::ValidationFailed,
										std::nullopt,
										"Create initial properties contain a duplicate name"
									};
						auto *Definition = InstanceClassRegistry::GetDefinitionBySchemaId(CommandValue.ClassSchemaId);
						if (!Definition || Definition->DefinitionVersion != CommandValue.DefinitionVersion ||
							!InstanceClassRegistry::IsConstructible(*Definition) || !Definition->EditorVisible ||
							Definition->ClassName == "DataModel")
							return {
								MutationStatus::InvalidClass,
								std::nullopt,
								"Class is missing, incompatible, or not editor-constructible"
							};
						auto Parent = ObjectRegistry::Get().Lookup(CommandValue.Parent);
						if (!Parent) return {MutationStatus::StaleObject, std::nullopt, "Parent identity is stale"};
						if (Authority.GetScope() && Parent->GetReplicationScopeId() != *Authority.GetScope())
							return {MutationStatus::InvalidParent, std::nullopt, "Parent is outside the project scope"};
						auto DataModelValue = Parent->GetDataModel();
						if (!DataModelValue)
							return {MutationStatus::InvalidParent, std::nullopt, "Parent is not project-owned"};
						const auto Scope = DataModelValue->GetObjectId();
						if (DataModelValue->IsProtectedServiceClass(Definition->Id))
							return {
								MutationStatus::InvalidClass,
								std::nullopt,
								"Service and singleton classes are not editor-constructible"
							};
						if (SubtreeSize(DataModelValue) >= MaximumPersistenceObjects)
							return {
								MutationStatus::ResourceLimit, std::nullopt, "Project object-count limit is reached"
							};
						if (AncestryDepth(Parent) + 1 > MaximumProtocolJsonDepth)
							return {
								MutationStatus::ResourceLimit,
								std::nullopt,
								"Create would exceed the hierarchy depth limit"
							};
						auto InstanceValue = InstanceClassRegistry::Construct(*Definition);
						if (!InstanceValue)
							return {MutationStatus::InternalError, std::nullopt, "Class constructor returned null"};
						auto ScriptValue = std::dynamic_pointer_cast<LuaSourceContainer>(InstanceValue);
						if (CommandValue.InitialSource && !ScriptValue)
							return {
								MutationStatus::InvalidClass,
								std::nullopt,
								"Initial script source requires a LuaSourceContainer class"
							};
						{
							ScopedChangeJournalSuppression Suppression;
							if (InstanceValue->ApplyPropertyMutation(
									"Archivable", true, Enums::Permission::Engine, securityContext
								) != MutationStatus::Success)
								return {
									MutationStatus::ValidationFailed,
									std::nullopt,
									"Created class cannot enter persistent project state"
								};
							if (CommandValue.Name &&
								InstanceValue->ApplyPropertyMutation(
									"Name", *CommandValue.Name, Enums::Permission::Engine, securityContext
								) != MutationStatus::Success)
								return {MutationStatus::ValidationFailed, std::nullopt, "Initial Name is invalid"};
							if (CommandValue.InitialSource) ScriptValue->SetSource(*CommandValue.InitialSource);
							for (const auto &Initial : CommandValue.InitialProperties) {
								auto *Property = InstanceValue->FindProperty(Initial.PropertyName);
								if (!Property || Property->DeclaringSchemaId != Initial.DeclaringClassSchemaId ||
									Property->DeclaringDefinitionVersion != Initial.DeclaringDefinitionVersion ||
									!Property->Editable ||
									Property->SemanticType == InstanceProperty::DataType::Unsupported ||
									Property->SemanticType == InstanceProperty::DataType::ObjectReference ||
									Initial.PropertyName == "Source")
									return {
										MutationStatus::ReadOnly,
										std::nullopt,
										"Create initial property is absent, stale, or not editor-writable"
									};
								const auto Status = InstanceValue->ApplyPropertyWireMutation(
									Initial.PropertyName, Initial.Value, Enums::Permission::None, securityContext
								);
								if (Status != MutationStatus::Success)
									return {
										Status,
										std::nullopt,
										std::format(
											"Cannot initialize {}: {}",
											Initial.PropertyName,
											GetMutationStatusDescription(Status)
										)
									};
							}
						}
						std::string Snapshot;
						if (Authority.GetOrigin() == MutationCommandOrigin::Studio) {
							Snapshot = InstanceSerialization::Serialize(
								InstanceSerialization::InstanceFormat::Json, InstanceValue
							);
							if (Snapshot.size() > MaximumTransactionSubtreeBytes)
								return {
									MutationStatus::TransactionLimit,
									std::nullopt,
									"Created subtree exceeds transaction history limits"
								};
						}
						SubtreeTransactionChange Prototype{
							SubtreeTransactionKind::Create, {}, CommandValue.Parent, {}, Snapshot
						};
						Prototype.Objects.resize(1);
						return ApplyAuthoringTransaction(
							DataModelValue,
							"Create Instance",
							Prototype,
							[&] {
						DataModelValue->BeginAuthoritativeRevisionBatch();
						try {
							{
								ScopedChangeJournalSuppression Suppression;
								InstanceValue->SetParent(Parent);
							}
							const auto Id = InstanceValue->GetObjectId();
							InstanceValue->PublishReplicationSubtree(Scope);
							DataModelValue->CommitAuthoritativeRevisionBatch();
							return MutationResult {MutationStatus::Success, Id, {}};
						} catch (...) {
							{
								ScopedChangeJournalSuppression Suppression;
								InstanceValue->Destroy();
							}
							DataModelValue->CancelAuthoritativeRevisionBatch();
							throw;
						}
							},
							[&](const MutationResult &Result) -> TransactionChange {
								auto Change = Prototype;
								Change.Root = *Result.Object;
								Change.Objects.clear();
								std::vector<std::shared_ptr<Instance>> Nodes{InstanceValue};
								InstanceValue->CollectDescendants(Nodes);
								for (const auto &Node : Nodes)
									Change.Objects.push_back(Node->GetObjectId());
								return Change;
							}
						);
					} else {
						auto InstanceValue = ObjectRegistry::Get().Lookup(CommandValue.Object);
						if (!InstanceValue)
							return {MutationStatus::StaleObject, std::nullopt, "Object identity is stale"};
						if (Authority.GetScope() && InstanceValue->GetReplicationScopeId() != *Authority.GetScope())
							return {MutationStatus::Rejected, std::nullopt, "Object is outside the project scope"};
						if constexpr (std::is_same_v<Command, UpdateWirePropertiesCommand>) {
							if (Authority.GetOrigin() != MutationCommandOrigin::Studio)
								return {MutationStatus::Unauthorized, CommandValue.Object,
									"Atomic property authoring is Studio-only"};
							if (CommandValue.Properties.empty() ||
								CommandValue.Properties.size() > MaximumAtomicPropertyMutations)
								return {MutationStatus::ResourceLimit, CommandValue.Object,
									"Atomic property mutation count is out of range"};
							for (std::size_t Left = 0; Left < CommandValue.Properties.size(); ++Left)
								for (std::size_t Right = Left + 1; Right < CommandValue.Properties.size(); ++Right)
									if (CommandValue.Properties[Left].PropertyName ==
										CommandValue.Properties[Right].PropertyName)
										return {MutationStatus::ValidationFailed, CommandValue.Object,
											"Atomic property mutation contains a duplicate property"};

							auto DataModelValue = InstanceValue->GetDataModel();
							if (!DataModelValue)
								return {MutationStatus::Rejected, CommandValue.Object,
									"Atomic property target has no DataModel"};
							std::vector<PropertyTransactionChange> Changes;
							Changes.reserve(CommandValue.Properties.size());
							std::size_t SemanticBytes = 0;
							for (const auto &Requested : CommandValue.Properties) {
								if (Requested.PropertyName == "Source" &&
									std::dynamic_pointer_cast<LuaSourceContainer>(InstanceValue))
									return {MutationStatus::Unauthorized, CommandValue.Object,
										"Script source requires the dedicated source-authoring operation"};
								auto *Property = InstanceValue->FindProperty(Requested.PropertyName);
								if (!Property || !Property->Editable ||
									Property->SemanticType == InstanceProperty::DataType::Unsupported)
									return {MutationStatus::ReadOnly, CommandValue.Object,
										"Property is not exposed for Studio editing by the frozen schema"};
								const auto Validation = InstanceValue->ValidatePropertyWireMutation(
									Requested.PropertyName, Requested.Value, Enums::Permission::None, securityContext
								);
								if (Validation != MutationStatus::Success)
									return {Validation, CommandValue.Object,
										std::format("Cannot set {}: {}", Requested.PropertyName,
											GetMutationStatusDescription(Validation))};
								auto Before = InstanceValue->ReadPropertyWireValue(Requested.PropertyName)
									.value_or(WireValue(std::monostate{}));
								if (Before == Requested.Value) continue;
								PropertyTransactionChange Change{
									CommandValue.Object, Property->DeclaringSchemaId,
									Property->DeclaringDefinitionVersion, Requested.PropertyName,
									std::move(Before), Requested.Value,
								};
								SemanticBytes += EstimateTransactionChangeBytes(Change);
								Changes.push_back(std::move(Change));
							}
							if (Changes.empty()) return {MutationStatus::Success, CommandValue.Object, {}};

							auto &Transactions = DataModelValue->Transactions;
							const auto Owner = Authority.GetTransactionOwner();
							const bool Implicit = !Authority.GetTransactionId();
							auto Begin = Implicit
								? Transactions.BeginImplicit(*DataModelValue, Owner, "Set Transform")
								: TransactionResult{
									.Status = Transactions.IsOpen(*Authority.GetTransactionId(), Owner)
										? TransactionStatus::Success : TransactionStatus::NotFound,
									.Id = *Authority.GetTransactionId(),
								};
							if (!Begin.Succeeded())
								return {
									Begin.Status == TransactionStatus::LimitExceeded ? MutationStatus::TransactionLimit
										: MutationStatus::TransactionNotFound,
									CommandValue.Object,
									Begin.Message.empty() ? "Transaction is not open for this session" : Begin.Message,
								};
							const auto Transaction = Begin.Id;
							auto Admission = Transactions.ValidateMutation(
								Transaction, Owner, SemanticBytes, Changes.size()
							);
							if (!Admission.Succeeded()) {
								if (Implicit) (void)Transactions.Commit(*DataModelValue, Transaction, Owner);
								return {MutationStatus::TransactionLimit, CommandValue.Object, Admission.Message};
							}

							MutationResult Result{MutationStatus::Success, CommandValue.Object, {}};
							bool Changed = false;
							std::vector<BufferedChangeRecord> JournalRecords;
							std::size_t Applied = 0;
							{
								ScopedAuthoritativeRevisionDeferral RevisionDeferral(*DataModelValue, Changed);
								ScopedChangeJournalCapture JournalCapture;
								auto RollBack = [&] {
									ScopedChangeJournalSuppression Suppression;
									while (Applied > 0) {
										--Applied;
										const auto Status = InstanceValue->ApplyPropertyWireMutation(
											Changes[Applied].PropertyName, Changes[Applied].Before,
											Enums::Permission::None, securityContext
										);
										if (Status != MutationStatus::Success)
											throw std::logic_error("Atomic property rollback failed");
									}
								};
								try {
									for (const auto &Change : Changes) {
										const auto Status = InstanceValue->ApplyPropertyWireMutation(
											Change.PropertyName, Change.After, Enums::Permission::None, securityContext
										);
										if (Status != MutationStatus::Success) {
											Result = {Status, CommandValue.Object,
												std::format("Cannot set {}: {}", Change.PropertyName,
													GetMutationStatusDescription(Status))};
											RollBack();
											break;
										}
										++Applied;
									}
								} catch (...) {
									RollBack();
									(void)JournalCapture.Take();
									if (Implicit) (void)Transactions.Commit(*DataModelValue, Transaction, Owner);
									throw;
								}
								JournalRecords = JournalCapture.Take();
							}
							if (Result.Succeeded()) {
								for (std::size_t Index = 0; Index < Changes.size(); ++Index) {
									Changes[Index].After = InstanceValue->ReadPropertyWireValue(Changes[Index].PropertyName)
										.value_or(Changes[Index].After);
									Transactions.RecordMutation(
										Transaction, Owner, Changes[Index],
										Index == 0 ? std::move(JournalRecords) : std::vector<BufferedChangeRecord>{}
									);
								}
							}
							if (Implicit) {
								auto Commit = Transactions.Commit(*DataModelValue, Transaction, Owner);
								if (!Commit.Succeeded())
									return {
										Commit.Status == TransactionStatus::RevisionExhausted
											? MutationStatus::RevisionExhausted : MutationStatus::InternalError,
										CommandValue.Object, Commit.Message,
									};
							}
							return Result;
						} else if constexpr (std::is_same_v<Command, UpdatePropertyCommand> ||
							std::is_same_v<Command, UpdateWirePropertyCommand>) {
							if (CommandValue.PropertyName == "Source" &&
								std::dynamic_pointer_cast<LuaSourceContainer>(InstanceValue))
								return {MutationStatus::Unauthorized, std::nullopt,
									"Script source requires the dedicated source-authoring operation"};
							auto DataModelValue = InstanceValue->GetDataModel();
							auto *Property = InstanceValue->FindProperty(CommandValue.PropertyName);
							if (Authority.GetOrigin() == MutationCommandOrigin::Studio &&
								(!Property || !Property->Editable ||
								 Property->SemanticType == InstanceProperty::DataType::Unsupported))
								return {MutationStatus::ReadOnly, CommandValue.Object,
									"Property is not exposed for Studio editing by the frozen schema"};
							auto Before = InstanceValue->ReadPropertyWireValue(CommandValue.PropertyName)
										  .value_or(WireValue(std::monostate{}));
							auto Requested = [&]() -> WireValue {
								if constexpr (std::is_same_v<Command, UpdateWirePropertyCommand>) return CommandValue.Value;
								else return EncodeNativeWireValue(CommandValue.Value).value_or(Before);
							}();
							PropertyTransactionChange Prototype{
								CommandValue.Object,
								Property ? Property->DeclaringSchemaId : SchemaId{},
								Property ? Property->DeclaringDefinitionVersion : 0,
								CommandValue.PropertyName,
								Before,
								Requested,
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Set " + CommandValue.PropertyName,
								Prototype,
								[&] {
							const auto Status = [&] {
								if constexpr (std::is_same_v<Command, UpdateWirePropertyCommand>)
									return InstanceValue->ApplyPropertyWireMutation(
										CommandValue.PropertyName, CommandValue.Value, Enums::Permission::None, securityContext
									);
								else return InstanceValue->ApplyPropertyMutation(
									CommandValue.PropertyName, CommandValue.Value, Enums::Permission::None, securityContext
								);
							}();
							return MutationResult {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" :
									std::format("Cannot set {}: {}", CommandValue.PropertyName, GetMutationStatusDescription(Status))
							};
						},
								[&](const MutationResult &) -> TransactionChange {
									auto Committed = Prototype;
									Committed.After = InstanceValue->ReadPropertyWireValue(CommandValue.PropertyName)
														  .value_or(Committed.After);
									return Committed;
								}
							);
						} else if constexpr (std::is_same_v<Command, UpdateScriptSourceCommand>) {
							auto ScriptValue = std::dynamic_pointer_cast<LuaSourceContainer>(InstanceValue);
							if (!ScriptValue)
								return {MutationStatus::InvalidClass, std::nullopt, "Object is not a supported script"};
							if (CommandValue.ExpectedSourceVersion <= 0 ||
								ScriptValue->GetSourceVersion() != CommandValue.ExpectedSourceVersion)
								return {MutationStatus::Conflict, CommandValue.Object,
									"Authoritative script source changed after it was loaded"};
							auto *Definition = InstanceClassRegistry::GetDefinitionByName("LuaSourceContainer");
							if (!Definition)
								return {MutationStatus::InternalError, std::nullopt, "LuaSourceContainer schema is unavailable"};
							auto DataModelValue = ScriptValue->GetDataModel();
							ScriptSourceTransactionChange Change{
								CommandValue.Object,
								Definition->Id,
								Definition->DefinitionVersion,
								ScriptValue->GetSource(),
								CommandValue.Source,
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Edit Script",
								Change,
								[&] {
									ScriptValue->SetSource(std::move(CommandValue.Source));
									return MutationResult{MutationStatus::Success, CommandValue.Object, {}};
								},
								[Change](const MutationResult &) -> TransactionChange { return Change; }
							);
						} else if constexpr (std::is_same_v<Command, UpdateAttributeCommand>) {
							auto DataModelValue = InstanceValue->GetDataModel();
							AttributeTransactionChange Change{
								CommandValue.Object,
								CommandValue.AttributeName,
								InstanceValue->GetAttributeValue(
									CommandValue.AttributeName, ScriptSecurityContext::CoreTrusted()
								),
								CommandValue.Value,
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Set Attribute",
								Change,
								[&] {
							const auto Status = InstanceValue->ApplyAttributeMutation(
								CommandValue.AttributeName, std::move(CommandValue.Value), securityContext
							);
							return MutationResult {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Attribute mutation rejected"
							};
						},
								[Change](const MutationResult &) -> TransactionChange { return Change; }
							);
						} else if constexpr (std::is_same_v<Command, UpdateExtensionPropertyCommand>) {
							auto DataModelValue = InstanceValue->GetDataModel();
							WireValue BeforeExtension = CommandValue.Value;
							try {
								BeforeExtension = InstanceValue->GetExtensionPropertyValue(
									CommandValue.ExtensionSchemaId,
									CommandValue.PropertyName,
									ScriptSecurityContext::CoreTrusted()
								);
							} catch (const std::invalid_argument &) {}
							ExtensionTransactionChange Change{
								CommandValue.Object,
								CommandValue.ExtensionSchemaId,
								CommandValue.DefinitionVersion,
								CommandValue.PropertyName,
								std::move(BeforeExtension),
								CommandValue.Value,
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Set Extension Property",
								Change,
								[&] {
							const auto Status = InstanceValue->ApplyExtensionPropertyMutation(
								CommandValue.ExtensionSchemaId,
								CommandValue.DefinitionVersion,
								CommandValue.PropertyName,
								std::move(CommandValue.Value),
								securityContext
							);
							return MutationResult {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Extension property mutation rejected"
							};
								},
								[Change](const MutationResult &) -> TransactionChange { return Change; }
							);
						} else if constexpr (std::is_same_v<Command, AddTagCommand> ||
											 std::is_same_v<Command, RemoveTagCommand>) {
							auto DataModelValue = InstanceValue->GetDataModel();
							if (!DataModelValue)
								return {MutationStatus::Rejected, std::nullopt, "Tag target is not project-owned"};
							const auto Scope = DataModelValue->GetObjectId();
							if (CommandValue.ExpectedScope && *CommandValue.ExpectedScope != Scope)
								return {
									MutationStatus::Rejected, std::nullopt, "Tag target is outside the expected scope"
							};
							const bool Before = DataModelValue->Tags.Has(
								Scope, CommandValue.Object, CommandValue.TagName, ScriptSecurityContext::CoreTrusted()
							);
							const bool After = std::is_same_v<Command, AddTagCommand>;
							TagTransactionChange Change{CommandValue.Object, CommandValue.TagName, Before, After};
							return ApplyAuthoringTransaction(
								DataModelValue,
								After ? "Add Tag" : "Remove Tag",
								Change,
								[&] {
							if constexpr (std::is_same_v<Command, AddTagCommand>)
								(void)DataModelValue->Tags.Add(
									Scope, CommandValue.Object, CommandValue.TagName, securityContext
								);
							else
								(void)DataModelValue->Tags.Remove(
									Scope, CommandValue.Object, CommandValue.TagName, securityContext
								);
							return MutationResult {MutationStatus::Success, CommandValue.Object, {}};
								},
								[Change](const MutationResult &) -> TransactionChange { return Change; }
							);
						} else if constexpr (std::is_same_v<Command, ReparentObjectCommand>) {
							if (!CommandValue.Parent)
								return {
									MutationStatus::InvalidParent, std::nullopt, "Project objects require a parent"
								};
							auto Parent = ObjectRegistry::Get().Lookup(*CommandValue.Parent);
							if (!Parent) return {MutationStatus::StaleObject, std::nullopt, "Parent identity is stale"};
							auto DataModelValue = InstanceValue->GetDataModel();
							if (IsProtectedStructuralObject(DataModelValue, InstanceValue))
								return {
									MutationStatus::ProtectedObject,
									std::nullopt,
									"Protected project objects cannot be reparented"
								};
							if (Parent->GetDataModel() != DataModelValue)
								return {
									MutationStatus::InvalidParent,
									std::nullopt,
									"Parent belongs to a different DataModel"
								};
							if (WouldCreateCycle(InstanceValue, Parent))
								return {
									MutationStatus::InvalidParent,
									std::nullopt,
									"Parent would create a hierarchy cycle"
								};
							if (AncestryDepth(Parent) + SubtreeDepth(InstanceValue) > MaximumProtocolJsonDepth)
								return {
									MutationStatus::ResourceLimit,
									std::nullopt,
									"Reparent would exceed the hierarchy depth limit"
								};
							ChangeJournal::Get().EnsureCanCommit(DataModelValue->GetObjectId(), 1);
							auto OldParent = InstanceValue->GetParent();
							ReparentTransactionChange Change{
								CommandValue.Object,
								OldParent ? std::optional((*OldParent)->GetObjectId()) : std::nullopt,
								CommandValue.Parent,
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Reparent Instance",
								Change,
								[&] {
							DataModelValue->BeginAuthoritativeRevisionBatch();
							try {
								InstanceValue->SetParent(Parent);
								DataModelValue->CommitAuthoritativeRevisionBatch();
							} catch (...) {
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
							return MutationResult {MutationStatus::Success, CommandValue.Object, {}};
								},
								[Change](const MutationResult &) -> TransactionChange { return Change; }
							);
						} else if constexpr (std::is_same_v<Command, DestroyObjectCommand>) {
							auto DataModelValue = InstanceValue->GetDataModel();
							if (IsProtectedStructuralObject(DataModelValue, InstanceValue))
								return {
									MutationStatus::ProtectedObject,
									std::nullopt,
									"Protected project objects cannot be destroyed"
								};
							std::vector<std::shared_ptr<Instance>> DestroyedNodes{InstanceValue};
							InstanceValue->CollectDescendants(DestroyedNodes);
							std::size_t DestroyRecords = 0;
							for (const auto &Node : DestroyedNodes)
								DestroyRecords += 2 + DataModelValue->Tags
														  .GetTags(
															  DataModelValue->GetObjectId(),
															  Node->GetObjectId(),
															  ScriptSecurityContext::CoreTrusted()
														  )
														  .size();
							ChangeJournal::Get().EnsureCanCommit(DataModelValue->GetObjectId(), DestroyRecords);
							std::string Snapshot;
							if (Authority.GetOrigin() == MutationCommandOrigin::Studio) {
								Snapshot = InstanceSerialization::Serialize(
									InstanceSerialization::InstanceFormat::Json, InstanceValue
								);
								if (Snapshot.size() > MaximumTransactionSubtreeBytes)
									return {
										MutationStatus::TransactionLimit,
										std::nullopt,
										"Destroyed subtree exceeds transaction history limits"
									};
							}
							SubtreeTransactionChange Change{
								SubtreeTransactionKind::Destroy,
								CommandValue.Object,
								InstanceValue->GetParent() ? std::optional((*InstanceValue->GetParent())->GetObjectId())
														   : std::nullopt,
								{},
								std::move(Snapshot),
							};
							for (const auto &Node : DestroyedNodes)
								Change.Objects.push_back(Node->GetObjectId());
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Delete Instance",
								Change,
								[&] {
							DataModelValue->BeginAuthoritativeRevisionBatch();
							try {
								InstanceValue->Destroy();
								DataModelValue->CommitAuthoritativeRevisionBatch();
							} catch (...) {
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
							return MutationResult {MutationStatus::Success, CommandValue.Object, {}};
								},
								[Change](const MutationResult &) mutable -> TransactionChange {
									return std::move(Change);
								}
							);
						} else {
							auto DataModelValue = InstanceValue->GetDataModel();
							if (IsProtectedStructuralObject(DataModelValue, InstanceValue))
								return {
									MutationStatus::ProtectedObject,
									std::nullopt,
									"Protected project objects cannot be duplicated"
								};
							auto Parent = InstanceValue->GetParent();
							if (!Parent)
								return {MutationStatus::InvalidParent, std::nullopt, "Duplicate source has no parent"};
							const auto Required = SubtreeSize(InstanceValue);
							const auto Existing = SubtreeSize(DataModelValue);
							if (Existing > MaximumPersistenceObjects ||
								Required > MaximumPersistenceObjects - Existing)
								return {
									MutationStatus::ResourceLimit,
									std::nullopt,
									"Duplicate would exceed the object-count limit"
								};
							if (AncestryDepth(*Parent) + SubtreeDepth(InstanceValue) > MaximumProtocolJsonDepth)
								return {
									MutationStatus::ResourceLimit,
									std::nullopt,
									"Duplicate would exceed the hierarchy depth limit"
								};
							std::string Encoded = InstanceSerialization::Serialize(
								InstanceSerialization::InstanceFormat::Json, InstanceValue
							);
							std::istringstream Input(Encoded);
							InstanceSerialization::DeserializationState State;
							{
								ScopedChangeJournalSuppression Suppression;
								State = InstanceSerialization::DeserializeDetached(
									InstanceSerialization::InstanceFormat::Json, Input
								);
							}
							if (!State.Ok || !State.Instance)
								return {
									MutationStatus::ValidationFailed,
									std::nullopt,
									"Persistent source state could not be cloned"
								};
							auto Clone = State.Instance;
							Clone->MarkPersistenceSubtreeArchivable();
							if (Authority.GetOrigin() == MutationCommandOrigin::Studio &&
								Encoded.size() > MaximumTransactionSubtreeBytes)
								return {
									MutationStatus::TransactionLimit,
									std::nullopt,
									"Duplicated subtree exceeds transaction history limits"
								};
							SubtreeTransactionChange Prototype{
								SubtreeTransactionKind::Duplicate,
								{},
								(*Parent)->GetObjectId(),
								{},
								Authority.GetOrigin() == MutationCommandOrigin::Studio ? Encoded : std::string{}
							};
							Prototype.Objects.resize(Required);
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Duplicate Instance",
								Prototype,
								[&] {
							DataModelValue->BeginAuthoritativeRevisionBatch();
							try {
								{
									ScopedChangeJournalSuppression Suppression;
									Clone->SetParent(*Parent);
								}
								const auto Id = Clone->GetObjectId();
								Clone->PublishReplicationSubtree(DataModelValue->GetObjectId());
								DataModelValue->CommitAuthoritativeRevisionBatch();
								return MutationResult {MutationStatus::Success, Id, {}};
							} catch (...) {
								{
									ScopedChangeJournalSuppression Suppression;
									Clone->Destroy();
								}
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
								},
								[&](const MutationResult &Result) -> TransactionChange {
									auto Change = Prototype;
									Change.Root = *Result.Object;
									Change.Objects.clear();
									std::vector<std::shared_ptr<Instance>> Nodes{Clone};
									Clone->CollectDescendants(Nodes);
									for (const auto &Node : Nodes)
										Change.Objects.push_back(Node->GetObjectId());
									return Change;
								}
							);
						}
					}
				},
				command
			);
		} catch (const std::invalid_argument &error) {
			return {MutationStatus::ValidationFailed, std::nullopt, error.what()};
		} catch (const std::overflow_error &error) {
			return {MutationStatus::RevisionExhausted, std::nullopt, error.what()};
		} catch (const std::exception &error) {
			return {MutationStatus::Rejected, std::nullopt, error.what()};
		} catch (...) {
			return {MutationStatus::InternalError, std::nullopt, "Unknown mutation failure"};
		}
	}

	namespace {
		TransactionResult ExecuteHistory(
			MutationGateway &Gateway, DataModel &World, ScriptSecurityContext SecurityContext, bool Redo
		) {
			auto &History = World.Transactions;
			if (History.GetOpenCount() != 0)
				return {.Status = TransactionStatus::InvalidState, .Message = "History execution is unavailable while a transaction is open"};
			const auto *Transaction = Redo ? History.GetRedoTransaction() : History.GetUndoTransaction();
			if (!Transaction)
				return {.Status = Redo ? TransactionStatus::NothingToRedo : TransactionStatus::NothingToUndo,
					.Message = Redo ? "There is nothing to redo" : "There is nothing to undo"};
			try {
				World.EnsureAuthoritativeRevisionAvailable();
			} catch (const std::overflow_error &Error) {
				return {.Status = TransactionStatus::RevisionExhausted, .Id = Transaction->Id, .Message = Error.what()};
			}

			auto ApplyChange = [&](const TransactionChange &Change, bool Forward) -> MutationResult {
				return std::visit([&](const auto &Typed) -> MutationResult {
					using ChangeType = std::decay_t<decltype(Typed)>;
					if constexpr (std::is_same_v<ChangeType, PropertyTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
						if (!InstanceValue) return {MutationStatus::StaleObject, {}, "History property target is stale"};
						auto *Property = InstanceValue->FindProperty(Typed.PropertyName);
						if (!Property || Property->DeclaringSchemaId != Typed.DeclaringSchemaId ||
							Property->DeclaringDefinitionVersion != Typed.DefinitionVersion)
							return {MutationStatus::ValidationFailed, {}, "History property schema is incompatible"};
						const auto Expected = ResolveWireReferences(Forward ? Typed.Before : Typed.After, History);
						if (InstanceValue->ReadPropertyWireValue(Typed.PropertyName) != std::optional<WireValue>(Expected))
							return {MutationStatus::ValidationFailed, {}, "History property state has diverged"};
						auto Value = ResolveWireReferences(Forward ? Typed.After : Typed.Before, History);
						return Gateway.Apply(UpdateWirePropertyCommand{Object, Typed.PropertyName, std::move(Value)},
							MutationAuthorityContext::Local(SecurityContext));
					} else if constexpr (std::is_same_v<ChangeType, ScriptSourceTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto ScriptValue = std::dynamic_pointer_cast<LuaSourceContainer>(ObjectRegistry::Get().Lookup(Object));
						if (!ScriptValue) return {MutationStatus::StaleObject, {}, "History script target is stale"};
						auto *Definition = InstanceClassRegistry::GetDefinitionByName("LuaSourceContainer");
						if (!Definition || Definition->Id != Typed.DeclaringSchemaId ||
							Definition->DefinitionVersion != Typed.DefinitionVersion)
							return {MutationStatus::ValidationFailed, {}, "History script schema is incompatible"};
						const auto &Expected = Forward ? Typed.Before : Typed.After;
						if (ScriptValue->GetSource() != Expected)
							return {MutationStatus::ValidationFailed, {}, "History script source has diverged"};
						return Gateway.Apply(UpdateScriptSourceCommand{
							Object,
							ScriptValue->GetSourceVersion(),
							Forward ? Typed.After : Typed.Before,
						}, MutationAuthorityContext::Local(SecurityContext));
					} else if constexpr (std::is_same_v<ChangeType, AttributeTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
						if (!InstanceValue) return {MutationStatus::StaleObject, {}, "History attribute target is stale"};
						auto Expected = Forward ? Typed.Before : Typed.After;
						if (Expected) Expected = ResolveWireReferences(*Expected, History);
						if (InstanceValue->GetAttributeValue(Typed.AttributeName, ScriptSecurityContext::CoreTrusted()) != Expected)
							return {MutationStatus::ValidationFailed, {}, "History attribute state has diverged"};
						auto Value = Forward ? Typed.After : Typed.Before;
						if (Value) Value = ResolveWireReferences(*Value, History);
						return Gateway.Apply(UpdateAttributeCommand{Object, Typed.AttributeName, std::move(Value)},
							MutationAuthorityContext::Local(SecurityContext));
					} else if constexpr (std::is_same_v<ChangeType, TagTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
						if (!InstanceValue || InstanceValue->GetDataModel().get() != &World)
							return {MutationStatus::StaleObject, {}, "History tag target is stale"};
						const bool Expected = Forward ? Typed.Before : Typed.After;
						if (World.Tags.Has(World.GetObjectId(), Object, Typed.TagName, ScriptSecurityContext::CoreTrusted()) != Expected)
							return {MutationStatus::ValidationFailed, {}, "History tag state has diverged"};
						const bool Value = Forward ? Typed.After : Typed.Before;
						if (Value) return Gateway.Apply(AddTagCommand{Object, Typed.TagName, World.GetObjectId()},
							MutationAuthorityContext::Local(SecurityContext));
						return Gateway.Apply(RemoveTagCommand{Object, Typed.TagName, World.GetObjectId()},
							MutationAuthorityContext::Local(SecurityContext));
					} else if constexpr (std::is_same_v<ChangeType, ExtensionTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
						if (!InstanceValue) return {MutationStatus::StaleObject, {}, "History extension target is stale"};
						try {
							const auto Expected = ResolveWireReferences(Forward ? Typed.Before : Typed.After, History);
							if (InstanceValue->GetExtensionPropertyValue(Typed.ExtensionSchemaId, Typed.PropertyName,
								ScriptSecurityContext::CoreTrusted()) != Expected)
								return {MutationStatus::ValidationFailed, {}, "History extension state has diverged"};
						} catch (...) { return {MutationStatus::ValidationFailed, {}, "History extension schema is incompatible"}; }
						return Gateway.Apply(UpdateExtensionPropertyCommand{Object, Typed.ExtensionSchemaId,
							Typed.DefinitionVersion, Typed.PropertyName,
							ResolveWireReferences(Forward ? Typed.After : Typed.Before, History)},
							MutationAuthorityContext::Local(SecurityContext));
					} else if constexpr (std::is_same_v<ChangeType, ReparentTransactionChange>) {
						auto Object = History.ResolveIdentity(Typed.Object);
						auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
						if (!InstanceValue) return {MutationStatus::StaleObject, {}, "History reparent target is stale"};
						auto ExpectedParent = Forward ? Typed.BeforeParent : Typed.AfterParent;
						if (ExpectedParent) ExpectedParent = History.ResolveIdentity(*ExpectedParent);
						auto CurrentParent = InstanceValue->GetParent();
						const auto Current = CurrentParent ? std::optional((*CurrentParent)->GetObjectId()) : std::nullopt;
						if (Current != ExpectedParent)
							return {MutationStatus::ValidationFailed, {}, "History hierarchy state has diverged"};
						auto Parent = Forward ? Typed.AfterParent : Typed.BeforeParent;
						if (Parent) Parent = History.ResolveIdentity(*Parent);
						return Gateway.Apply(ReparentObjectCommand{Object, Parent}, MutationAuthorityContext::Local(SecurityContext));
					} else {
						const bool Restore = Typed.Kind == SubtreeTransactionKind::Destroy ? !Forward : Forward;
						if (Restore) {
							if (!Typed.Parent) return {MutationStatus::InvalidParent, {}, "History subtree has no restorable parent"};
							auto Result = Gateway.Apply(RestoreSubtreeCommand{Typed.PersistentSnapshot,
								History.ResolveIdentity(*Typed.Parent), Typed.Objects.size()},
								MutationAuthorityContext::Local(SecurityContext));
							if (!Result.Succeeded() || !Result.Object) return Result;
							auto Root = ObjectRegistry::Get().Lookup(*Result.Object);
							std::vector<std::shared_ptr<Instance>> Nodes{Root};
							Root->CollectDescendants(Nodes);
							if (Nodes.size() != Typed.Objects.size())
								throw std::logic_error("Validated history subtree shape changed during attachment");
							for (std::size_t Index = 0; Index < Nodes.size(); ++Index)
								History.RemapIdentity(Typed.Objects[Index], Nodes[Index]->GetObjectId());
							return Result;
						}
						return Gateway.Apply(DestroyObjectCommand{History.ResolveIdentity(Typed.Root)},
							MutationAuthorityContext::Local(SecurityContext));
					}
				}, Change);
			};

			bool Changed = false;
			std::vector<BufferedChangeRecord> JournalRecords;
			std::vector<const TransactionChange *> AppliedChanges;
			{
				ScopedAuthoritativeRevisionDeferral RevisionDeferral(World, Changed);
				ScopedChangeJournalCapture JournalCapture;
				auto RollBack = [&](bool AppliedForward) {
					for (auto It = AppliedChanges.rbegin(); It != AppliedChanges.rend(); ++It) {
						auto RollbackResult = ApplyChange(**It, !AppliedForward);
						if (!RollbackResult.Succeeded())
							throw std::logic_error("History execution rollback failed after preflight divergence");
					}
				};
				if (Redo) {
					for (const auto &Change : Transaction->Changes) {
						auto Result = ApplyChange(Change, true);
						if (!Result.Succeeded()) {
							RollBack(true);
							(void)JournalCapture.Take();
							return {.Status = TransactionStatus::ExecutionFailed, .Id = Transaction->Id, .Message = Result.Message};
						}
						AppliedChanges.push_back(&Change);
					}
				} else {
					for (auto It = Transaction->Changes.rbegin(); It != Transaction->Changes.rend(); ++It) {
						auto Result = ApplyChange(*It, false);
						if (!Result.Succeeded()) {
							RollBack(false);
							(void)JournalCapture.Take();
							return {.Status = TransactionStatus::ExecutionFailed, .Id = Transaction->Id, .Message = Result.Message};
						}
						AppliedChanges.push_back(&*It);
					}
				}
				JournalRecords = JournalCapture.Take();
			}
			if (!Changed) return {.Status = TransactionStatus::ExecutionFailed, .Id = Transaction->Id,
				.Message = "History execution produced no authoritative change"};
			try {
				std::vector<std::pair<ObjectId, std::size_t>> ScopeCounts;
				for (const auto &Record : JournalRecords) {
					auto Found = std::find_if(ScopeCounts.begin(), ScopeCounts.end(), [&](const auto &Entry) { return Entry.first == Record.Scope; });
					if (Found == ScopeCounts.end()) ScopeCounts.emplace_back(Record.Scope, 1); else ++Found->second;
				}
				for (const auto &[Scope, Count] : ScopeCounts) ChangeJournal::Get().EnsureCanCommit(Scope, Count);
				auto Result = Redo ? History.CompleteRedo(World) : History.CompleteUndo(World);
				if (!Result.Succeeded()) return Result;
				PublishCapturedJournal(std::move(JournalRecords));
				return Result;
			} catch (const std::exception &Error) {
				return {.Status = TransactionStatus::ExecutionFailed, .Id = Transaction->Id, .Message = Error.what()};
			}
		}
	}

	TransactionResult MutationGateway::Undo(DataModel &World, ScriptSecurityContext SecurityContext) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return {.Status = TransactionStatus::InvalidState, .Message = "Undo requires Main"};
		if (!SecurityContext.HasCapability(ScriptCapability::MutateDataModel))
			return {.Status = TransactionStatus::InvalidState, .Message = "Undo requires authoring mutation authority"};
		return ExecuteHistory(*this, World, std::move(SecurityContext), false);
	}

	TransactionResult MutationGateway::Redo(DataModel &World, ScriptSecurityContext SecurityContext) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main)
			return {.Status = TransactionStatus::InvalidState, .Message = "Redo requires Main"};
		if (!SecurityContext.HasCapability(ScriptCapability::MutateDataModel))
			return {.Status = TransactionStatus::InvalidState, .Message = "Redo requires authoring mutation authority"};
		return ExecuteHistory(*this, World, std::move(SecurityContext), true);
	}

	std::size_t MutationGateway::Drain(std::size_t maximumCommands) {
		AssertAuthoritativeMutation("MutationGateway::Drain");
		std::size_t count = 0;
		while (count < maximumCommands) {
			std::optional<PendingMutation> pending;
			{
				std::scoped_lock lock(Mutex);
				if (Pending.empty()) break;
				pending.emplace(std::move(Pending.front()));
				Pending.pop_front();
			}
			pending->Completion->Complete(Apply(std::move(pending->Command), std::move(pending->Authority)));
			++count;
		}
		return count;
	}

	std::size_t MutationGateway::GetPendingCount() const {
		std::scoped_lock lock(Mutex);
		return Pending.size();
	}
}

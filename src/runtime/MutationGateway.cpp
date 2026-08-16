#include "gargantuan/runtime/MutationGateway.hpp"

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
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
					if constexpr (std::is_same_v<Command, CreateObjectCommand>) {
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
						if constexpr (std::is_same_v<Command, UpdatePropertyCommand>) {
							auto DataModelValue = InstanceValue->GetDataModel();
							auto *Property = InstanceValue->FindProperty(CommandValue.PropertyName);
							auto Before = InstanceValue->ReadPropertyWireValue(CommandValue.PropertyName)
											  .value_or(WireValue(std::monostate{}));
							PropertyTransactionChange Prototype{
								CommandValue.Object,
								Property ? Property->DeclaringSchemaId : SchemaId{},
								Property ? Property->DeclaringDefinitionVersion : 0,
								CommandValue.PropertyName,
								Before,
								EncodeNativeWireValue(CommandValue.Value).value_or(Before),
							};
							return ApplyAuthoringTransaction(
								DataModelValue,
								"Set " + CommandValue.PropertyName,
								Prototype,
								[&] {
							const auto Status = InstanceValue->ApplyPropertyMutation(
								CommandValue.PropertyName, CommandValue.Value, Enums::Permission::None, securityContext
							);
							return MutationResult {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Property mutation rejected"
							};
						},
								[&](const MutationResult &) -> TransactionChange {
									auto Committed = Prototype;
									Committed.After = InstanceValue->ReadPropertyWireValue(CommandValue.PropertyName)
														  .value_or(Committed.After);
									return Committed;
								}
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
							if (Parent->GetDataModel() != DataModelValue || WouldCreateCycle(InstanceValue, Parent))
								return {
									MutationStatus::InvalidParent,
									std::nullopt,
									"Parent is outside scope or creates a hierarchy cycle"
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
									for (const auto &[Node, Tags] : State.PendingTags)
										for (const auto &Tag : Tags)
											(void)DataModelValue->Tags.Add(
												DataModelValue->GetObjectId(),
												Node->GetObjectId(),
												Tag,
												ScriptSecurityContext::CoreTrusted()
											);
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

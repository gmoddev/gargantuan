#include "gargantuan/runtime/MutationGateway.hpp"

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

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
		std::optional<ObjectId> ScopeValue
	)
		: Origin(OriginValue), SecurityContext(std::move(SecurityContextValue)), Scope(ScopeValue) {
		if (Scope && !Scope->IsValid()) throw std::invalid_argument("Mutation authority scope is invalid");
	}

	MutationAuthorityContext MutationAuthorityContext::Local(ScriptSecurityContext SecurityContext) {
		return {MutationCommandOrigin::LocalInternal, std::move(SecurityContext), std::nullopt};
	}

	MutationAuthorityContext MutationAuthorityContext::Studio(ScriptSecurityContext SecurityContext, ObjectId Scope) {
		if (SecurityContext.Domain != ScriptExecutionDomain::Studio)
			throw std::invalid_argument("Studio mutation authority requires the Studio execution domain");
		return {MutationCommandOrigin::Studio, std::move(SecurityContext), Scope};
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
						DataModelValue->BeginAuthoritativeRevisionBatch();
						try {
							{
								ScopedChangeJournalSuppression Suppression;
								InstanceValue->SetParent(Parent);
							}
							const auto Id = InstanceValue->GetObjectId();
							InstanceValue->PublishReplicationSubtree(Scope);
							DataModelValue->CommitAuthoritativeRevisionBatch();
							return {MutationStatus::Success, Id, {}};
						} catch (...) {
							{
								ScopedChangeJournalSuppression Suppression;
								InstanceValue->Destroy();
							}
							DataModelValue->CancelAuthoritativeRevisionBatch();
							throw;
						}
					} else {
						auto InstanceValue = ObjectRegistry::Get().Lookup(CommandValue.Object);
						if (!InstanceValue)
							return {MutationStatus::StaleObject, std::nullopt, "Object identity is stale"};
						if (Authority.GetScope() && InstanceValue->GetReplicationScopeId() != *Authority.GetScope())
							return {MutationStatus::Rejected, std::nullopt, "Object is outside the project scope"};
						if constexpr (std::is_same_v<Command, UpdatePropertyCommand>) {
							const auto Status = InstanceValue->ApplyPropertyMutation(
								CommandValue.PropertyName, CommandValue.Value, Enums::Permission::None, securityContext
							);
							return {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Property mutation rejected"
							};
						} else if constexpr (std::is_same_v<Command, UpdateAttributeCommand>) {
							const auto Status = InstanceValue->ApplyAttributeMutation(
								CommandValue.AttributeName, std::move(CommandValue.Value), securityContext
							);
							return {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Attribute mutation rejected"
							};
						} else if constexpr (std::is_same_v<Command, UpdateExtensionPropertyCommand>) {
							const auto Status = InstanceValue->ApplyExtensionPropertyMutation(
								CommandValue.ExtensionSchemaId,
								CommandValue.DefinitionVersion,
								CommandValue.PropertyName,
								std::move(CommandValue.Value),
								securityContext
							);
							return {
								Status,
								CommandValue.Object,
								Status == MutationStatus::Success ? "" : "Extension property mutation rejected"
							};
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
							if constexpr (std::is_same_v<Command, AddTagCommand>)
								(void)DataModelValue->Tags.Add(
									Scope, CommandValue.Object, CommandValue.TagName, securityContext
								);
							else
								(void)DataModelValue->Tags.Remove(
									Scope, CommandValue.Object, CommandValue.TagName, securityContext
								);
							return {MutationStatus::Success, CommandValue.Object, {}};
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
							DataModelValue->BeginAuthoritativeRevisionBatch();
							try {
								InstanceValue->SetParent(Parent);
								DataModelValue->CommitAuthoritativeRevisionBatch();
							} catch (...) {
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
							return {MutationStatus::Success, CommandValue.Object, {}};
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
							DataModelValue->BeginAuthoritativeRevisionBatch();
							try {
								InstanceValue->Destroy();
								DataModelValue->CommitAuthoritativeRevisionBatch();
							} catch (...) {
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
							return {MutationStatus::Success, CommandValue.Object, {}};
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
								return {MutationStatus::Success, Id, {}};
							} catch (...) {
								{
									ScopedChangeJournalSuppression Suppression;
									Clone->Destroy();
								}
								DataModelValue->CancelAuthoritativeRevisionBatch();
								throw;
							}
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

#include "gargantuan/network/ReplicaApplier.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/Enum.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "runtime/SnapshotValidation.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>
#include <vector>

namespace gargantuan::network {
	namespace {
		SnapshotObject ToSnapshotObject(const PublishReplication &Publish) {
			return {
				WireObjectId::FromObjectId(Publish.Object),
				Publish.ClassSchemaId,
				Publish.DefinitionVersion,
				Publish.ClassName,
				Publish.Name,
				Publish.Parent ? std::optional(WireObjectId::FromObjectId(*Publish.Parent)) : std::nullopt,
				Publish.Properties,
				Publish.Attributes,
				Publish.Extensions,
				Publish.CustomProperties,
				Publish.Tags,
			};
		}

		void ApplyExtensionUpdate(SnapshotObject &Object, const ExtensionPropertyReplicationUpdate &Update) {
			const auto &Schema = GetActiveRuntimeSchemaRegistry();
			auto *Definition = Schema.FindExtensionById(Update.ExtensionSchemaId);
			auto *Property = Schema.FindExtensionProperty(Update.ExtensionSchemaId, Update.PropertyName);
			if (!Definition || Definition->DefinitionVersion != Update.DefinitionVersion || !Property ||
				!Schema.IsExtensionApplicableToClass(Update.ExtensionSchemaId, Object.ClassSchemaId))
				throw std::invalid_argument("Extension identity, version, property, or target is incompatible");
			(void)ValidateSchemaExtensionPropertyValue(Property->Type, Update.Value);
			auto State = std::ranges::find(
				Object.Extensions, Update.ExtensionSchemaId, &SnapshotExtensionState::ExtensionSchemaId
			);
			if (Update.Value == Property->DefaultValue) {
				if (State == Object.Extensions.end() || !State->Properties.erase(Update.PropertyName))
					throw std::invalid_argument("Extension update removes an absent override");
				if (State->Properties.empty()) Object.Extensions.erase(State);
				return;
			}
			if (State == Object.Extensions.end()) {
				Object.Extensions.push_back(
					{Update.ExtensionSchemaId, Update.DefinitionVersion, {{Update.PropertyName, Update.Value}}}
				);
				std::ranges::sort(Object.Extensions, {}, &SnapshotExtensionState::ExtensionSchemaId);
			} else {
				if (State->DefinitionVersion != Update.DefinitionVersion)
					throw std::invalid_argument("Extension state version is incompatible");
				State->Properties[Update.PropertyName] = Update.Value;
			}
		}

		void ApplyPropertyUpdate(SnapshotObject &Object, const PropertyReplicationUpdate &Update) {
			if (!Update.DeclaringClassSchemaId) {
				Object.Properties[Update.PropertyName] = Update.Value;
				return;
			}
			const auto &Schema = GetActiveRuntimeSchemaRegistry();
			auto *Definition = Schema.FindClassById(*Update.DeclaringClassSchemaId);
			auto *Property = Schema.FindCustomClassProperty(*Update.DeclaringClassSchemaId, Update.PropertyName);
			if (!Definition || !Update.DefinitionVersion ||
				Definition->DefinitionVersion != *Update.DefinitionVersion || !Property ||
				!Schema.IsClassDerivedFrom(Object.ClassSchemaId, *Update.DeclaringClassSchemaId))
				throw std::invalid_argument("Custom property identity, version, or target is incompatible");
			(void)ValidateSchemaExtensionPropertyValue(Property->Type, Update.Value);
			auto State = std::ranges::find(
				Object.CustomProperties,
				*Update.DeclaringClassSchemaId,
				&SnapshotCustomClassState::DeclaringClassSchemaId
			);
			if (Update.Value == Property->DefaultValue) {
				if (State == Object.CustomProperties.end() || !State->Properties.erase(Update.PropertyName))
					throw std::invalid_argument("Custom property update removes an absent override");
				if (State->Properties.empty()) Object.CustomProperties.erase(State);
				return;
			}
			if (State == Object.CustomProperties.end()) {
				Object.CustomProperties.push_back(
					{*Update.DeclaringClassSchemaId, *Update.DefinitionVersion, {{Update.PropertyName, Update.Value}}}
				);
				std::ranges::sort(Object.CustomProperties, {}, &SnapshotCustomClassState::DeclaringClassSchemaId);
			} else {
				if (State->DefinitionVersion != *Update.DefinitionVersion)
					throw std::invalid_argument("Custom property state version is incompatible");
				State->Properties[Update.PropertyName] = Update.Value;
			}
		}

		void ApplyPropertyToReplica(
			const PropertyReplicationUpdate &Update,
			const std::shared_ptr<Instance> &InstanceValue,
			const SnapshotLoadResult &Receiver
		) {
			auto *Property = InstanceValue->FindProperty(Update.PropertyName);
			if (!Property || Property->ReplicationPolicy != InstanceProperty::Replication::FutureReplicated ||
				!Property->Write)
				throw std::invalid_argument(
					"Replica property '" + Update.PropertyName + "' is unknown, read-only, or not replicated"
				);
			if (Update.DeclaringClassSchemaId) {
				if (!Update.DefinitionVersion || !Property->CustomSchemaPropertyType ||
					Property->DeclaringSchemaId != *Update.DeclaringClassSchemaId ||
					Property->DeclaringDefinitionVersion != *Update.DefinitionVersion ||
					InstanceValue->ApplyCustomClassPropertyMutation(
						*Update.DeclaringClassSchemaId,
						*Update.DefinitionVersion,
						Update.PropertyName,
						Update.Value,
						ScriptSecurityContext::CoreTrusted()
					) != MutationStatus::Success)
					throw std::invalid_argument("Replica rejected custom property state");
				return;
			}
			if (Property->CustomSchemaPropertyType)
				throw std::invalid_argument("Replica custom property identity is missing");
			if (std::holds_alternative<std::monostate>(Update.Value) ||
				std::holds_alternative<WireObjectReference>(Update.Value)) {
				if (!Property->WriteObjectReference)
					throw std::invalid_argument("Replica WireValue is not a reference property");
				std::shared_ptr<Instance> Referenced;
				if (const auto *Reference = std::get_if<WireObjectReference>(&Update.Value)) {
					Referenced = Receiver.Resolve(Reference->Object);
					if (!Referenced) throw std::invalid_argument("Replica reference is not materialized");
				}
				Property->WriteObjectReference(InstanceValue.get(), Referenced);
				return;
			}
			if (const auto *EnumValue = std::get_if<WireEnumItem>(&Update.Value)) {
				if (!Property->WriteEnumValue || Property->ReflectedTypedef != "Enum." + EnumValue->EnumType)
					throw std::invalid_argument("Replica enum property type is incompatible");
				auto Type = Enums::GetEnums().find(EnumValue->EnumType);
				if (Type == Enums::GetEnums().end()) throw std::invalid_argument("Replica enum type is unknown");
				auto Item = Type->second->FromName(EnumValue->Item);
				if (!Item) throw std::invalid_argument("Replica enum item is unknown");
				Property->WriteEnumValue(InstanceValue.get(), Item->Value);
				return;
			}
			auto Native = DecodeNativeWireValue(Update.Value);
			if (!Native ||
				InstanceValue->ApplyPropertyMutation(Update.PropertyName, *Native, Enums::Permission::Engine) !=
					MutationStatus::Success)
				throw std::invalid_argument("Replica rejected property WireValue");
		}

		void ApplyPublishToReplica(
			const PublishReplication &Publish,
			const std::shared_ptr<Instance> &InstanceValue,
			SnapshotLoadResult &Receiver
		) {
			InstanceValue->SetName(Publish.Name);
			for (const auto &[Name, Value] : Publish.Properties)
				ApplyPropertyToReplica(PropertyReplicationUpdate{Publish.Object, Name, Value}, InstanceValue, Receiver);
			for (const auto &[Name, Value] : Publish.Attributes)
				if (InstanceValue->ApplyAttributeMutation(Name, Value, ScriptSecurityContext::CoreTrusted()) !=
					MutationStatus::Success)
					throw std::invalid_argument("Replica rejected published Attribute");
			for (const auto &State : Publish.Extensions)
				for (const auto &[Name, Value] : State.Properties)
					if (InstanceValue->ApplyExtensionPropertyMutation(
							State.ExtensionSchemaId,
							State.DefinitionVersion,
							Name,
							Value,
							ScriptSecurityContext::CoreTrusted()
						) != MutationStatus::Success)
						throw std::invalid_argument("Replica rejected published extension property");
			for (const auto &State : Publish.CustomProperties)
				for (const auto &[Name, Value] : State.Properties)
					if (InstanceValue->ApplyCustomClassPropertyMutation(
							State.DeclaringClassSchemaId,
							State.DefinitionVersion,
							Name,
							Value,
							ScriptSecurityContext::CoreTrusted()
						) != MutationStatus::Success)
						throw std::invalid_argument("Replica rejected published custom property");
			auto DataModelValue = std::dynamic_pointer_cast<DataModel>(Receiver.Root);
			if (!DataModelValue) throw std::invalid_argument("Replica root is not a DataModel");
			for (const auto &Tag : Publish.Tags)
				if (!DataModelValue->Tags.Add(
						DataModelValue->GetObjectId(),
						InstanceValue->GetObjectId(),
						Tag,
						ScriptSecurityContext::CoreTrusted()
					))
					throw std::invalid_argument("Replica rejected published Tag");
			if (Publish.Parent) {
				auto Parent = Receiver.Resolve(WireObjectId::FromObjectId(*Publish.Parent));
				if (!Parent) throw std::invalid_argument("Replica publish parent is not materialized");
				InstanceValue->SetParent(Parent);
			}
		}

		void RemoveReplicaObject(
			SnapshotLoadResult &Receiver, ObjectId Object, std::map<const Instance *, WireObjectId> &ReplicaIds
		) {
			auto Target = Receiver.Resolve(WireObjectId::FromObjectId(Object));
			if (!Target) throw std::invalid_argument("Replica removal target is not materialized");
			std::vector<std::shared_ptr<Instance>> Removed{Target};
			for (std::size_t Index = 0; Index < Removed.size(); ++Index)
				Removed.insert(Removed.end(), Removed[Index]->Children.begin(), Removed[Index]->Children.end());
			Target->Destroy();
			for (const auto &Value : Removed) {
				auto Id = ReplicaIds.find(Value.get());
				if (Id == ReplicaIds.end()) throw std::logic_error("Replica identity index is incomplete");
				Receiver.Objects.erase(Id->second);
				ReplicaIds.erase(Id);
			}
		}
	}

	ReplicaApplyResult ReplicaApplier::ApplyBytes(std::span<const std::byte> Bytes) {
		auto Decoded = DecodeReplicationFrame(Bytes);
		if (!Decoded) return {ReplicaApplyStatus::MalformedFrame, 0, Decoded.error().Format()};
		return ApplyFrame(*Decoded);
	}

	ReplicaApplyResult ReplicaApplier::ApplyFrame(const ReplicationFrame &Frame) {
		if (!Frame.IsValid()) return {ReplicaApplyStatus::MalformedFrame, 0, "Replication frame is invalid"};
		if (Frame.Kind == ReplicationMessageKind::Baseline) {
			if (Frame.Sequence.Value() != 1)
				return {ReplicaApplyStatus::OutOfOrderSequence, 0, "Baseline sequence must begin at one"};
			if (Epoch.IsValid() && !Frame.Epoch.IsNewerThan(Epoch)) {
				++Metrics.RejectedStaleOperations;
				return {ReplicaApplyStatus::StaleEpoch, 0, "Replication baseline epoch is stale"};
			}
			if (!IsReplicationSchemaCompatible(Frame.Schema))
				return {ReplicaApplyStatus::SchemaMismatch, 0, "Replication schema manifest is incompatible"};
		} else {
			if (!Epoch.IsValid() || Frame.Epoch != Epoch) {
				++Metrics.RejectedStaleOperations;
				return {ReplicaApplyStatus::StaleEpoch, 0, "Replication epoch is stale or unknown"};
			}
			if (Frame.Sequence.Value() < NextSequence.Value()) {
				++Metrics.RejectedStaleOperations;
				return {ReplicaApplyStatus::StaleSequence, 0, "Reliable replication sequence is stale"};
			}
			if (Frame.Sequence != NextSequence)
				return {ReplicaApplyStatus::OutOfOrderSequence, 0, "Reliable replication sequence is out of order"};
		}
		auto FollowingSequence = Frame.Sequence.TryNext();
		if (!FollowingSequence)
			return {ReplicaApplyStatus::SemanticRejection, 0, "Reliable replication sequence is exhausted"};

		Snapshot Candidate;
		if (Frame.Kind == ReplicationMessageKind::Baseline) {
			Candidate.Version = SnapshotFormatVersion;
			Candidate.Cursor.NextSequence = 1;
			for (const auto &Operation : Frame.Operations) {
				const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent);
				if (!Publish)
					return {ReplicaApplyStatus::SemanticRejection, 0, "Baseline contains a non-publish operation"};
				Candidate.Objects.push_back(ToSnapshotObject(*Publish));
			}
			auto Root = std::ranges::find_if(Candidate.Objects, [](const auto &Object) { return !Object.Parent; });
			if (Root == Candidate.Objects.end())
				return {ReplicaApplyStatus::SemanticRejection, 0, "Baseline has no root object"};
			Candidate.Cursor.Scope = Root->Id.ToObjectId();
		} else
			Candidate = SemanticState;

		std::map<WireObjectId, std::size_t> CandidateIndex;
		std::map<WireObjectId, std::set<WireObjectId>> CandidateChildren;
		std::set<WireObjectId> RemovedCandidateObjects;
		std::set<WireObjectId> CandidateLifecycleObjects;
		if (Frame.Kind == ReplicationMessageKind::Incremental) {
			for (std::size_t Index = 0; Index < Candidate.Objects.size(); ++Index) {
				CandidateIndex.emplace(Candidate.Objects[Index].Id, Index);
				if (Candidate.Objects[Index].Parent)
					CandidateChildren[*Candidate.Objects[Index].Parent].insert(Candidate.Objects[Index].Id);
			}
		}
		auto FindCandidate = [&](ObjectId Id) -> SnapshotObject * {
			auto Found = CandidateIndex.find(WireObjectId::FromObjectId(Id));
			return Found == CandidateIndex.end() ? nullptr : &Candidate.Objects[Found->second];
		};
		auto RemoveCandidate = [&](ObjectId Id) {
			const auto Root = WireObjectId::FromObjectId(Id);
			if (!CandidateIndex.contains(Root)) return false;
			std::vector<WireObjectId> Pending{Root};
			for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
				auto Children = CandidateChildren.find(Pending[Index]);
				if (Children != CandidateChildren.end())
					Pending.insert(Pending.end(), Children->second.begin(), Children->second.end());
			}
			for (const auto &Removed : Pending) {
				auto Found = CandidateIndex.find(Removed);
				if (Found == CandidateIndex.end()) continue;
				const auto &Object = Candidate.Objects[Found->second];
				if (Object.Parent) CandidateChildren[*Object.Parent].erase(Removed);
				CandidateChildren.erase(Removed);
				CandidateIndex.erase(Found);
				RemovedCandidateObjects.insert(Removed);
			}
			return true;
		};

		bool LiveCommitStarted = false;
		try {
			for (const auto &Operation : Frame.Operations) {
				if (Frame.Kind == ReplicationMessageKind::Baseline) continue;
				std::visit(
					[&](const auto &Value) {
						using Type = std::decay_t<decltype(Value)>;
						if constexpr (std::is_same_v<Type, PublishReplication>) {
							if (!CandidateLifecycleObjects.insert(WireObjectId::FromObjectId(Value.Object)).second)
								throw std::invalid_argument("Object has multiple lifecycle operations in one frame");
							if (FindCandidate(Value.Object))
								throw std::invalid_argument("Publish targets an already materialized object");
							if (Value.Parent && !FindCandidate(*Value.Parent))
								throw std::invalid_argument("Publish parent is not materialized");
							Candidate.Objects.push_back(ToSnapshotObject(Value));
							CandidateIndex.emplace(Candidate.Objects.back().Id, Candidate.Objects.size() - 1);
							if (Candidate.Objects.back().Parent)
								CandidateChildren[*Candidate.Objects.back().Parent].insert(Candidate.Objects.back().Id);
						} else if constexpr (std::is_same_v<Type, UnpublishReplication> ||
											 std::is_same_v<Type, DestroyReplication>) {
							if (!CandidateLifecycleObjects.insert(WireObjectId::FromObjectId(Value.Object)).second)
								throw std::invalid_argument("Object has multiple lifecycle operations in one frame");
							if (!RemoveCandidate(Value.Object))
								throw std::invalid_argument("Removal targets an unpublished object");
						} else {
							auto *Object = FindCandidate(Value.Object);
							if (!Object)
								throw std::invalid_argument("Replication operation targets an unpublished object");
							if constexpr (std::is_same_v<Type, PropertyReplicationUpdate>)
								ApplyPropertyUpdate(*Object, Value);
							else if constexpr (std::is_same_v<Type, ExtensionPropertyReplicationUpdate>)
								ApplyExtensionUpdate(*Object, Value);
							else if constexpr (std::is_same_v<Type, ReparentReplication>) {
								if (Value.Parent && !FindCandidate(*Value.Parent))
									throw std::invalid_argument("Reparent target is unpublished");
								if (Object->Parent) CandidateChildren[*Object->Parent].erase(Object->Id);
								Object->Parent = Value.Parent ? std::optional(WireObjectId::FromObjectId(*Value.Parent))
															  : std::nullopt;
								if (Object->Parent) CandidateChildren[*Object->Parent].insert(Object->Id);
							} else if constexpr (std::is_same_v<Type, AttributeReplicationUpdate>) {
								if (Value.Value)
									Object->Attributes[Value.AttributeName] = *Value.Value;
								else if (!Object->Attributes.erase(Value.AttributeName))
									throw std::invalid_argument("Attribute removal targets an absent value");
							} else if constexpr (std::is_same_v<Type, TagAddedReplication>) {
								if (std::ranges::find(Object->Tags, Value.TagName) != Object->Tags.end())
									throw std::invalid_argument("Tag add duplicates membership");
								Object->Tags.push_back(Value.TagName);
								std::ranges::sort(Object->Tags);
							} else if constexpr (std::is_same_v<Type, TagRemovedReplication>) {
								if (!std::erase(Object->Tags, Value.TagName))
									throw std::invalid_argument("Tag removal targets absent membership");
							}
						}
					},
					Operation.Intent
				);
			}
			if (!RemovedCandidateObjects.empty())
				std::erase_if(Candidate.Objects, [&](const auto &Object) {
					return RemovedCandidateObjects.contains(Object.Id);
				});
			ValidateSnapshotSemantic(Candidate);
			SnapshotLoadResult Loaded;
			{
				ScopedChangeJournalSuppression SuppressReplicaJournal;
				Loaded = LoadSnapshot(Candidate);
			}
			if (!Loaded.Succeeded())
				throw std::invalid_argument(
					Loaded.Errors.empty() ? "Replica materialization failed" : Loaded.Errors.front()
				);
			if (Frame.Kind == ReplicationMessageKind::Incremental) {
				LiveCommitStarted = true;
				ScopedChangeJournalSuppression SuppressReplicaJournal;
				ScopedSignalDeferral DeferNotifications;
				std::map<const Instance *, WireObjectId> ReplicaIds;
				for (const auto &[Id, Value] : Receiver.Objects)
					ReplicaIds.emplace(Value.get(), Id);
				for (const auto &Operation : Frame.Operations)
					if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent)) {
						auto *Definition = InstanceClassRegistry::GetDefinitionBySchemaId(Publish->ClassSchemaId);
						const auto ExpectedName = Definition && Definition->ConstructionKind ==
																	SchemaClassConstructionKind::CustomData
													  ? Definition->CanonicalName
												  : Definition ? Definition->ClassName
															   : std::string{};
						if (!Definition || Definition->DefinitionVersion != Publish->DefinitionVersion ||
							ExpectedName != Publish->ClassName || !InstanceClassRegistry::IsConstructible(*Definition))
							throw std::invalid_argument("Replica publish class identity is incompatible");
						auto InstanceValue = InstanceClassRegistry::Construct(*Definition);
						const auto WireId = WireObjectId::FromObjectId(Publish->Object);
						if (!InstanceValue || !Receiver.Objects.emplace(WireId, InstanceValue).second ||
							!ReplicaIds.emplace(InstanceValue.get(), WireId).second)
							throw std::invalid_argument("Replica publish construction failed");
					}
				for (const auto &Operation : Frame.Operations)
					std::visit(
						[&](const auto &Value) {
							using Type = std::decay_t<decltype(Value)>;
							if constexpr (std::is_same_v<Type, PublishReplication>) {
								ApplyPublishToReplica(
									Value, Receiver.Resolve(WireObjectId::FromObjectId(Value.Object)), Receiver
								);
							} else if constexpr (std::is_same_v<Type, UnpublishReplication> ||
												 std::is_same_v<Type, DestroyReplication>) {
								RemoveReplicaObject(Receiver, Value.Object, ReplicaIds);
							} else {
								auto InstanceValue = Receiver.Resolve(WireObjectId::FromObjectId(Value.Object));
								if (!InstanceValue)
									throw std::invalid_argument("Replica operation target is not materialized");
								if constexpr (std::is_same_v<Type, PropertyReplicationUpdate>)
									ApplyPropertyToReplica(Value, InstanceValue, Receiver);
								else if constexpr (std::is_same_v<Type, ExtensionPropertyReplicationUpdate>) {
									if (InstanceValue->ApplyExtensionPropertyMutation(
											Value.ExtensionSchemaId,
											Value.DefinitionVersion,
											Value.PropertyName,
											Value.Value,
											ScriptSecurityContext::CoreTrusted()
										) != MutationStatus::Success)
										throw std::invalid_argument("Replica rejected extension update");
								} else if constexpr (std::is_same_v<Type, ReparentReplication>) {
									std::shared_ptr<Instance> Parent;
									if (Value.Parent) {
										Parent = Receiver.Resolve(WireObjectId::FromObjectId(*Value.Parent));
										if (!Parent)
											throw std::invalid_argument("Replica reparent parent is not materialized");
									}
									InstanceValue->SetParent(Parent ? std::optional(Parent) : std::nullopt);
								} else if constexpr (std::is_same_v<Type, AttributeReplicationUpdate>) {
									if (InstanceValue->ApplyAttributeMutation(
											Value.AttributeName, Value.Value, ScriptSecurityContext::CoreTrusted()
										) != MutationStatus::Success)
										throw std::invalid_argument("Replica rejected Attribute update");
								} else if constexpr (std::is_same_v<Type, TagAddedReplication> ||
													 std::is_same_v<Type, TagRemovedReplication>) {
									auto DataModelValue = std::dynamic_pointer_cast<DataModel>(Receiver.Root);
									if (!DataModelValue) throw std::invalid_argument("Replica root is not a DataModel");
									const bool Changed = std::is_same_v<Type, TagAddedReplication>
															 ? DataModelValue->Tags.Add(
																   DataModelValue->GetObjectId(),
																   InstanceValue->GetObjectId(),
																   Value.TagName,
																   ScriptSecurityContext::CoreTrusted()
															   )
															 : DataModelValue->Tags.Remove(
																   DataModelValue->GetObjectId(),
																   InstanceValue->GetObjectId(),
																   Value.TagName,
																   ScriptSecurityContext::CoreTrusted()
															   );
									if (!Changed) throw std::invalid_argument("Replica rejected Tag update");
								}
							}
						},
						Operation.Intent
					);
				DeferNotifications.Commit();
			} else
				Receiver = std::move(Loaded);
			SemanticState = std::move(Candidate);
			Epoch = Frame.Epoch;
			NextSequence = *FollowingSequence;
			++Metrics.FramesApplied;
			Metrics.OperationsApplied += Frame.Operations.size();
			return {ReplicaApplyStatus::Applied, Frame.Operations.size(), {}};
		} catch (const std::exception &Error) {
			if (LiveCommitStarted) {
				ScopedChangeJournalSuppression SuppressReplicaJournal;
				auto Restored = LoadSnapshot(SemanticState);
				if (Restored.Succeeded()) Receiver = std::move(Restored);
			}
			const std::string Message = Error.what();
			if (Message.find("reference") != std::string::npos || Message.find("parent") != std::string::npos)
				++Metrics.RejectedInvalidReferences;
			return {ReplicaApplyStatus::SemanticRejection, 0, Message};
		}
	}

	void ReplicaApplier::Reset() {
		SemanticState = {};
		Receiver = {};
		Epoch = {};
		NextSequence = {};
	}

	std::shared_ptr<Instance> ReplicaApplier::Resolve(ObjectId SourceObject) const {
		return Receiver.Resolve(WireObjectId::FromObjectId(SourceObject));
	}
}

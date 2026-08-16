#include "gargantuan/network/ReplicationCoordinator.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace gargantuan::network {
	namespace {
		PublishReplication MakePublish(const SnapshotObject &Object) {
			return {
				Object.Id.ToObjectId(),
				Object.ClassSchemaId,
				Object.ClassDefinitionVersion,
				Object.Parent ? std::optional(Object.Parent->ToObjectId()) : std::nullopt,
				Object.ClassName,
				Object.Name,
				Object.Properties,
				Object.Attributes,
				Object.Extensions,
				Object.CustomProperties,
				Object.Tags,
			};
		}

		std::map<ObjectId, const SnapshotObject *> IndexSnapshot(const Snapshot &SnapshotValue) {
			std::map<ObjectId, const SnapshotObject *> Result;
			for (const auto &Object : SnapshotValue.Objects)
				Result.emplace(Object.Id.ToObjectId(), &Object);
			return Result;
		}

		bool ReferencesKnown(const ReplicationView &View, const WireValue &Value) {
			if (const auto *Reference = std::get_if<WireObjectReference>(&Value))
				return View.Knows(Reference->Object.ToObjectId());
			return true;
		}

		bool PublishReferencesKnown(
			const ReplicationView &View,
			const PublishReplication &Publish,
			const std::set<ObjectId> &AdditionalKnown = {}
		) {
			auto Known = [&](const WireValue &Value) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				return !Reference || View.Knows(Reference->Object.ToObjectId()) ||
					   AdditionalKnown.contains(Reference->Object.ToObjectId());
			};
			for (const auto &[Name, Value] : Publish.Properties) {
				(void)Name;
				if (!Known(Value)) return false;
			}
			for (const auto &[Name, Value] : Publish.Attributes) {
				(void)Name;
				if (!Known(Value)) return false;
			}
			for (const auto &State : Publish.Extensions)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (!Known(Value)) return false;
				}
			for (const auto &State : Publish.CustomProperties)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (!Known(Value)) return false;
				}
			return true;
		}

		bool ReferencesAny(const SnapshotObject &Object, const std::set<ObjectId> &Targets) {
			auto Matches = [&](const WireValue &Value) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				return Reference && Targets.contains(Reference->Object.ToObjectId());
			};
			for (const auto &[Name, Value] : Object.Properties) {
				(void)Name;
				if (Matches(Value)) return true;
			}
			for (const auto &[Name, Value] : Object.Attributes) {
				(void)Name;
				if (Matches(Value)) return true;
			}
			for (const auto &State : Object.Extensions)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (Matches(Value)) return true;
				}
			for (const auto &State : Object.CustomProperties)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (Matches(Value)) return true;
				}
			return false;
		}
	}

	ReplicationCoordinator::ReplicationCoordinator(std::shared_ptr<Instance> SourceRoot)
		: SourceRoot(std::move(SourceRoot)) {}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(ConnectionId Connection, ReplicationEpoch Epoch) {
		if (!SourceRoot || !Connection.IsValid() || !Epoch.IsValid()) return {{}, "Invalid replication peer or source"};
		if (Peers.contains(Connection)) return {{}, "Replication peer is already registered"};
		for (const auto &[Existing, State] : Peers)
			if (Existing.Slot == Connection.Slot && State.View.Connection.IsValid())
				return {{}, "A live replication peer already owns this connection slot"};
		try {
			auto SnapshotValue = CaptureSnapshot(SourceRoot);
			if (SnapshotValue.Objects.size() > MaximumReplicationOperationsPerFrame)
				return {{}, "Replication baseline exceeds its object limit"};
			PeerState State{{Connection, Epoch}, SnapshotValue.Cursor, ReliableReplicationSequence(1)};
			ReplicationFrame Frame{
				ReplicationProtocolVersion, ReplicationMessageKind::Baseline, Epoch, State.NextSequence
			};
			Frame.Schema = CaptureReplicationSchemaCompatibility();
			Frame.Operations.reserve(SnapshotValue.Objects.size());
			for (const auto &Object : SnapshotValue.Objects) {
				const auto Id = Object.Id.ToObjectId();
				State.View.RelevantObjects.insert(Id);
				State.View.KnownObjects.insert(Id);
				Frame.Operations.push_back({Epoch, MakePublish(Object)});
			}
			auto Encoded = EncodeReplicationFrame(Frame);
			if (!Encoded) return {{}, Encoded.error().Format()};
			State.NextSequence = *State.NextSequence.TryNext();
			Metrics.ObjectsPublished += SnapshotValue.Objects.size();
			Metrics.OperationsGenerated += SnapshotValue.Objects.size();
			Metrics.BaselineObjects += SnapshotValue.Objects.size();
			Metrics.BaselineBytes += Encoded->size();
			Peers.emplace(Connection, std::move(State));
			return {std::move(Frame), {}};
		} catch (const std::exception &Error) {
			return {{}, Error.what()};
		}
	}

	ReplicationProduceResult
	ReplicationCoordinator::ProduceIncremental(ConnectionId Connection, std::size_t MaximumJournalRecords) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		auto CandidatePeer = Peer->second;
		auto CandidateMetrics = Metrics;
		if (MaximumJournalRecords == 0 || MaximumJournalRecords > MaximumWireJournalRecords)
			return {{}, "Replication journal batch limit is invalid"};
		auto Read = ChangeJournal::Get().Read(CandidatePeer.JournalCursor, MaximumJournalRecords);
		if (Read.Status == ChangeReadStatus::ResnapshotRequired)
			return {{}, "Authoritative journal cursor requires a new baseline"};
		ReplicationFrame Frame{
			ReplicationProtocolVersion,
			ReplicationMessageKind::Incremental,
			CandidatePeer.View.Epoch,
			CandidatePeer.NextSequence
		};
		if (Read.Records.empty()) return {{}, "No replication changes are available"};
		Snapshot Current;
		try {
			Current = CaptureSnapshot(SourceRoot);
		} catch (const std::exception &Error) {
			return {{}, Error.what()};
		}
		const auto Objects = IndexSnapshot(Current);
		std::set<ObjectId> PublishedThisFrame;
		std::set<ObjectId> BatchPublishObjects;
		for (const auto &Record : Read.Records)
			if (std::holds_alternative<ObjectCreatedChange>(Record.Payload) && Objects.contains(Record.Object))
				BatchPublishObjects.insert(Record.Object);
		for (const auto &Record : Read.Records) {
			const auto Known = CandidatePeer.View.Knows(Record.Object);
			const auto Relevant = CandidatePeer.View.RelevantObjects.contains(Record.Object);
			bool FailedReference = false;
			std::visit(
				[&](const auto &Change) {
					using Type = std::decay_t<decltype(Change)>;
					if constexpr (std::is_same_v<Type, ObjectCreatedChange>) {
						if (!Relevant) CandidatePeer.View.RelevantObjects.insert(Record.Object);
						if (Known) return;
						auto Found = Objects.find(Record.Object);
						if (Found == Objects.end()) return;
						const auto Publish = MakePublish(*Found->second);
						if (Publish.Parent && !CandidatePeer.View.Knows(*Publish.Parent)) {
							FailedReference = true;
							return;
						}
						if (!PublishReferencesKnown(CandidatePeer.View, Publish, BatchPublishObjects)) {
							FailedReference = true;
							return;
						}
						Frame.Operations.push_back({Frame.Epoch, Publish});
						CandidatePeer.View.KnownObjects.insert(Record.Object);
						PublishedThisFrame.insert(Record.Object);
						++CandidateMetrics.ObjectsPublished;
					} else if constexpr (std::is_same_v<Type, ObjectDestroyedChange>) {
						if (!Known) return;
						Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Record.Object}});
						CandidatePeer.View.ForgetReplica(Record.Object);
						CandidatePeer.View.RelevantObjects.erase(Record.Object);
						++CandidateMetrics.ObjectsDestroyed;
					} else {
						if (!Known || PublishedThisFrame.contains(Record.Object)) {
							if (PublishedThisFrame.contains(Record.Object)) ++CandidateMetrics.OperationsCoalesced;
							return;
						}
						if constexpr (std::is_same_v<Type, PropertyUpdatedChange>) {
							if (!Change.Replicated) return;
							// Destruction is a lifecycle operation, not an ordinary writable property.
							// Instance::Destroy journals both records so the later ObjectDestroyedChange
							// remains the single protocol representation.
							if (Change.PropertyName == "Destroyed") {
								++CandidateMetrics.OperationsCoalesced;
								return;
							}
							if (!ReferencesKnown(CandidatePeer.View, Change.Value)) {
								FailedReference = true;
								return;
							}
							PropertyReplicationUpdate Update{Record.Object, Change.PropertyName, Change.Value};
							if (Change.DeclaringClassSchemaId) {
								Update.DeclaringClassSchemaId = Change.DeclaringClassSchemaId;
								Update.DefinitionVersion = Change.DefinitionVersion;
							}
							Frame.Operations.push_back({Frame.Epoch, std::move(Update)});
						} else if constexpr (std::is_same_v<Type, AttributeUpdatedChange>) {
							if (Change.Value && !ReferencesKnown(CandidatePeer.View, *Change.Value)) {
								FailedReference = true;
								return;
							}
							Frame.Operations.push_back(
								{Frame.Epoch,
								 AttributeReplicationUpdate{Record.Object, Change.AttributeName, Change.Value}}
							);
						} else if constexpr (std::is_same_v<Type, ExtensionPropertyUpdatedChange>) {
							if (!ReferencesKnown(CandidatePeer.View, Change.Value)) {
								FailedReference = true;
								return;
							}
							Frame.Operations.push_back(
								{Frame.Epoch,
								 ExtensionPropertyReplicationUpdate{
									 Record.Object,
									 Change.ExtensionSchemaId,
									 Change.DefinitionVersion,
									 Change.PropertyName,
									 Change.Value
								 }}
							);
						} else if constexpr (std::is_same_v<Type, TagAddedChange>) {
							Frame.Operations.push_back(
								{Frame.Epoch, TagAddedReplication{Record.Object, Change.TagName}}
							);
						} else if constexpr (std::is_same_v<Type, TagRemovedChange>) {
							Frame.Operations.push_back(
								{Frame.Epoch, TagRemovedReplication{Record.Object, Change.TagName}}
							);
						} else if constexpr (std::is_same_v<Type, ObjectReparentedChange>) {
							if (Change.Parent && !CandidatePeer.View.Knows(*Change.Parent)) {
								FailedReference = true;
								return;
							}
							Frame.Operations.push_back(
								{Frame.Epoch, ReparentReplication{Record.Object, Change.Parent}}
							);
						}
					}
				},
				Record.Payload
			);
			if (FailedReference) {
				++CandidateMetrics.RejectedInvalidReferences;
				return {{}, "Replication operation references an object not materialized for this peer"};
			}
		}
		if (Frame.Operations.size() > MaximumReplicationOperationsPerFrame)
			return {{}, "Replication frame operation limit exceeded"};
		auto Encoded = EncodeReplicationFrame(Frame);
		if (!Encoded) return {{}, Encoded.error().Format()};
		auto Next = CandidatePeer.NextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		CandidatePeer.NextSequence = *Next;
		CandidatePeer.JournalCursor = Read.Cursor;
		Peer->second = std::move(CandidatePeer);
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		CandidateMetrics.IncrementalBytes += Encoded->size();
		CandidateMetrics.ReplicationBacklog = Read.Records.size() == MaximumJournalRecords ? 1 : 0;
		Metrics = CandidateMetrics;
		return {std::move(Frame), {}};
	}

	ReplicationProduceResult
	ReplicationCoordinator::SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end() || !Object.IsValid()) return {{}, "Replication peer or object is invalid"};
		auto CandidatePeer = Peer->second;
		auto CandidateMetrics = Metrics;
		Snapshot Current;
		try {
			Current = CaptureSnapshot(SourceRoot);
		} catch (const std::exception &Error) {
			return {{}, Error.what()};
		}
		const auto Objects = IndexSnapshot(Current);
		ReplicationFrame Frame{
			ReplicationProtocolVersion,
			ReplicationMessageKind::Incremental,
			CandidatePeer.View.Epoch,
			CandidatePeer.NextSequence
		};
		if (Relevant) {
			auto Found = Objects.find(Object);
			if (Found == Objects.end()) return {{}, "Cannot publish a stale authoritative object"};
			if (CandidatePeer.View.Knows(Object)) return {{}, "Object is already published"};
			auto Publish = MakePublish(*Found->second);
			if (Publish.Parent && !CandidatePeer.View.Knows(*Publish.Parent))
				return {{}, "Publish parent is not materialized for this peer"};
			if (!PublishReferencesKnown(CandidatePeer.View, Publish, {Object}))
				return {{}, "Publish references an object not materialized for this peer"};
			Frame.Operations.push_back({Frame.Epoch, std::move(Publish)});
			CandidatePeer.View.RelevantObjects.insert(Object);
			CandidatePeer.View.KnownObjects.insert(Object);
			++CandidateMetrics.ObjectsPublished;
		} else {
			if (!CandidatePeer.View.Knows(Object)) return {{}, "Object is already unpublished"};
			std::vector<ObjectId> Removed{Object};
			std::set<ObjectId> RemovedSet{Object};
			for (std::size_t Index = 0; Index < Removed.size(); ++Index) {
				for (const auto &[Candidate, SnapshotObjectValue] : Objects) {
					if (!CandidatePeer.View.Knows(Candidate) || RemovedSet.contains(Candidate)) continue;
					if ((SnapshotObjectValue->Parent &&
						 RemovedSet.contains(SnapshotObjectValue->Parent->ToObjectId())) ||
						ReferencesAny(*SnapshotObjectValue, RemovedSet)) {
						RemovedSet.insert(Candidate);
						Removed.push_back(Candidate);
					}
				}
			}
			for (auto Iterator = Removed.rbegin(); Iterator != Removed.rend(); ++Iterator) {
				Frame.Operations.push_back({Frame.Epoch, UnpublishReplication{*Iterator}});
				CandidatePeer.View.ForgetReplica(*Iterator);
				CandidatePeer.View.RelevantObjects.erase(*Iterator);
			}
			CandidateMetrics.ObjectsUnpublished += Removed.size();
		}
		auto Encoded = EncodeReplicationFrame(Frame);
		if (!Encoded) return {{}, Encoded.error().Format()};
		auto Next = CandidatePeer.NextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		CandidatePeer.NextSequence = *Next;
		Peer->second = std::move(CandidatePeer);
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		CandidateMetrics.IncrementalBytes += Encoded->size();
		Metrics = CandidateMetrics;
		return {std::move(Frame), {}};
	}

	bool ReplicationCoordinator::RemovePeer(ConnectionId Connection) {
		return Peers.erase(Connection) != 0;
	}

	const ReplicationView *ReplicationCoordinator::GetView(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer == Peers.end() ? nullptr : &Peer->second.View;
	}
}

#include "gargantuan/network/ReplicationCoordinator.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <type_traits>
#include <vector>

namespace gargantuan::network {
	namespace {
		constexpr std::size_t MaximumDependencyClosureDepth = 64;
		constexpr std::size_t MaximumDependencyClosureObjects = MaximumPeerDesiredObjects;
		constexpr std::size_t MaximumCatalogRefreshBatches = 16;

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

		const InstanceProperty *FindNativeProperty(const SnapshotObject &Object, std::string_view Name) {
			const auto *Definition = GetActiveRuntimeSchemaRegistry().FindClassById(Object.ClassSchemaId);
			if (!Definition) return nullptr;
			const auto Found = Definition->AllProperties.find(std::string(Name));
			return Found == Definition->AllProperties.end() ? nullptr : Found->second;
		}

		bool IsHardReference(const SnapshotObject &Object, std::string_view Name) {
			const auto *Property = FindNativeProperty(Object, Name);
			return Property && Property->SemanticType == InstanceProperty::DataType::ObjectReference &&
				   (Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard ||
					!Property->Nullable);
		}

		std::size_t AncestryDepth(ObjectId Object, const std::map<ObjectId, SnapshotObject> &Catalog) {
			std::size_t Depth = 0;
			std::set<ObjectId> Visited;
			while (Depth <= MaximumDependencyClosureDepth && Visited.insert(Object).second) {
				auto Found = Catalog.find(Object);
				if (Found == Catalog.end() || !Found->second.Parent) break;
				Object = Found->second.Parent->ToObjectId();
				++Depth;
			}
			return Depth;
		}
	}

	ReplicationCoordinator::ReplicationCoordinator(
		std::shared_ptr<Instance> SourceRoot, InitialRelevancePolicy IsInitiallyRelevantValue
	)
		: SourceRoot(std::move(SourceRoot)), IsInitiallyRelevant(std::move(IsInitiallyRelevantValue)) {
		if (!this->SourceRoot) return;
		auto SnapshotValue = CaptureSnapshot(this->SourceRoot);
		CatalogCursor = SnapshotValue.Cursor;
		for (auto &Object : SnapshotValue.Objects)
			Catalog.emplace(Object.Id.ToObjectId(), std::move(Object));
		Metrics.CatalogObjects = Catalog.size();
	}

	bool ReplicationCoordinator::RefreshCatalog(std::string &Error) {
		if (!SourceRoot || !CatalogCursor.Scope.IsValid()) {
			Error = "Replication catalog source is invalid";
			return false;
		}
		for (std::size_t Batch = 0; Batch < MaximumCatalogRefreshBatches; ++Batch) {
			for (auto Iterator = RetiredObjects.begin(); Iterator != RetiredObjects.end();) {
				const bool Known = std::ranges::any_of(Peers, [&](const auto &Entry) {
					return Entry.second.View.Knows(*Iterator);
				});
				if (Known) {
					++Iterator;
					continue;
				}
				Catalog.erase(*Iterator);
				Iterator = RetiredObjects.erase(Iterator);
			}
			auto Read = ChangeJournal::Get().Read(CatalogCursor, MaximumWireJournalRecords);
			if (Read.Status == ChangeReadStatus::ResnapshotRequired) {
				try {
					auto SnapshotValue = CaptureSnapshot(SourceRoot);
					Catalog.clear();
					RetiredObjects.clear();
					for (auto &Object : SnapshotValue.Objects)
						Catalog.emplace(Object.Id.ToObjectId(), std::move(Object));
					CatalogCursor = SnapshotValue.Cursor;
					Metrics.CatalogObjects = Catalog.size();
					++Metrics.CatalogRefreshes;
					return true;
				} catch (const std::exception &Failure) {
					Error = Failure.what();
					return false;
				}
			}
			std::set<ObjectId> Touched;
			for (const auto &Record : Read.Records)
				Touched.insert(Record.Object);
			try {
				for (const auto Object : Touched) {
					auto Live = ObjectRegistry::Get().Lookup(Object);
					if (!Live || Live->GetDestroyed() || Live->IsDestroying()) {
						RetiredObjects.insert(Object);
						continue;
					}
					RetiredObjects.erase(Object);
					Catalog[Object] = CaptureSnapshotObject(Live);
				}
			} catch (const std::exception &Failure) {
				Error = Failure.what();
				return false;
			}
			CatalogCursor = Read.Cursor;
			Metrics.CatalogObjects = Catalog.size();
			if (!Read.Records.empty()) ++Metrics.CatalogRefreshes;
			if (Read.Records.size() < MaximumWireJournalRecords) return true;
		}
		Error = "Replication catalog refresh work limit exceeded";
		return false;
	}

	bool ReplicationCoordinator::BuildDependencyClosure(
		const PeerRelevanceSelection &Selection, std::set<ObjectId> &Closure, std::string &Error
	) {
		if (Selection.DesiredObjects.size() > MaximumPeerDesiredObjects ||
			Selection.RequiredObjects.size() > MaximumPeerDesiredObjects) {
			Error = "Replication relevance selection exceeds its object limit";
			++Metrics.DependencyLimitFailures;
			return false;
		}
		struct PendingObject {
			ObjectId Object;
			std::size_t Depth = 0;
		};
		std::vector<PendingObject> Pending;
		Pending.reserve(Selection.DesiredObjects.size());
		std::set<ObjectId> Required(Selection.RequiredObjects.begin(), Selection.RequiredObjects.end());
		for (const auto Object : Selection.DesiredObjects)
			Pending.push_back({Object, 0});
		for (const auto Object : Selection.RequiredObjects)
			Pending.push_back({Object, 0});
		for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
			const auto [Object, Depth] = Pending[Index];
			if (!Object.IsValid() || Closure.contains(Object)) continue;
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end() || RetiredObjects.contains(Object)) {
				if (Required.contains(Object)) {
					Error = "Required relevance object is stale";
					++Metrics.DependencyLimitFailures;
					return false;
				}
				continue;
			}
			if (Depth > MaximumDependencyClosureDepth) {
				Error = "Replication dependency closure depth limit exceeded";
				++Metrics.DependencyLimitFailures;
				return false;
			}
			Closure.insert(Object);
			if (Closure.size() > MaximumDependencyClosureObjects) {
				Error = "Replication dependency closure object limit exceeded";
				++Metrics.DependencyLimitFailures;
				return false;
			}
			if (Found->second.Parent) Pending.push_back({Found->second.Parent->ToObjectId(), Depth + 1});
			for (const auto &[Name, Value] : Found->second.Properties) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				if (Reference && IsHardReference(Found->second, Name))
					Pending.push_back({Reference->Object.ToObjectId(), Depth + 1});
			}
		}
		Metrics.DependencyObjects += Closure.size();
		return true;
	}

	PublishReplication
	ReplicationCoordinator::MakePeerPublish(const SnapshotObject &Object, const std::set<ObjectId> &Known) const {
		auto Publish = MakePublish(Object);
		for (auto &[Name, Value] : Publish.Properties) {
			const auto *Reference = std::get_if<WireObjectReference>(&Value);
			if (Reference && !Known.contains(Reference->Object.ToObjectId()) && !IsHardReference(Object, Name))
				Value = std::monostate{};
		}
		return Publish;
	}

	ReplicationProduceResult ReplicationCoordinator::ProduceRelevanceFrame(
		ConnectionId Connection, const PeerRelevanceSelection &Selection, ReplicationMessageKind Kind
	) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		const auto Started = std::chrono::steady_clock::now();
		const auto RefreshStarted = std::chrono::steady_clock::now();
		std::string Error;
		if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
		Metrics.SnapshotCaptureCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - RefreshStarted)
				.count()
		);
		const auto DiscoveryStarted = std::chrono::steady_clock::now();
		std::set<ObjectId> Closure;
		if (!BuildDependencyClosure(Selection, Closure, Error)) return {{}, std::move(Error)};
		auto CandidatePeer = Peer->second;
		auto CandidateMetrics = Metrics;
		ReplicationFrame Frame{ReplicationProtocolVersion, Kind, CandidatePeer.View.Epoch, CandidatePeer.NextSequence};
		if (Kind == ReplicationMessageKind::Baseline) Frame.Schema = CaptureReplicationSchemaCompatibility();

		std::set<ObjectId> Existing(CandidatePeer.View.KnownObjects.begin(), CandidatePeer.View.KnownObjects.end());
		const std::set<ObjectId> RequiredObjects(
			Selection.RequiredObjects.begin(), Selection.RequiredObjects.end()
		);
		std::set<ObjectId> Leaving;
		std::set_difference(
			Existing.begin(), Existing.end(), Closure.begin(), Closure.end(), std::inserter(Leaving, Leaving.end())
		);
		std::set<ObjectId> Entering;
		std::set_difference(
			Closure.begin(), Closure.end(), Existing.begin(), Existing.end(), std::inserter(Entering, Entering.end())
		);
		std::set<ObjectId> DesiredEntering;
		std::set<ObjectId> DesiredLeaving;
		if (Kind == ReplicationMessageKind::Incremental) {
			DesiredEntering.swap(Entering);
			DesiredLeaving.swap(Leaving);
			std::size_t Remaining = MaximumRelevanceTransitionsPerFrame;
			std::set<ObjectId> PriorityEntering;
			for (const auto Object : DesiredEntering)
				if (RequiredObjects.contains(Object)) PriorityEntering.insert(Object);
			for (const auto Referrer : Closure) {
				if (!Existing.contains(Referrer)) continue;
				auto Found = Catalog.find(Referrer);
				if (Found == Catalog.end()) continue;
				for (const auto &[Name, Value] : Found->second.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (Reference && IsHardReference(Found->second, Name) &&
						DesiredEntering.contains(Reference->Object.ToObjectId()))
						PriorityEntering.insert(Reference->Object.ToObjectId());
				}
			}
			std::vector<ObjectId> EnterCandidates(DesiredEntering.begin(), DesiredEntering.end());
			std::ranges::sort(EnterCandidates, [&](ObjectId Left, ObjectId Right) {
				const bool LeftPriority = PriorityEntering.contains(Left);
				const bool RightPriority = PriorityEntering.contains(Right);
				if (LeftPriority != RightPriority) return LeftPriority;
				const auto LeftDepth = AncestryDepth(Left, Catalog);
				const auto RightDepth = AncestryDepth(Right, Catalog);
				return LeftDepth != RightDepth ? LeftDepth < RightDepth : Left < Right;
			});
			auto AddEnterGroup = [&](ObjectId Candidate) -> std::optional<bool> {
				if (Entering.contains(Candidate)) return true;
				std::set<ObjectId> DependencyGroup;
				std::vector<ObjectId> Pending{Candidate};
				for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
					const auto Object = Pending[Index];
					if (!DesiredEntering.contains(Object) || !DependencyGroup.insert(Object).second) continue;
					auto Found = Catalog.find(Object);
					if (Found == Catalog.end()) continue;
					if (Found->second.Parent) Pending.push_back(Found->second.Parent->ToObjectId());
					for (const auto &[Name, Value] : Found->second.Properties) {
						const auto *Reference = std::get_if<WireObjectReference>(&Value);
						if (Reference && IsHardReference(Found->second, Name))
							Pending.push_back(Reference->Object.ToObjectId());
					}
				}
				std::size_t NewDependencies = 0;
				for (const auto Object : DependencyGroup)
					if (!Entering.contains(Object)) ++NewDependencies;
				if (NewDependencies > MaximumRelevanceTransitionsPerFrame) {
					++Metrics.DependencyLimitFailures;
					return std::nullopt;
				}
				if (NewDependencies > Remaining) return false;
				Entering.insert(DependencyGroup.begin(), DependencyGroup.end());
				Remaining -= NewDependencies;
				return true;
			};
			for (const auto Candidate : EnterCandidates) {
				if (!PriorityEntering.contains(Candidate)) continue;
				auto Added = AddEnterGroup(Candidate);
				if (!Added)
					return {{}, "Atomic replication dependency group exceeds the transition work limit"};
			}
			const bool PriorityBacklogged = std::ranges::any_of(
				PriorityEntering, [&](ObjectId Object) { return !Entering.contains(Object); }
			);
			if (!PriorityBacklogged) {
				std::map<ObjectId, std::set<ObjectId>> LeavingDependents;
				for (const auto Object : DesiredLeaving) {
					auto Found = Catalog.find(Object);
					if (Found == Catalog.end()) continue;
					if (Found->second.Parent) {
						const auto Parent = Found->second.Parent->ToObjectId();
						if (DesiredLeaving.contains(Parent)) LeavingDependents[Parent].insert(Object);
					}
					for (const auto &[Name, Value] : Found->second.Properties) {
						const auto *Reference = std::get_if<WireObjectReference>(&Value);
						if (!Reference || !IsHardReference(Found->second, Name)) continue;
						const auto Target = Reference->Object.ToObjectId();
						if (DesiredLeaving.contains(Target)) LeavingDependents[Target].insert(Object);
					}
				}
				std::vector<ObjectId> LeaveOrder(DesiredLeaving.begin(), DesiredLeaving.end());
				std::ranges::sort(LeaveOrder, [&](ObjectId Left, ObjectId Right) {
					const auto LeftDepth = AncestryDepth(Left, Catalog);
					const auto RightDepth = AncestryDepth(Right, Catalog);
					return LeftDepth != RightDepth ? LeftDepth > RightDepth : Left < Right;
				});
				for (const auto Candidate : LeaveOrder) {
					if (Remaining == 0 || Leaving.contains(Candidate)) continue;
					std::set<ObjectId> DependencyGroup;
					std::vector<ObjectId> Pending{Candidate};
					for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
						const auto Object = Pending[Index];
						if (Leaving.contains(Object) || !DesiredLeaving.contains(Object) ||
							!DependencyGroup.insert(Object).second)
							continue;
						auto Dependents = LeavingDependents.find(Object);
						if (Dependents != LeavingDependents.end())
							Pending.insert(Pending.end(), Dependents->second.begin(), Dependents->second.end());
					}
					if (DependencyGroup.size() > MaximumRelevanceTransitionsPerFrame) {
						++Metrics.DependencyLimitFailures;
						return {{}, "Atomic replication dependency group exceeds the transition work limit"};
					}
					if (DependencyGroup.size() > Remaining) continue;
					Leaving.insert(DependencyGroup.begin(), DependencyGroup.end());
					Remaining -= DependencyGroup.size();
				}
			}
			for (const auto Candidate : EnterCandidates) {
				if (PriorityEntering.contains(Candidate)) continue;
				auto Added = AddEnterGroup(Candidate);
				if (!Added)
					return {{}, "Atomic replication dependency group exceeds the transition work limit"};
			}
		}
		CandidatePeer.PendingRelevanceTransitions = Kind == ReplicationMessageKind::Incremental
														? DesiredEntering.size() + DesiredLeaving.size() -
															  Entering.size() - Leaving.size()
														: 0;
		CandidateMetrics.MaterializationBacklog = CandidatePeer.PendingRelevanceTransitions;
		for (const auto &[OtherConnection, OtherPeer] : Peers)
			if (OtherConnection != Connection)
				CandidateMetrics.MaterializationBacklog += OtherPeer.PendingRelevanceTransitions;
		std::set<ObjectId> IncrementalMaterializedObjects;
		const std::set<ObjectId> *MaterializedAfterFrame = &Closure;
		if (Kind == ReplicationMessageKind::Incremental) {
			IncrementalMaterializedObjects = Existing;
			for (const auto Object : Leaving)
				IncrementalMaterializedObjects.erase(Object);
			IncrementalMaterializedObjects.insert(Entering.begin(), Entering.end());
			MaterializedAfterFrame = &IncrementalMaterializedObjects;
		}

		if (Kind == ReplicationMessageKind::Incremental && !Leaving.empty()) {
			for (const auto Referrer : Closure) {
				if (!Existing.contains(Referrer) || Entering.contains(Referrer)) continue;
				auto Object = Catalog.find(Referrer);
				if (Object == Catalog.end()) continue;
				for (const auto &[Name, Value] : Object->second.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					const auto *Property = FindNativeProperty(Object->second, Name);
					const bool CurrentHardReferenceIsNil =
						Property && Property->SemanticType == InstanceProperty::DataType::ObjectReference &&
						IsHardReference(Object->second, Name) && std::holds_alternative<std::monostate>(Value);
					const bool SoftTargetLeaves =
						Reference && !IsHardReference(Object->second, Name) &&
						Leaving.contains(Reference->Object.ToObjectId());
					if (CurrentHardReferenceIsNil || SoftTargetLeaves) {
						Frame.Operations.push_back(
							{Frame.Epoch, PropertyReplicationUpdate{Referrer, Name, std::monostate{}}}
						);
						++CandidateMetrics.SoftReferenceFixups;
					}
				}
			}
		}

		std::vector<ObjectId> EnterOrder(Entering.begin(), Entering.end());
		std::ranges::sort(EnterOrder, [&](ObjectId Left, ObjectId Right) {
			const auto LeftDepth = AncestryDepth(Left, Catalog);
			const auto RightDepth = AncestryDepth(Right, Catalog);
			return LeftDepth != RightDepth ? LeftDepth < RightDepth : Left < Right;
		});
		for (const auto Object : EnterOrder) {
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end()) return {{}, "Cannot publish a stale authoritative object"};
			auto Publish = MakePeerPublish(Found->second, *MaterializedAfterFrame);
			if (!PublishReferencesKnown(CandidatePeer.View, Publish, Entering))
				return {{}, "Hard materialization dependency is not available"};
			Frame.Operations.push_back({Frame.Epoch, std::move(Publish)});
			CandidatePeer.View.KnownObjects.insert(Object);
			++CandidateMetrics.ObjectsPublished;
		}

		if (Kind == ReplicationMessageKind::Incremental) {
			std::vector<ObjectId> LeaveOrder(Leaving.begin(), Leaving.end());
			std::ranges::sort(LeaveOrder, [&](ObjectId Left, ObjectId Right) {
				const auto LeftDepth = AncestryDepth(Left, Catalog);
				const auto RightDepth = AncestryDepth(Right, Catalog);
				return LeftDepth != RightDepth ? LeftDepth > RightDepth : Left < Right;
			});
			for (const auto Object : LeaveOrder) {
				if (RetiredObjects.contains(Object)) {
					Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Object}});
					++CandidateMetrics.ObjectsDestroyed;
				} else {
					Frame.Operations.push_back({Frame.Epoch, UnpublishReplication{Object}});
					++CandidateMetrics.ObjectsUnpublished;
				}
				CandidatePeer.View.ForgetReplica(Object);
			}
			for (const auto Referrer : Closure) {
				if (Entering.contains(Referrer) || !Existing.contains(Referrer)) continue;
				auto Object = Catalog.find(Referrer);
				if (Object == Catalog.end()) continue;
				for (const auto &[Name, Value] : Object->second.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (!Reference) continue;
					if (Entering.contains(Reference->Object.ToObjectId())) {
						Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Referrer, Name, Value}});
						++CandidateMetrics.SoftReferenceFixups;
					}
				}
			}
		}

		if (Frame.Operations.empty()) {
			CandidatePeer.DesiredObjects = std::set<ObjectId>(
				Selection.DesiredObjects.begin(), Selection.DesiredObjects.end()
			);
			CandidatePeer.View.RelevantObjects.clear();
			std::set_intersection(
				Closure.begin(),
				Closure.end(),
				CandidatePeer.View.KnownObjects.begin(),
				CandidatePeer.View.KnownObjects.end(),
				std::inserter(CandidatePeer.View.RelevantObjects, CandidatePeer.View.RelevantObjects.end())
			);
			Peer->second = std::move(CandidatePeer);
			return {{}, "No replication relevance changes are available"};
		}
		if (Frame.Operations.size() > MaximumReplicationOperationsPerFrame)
			return {{}, "Replication relevance frame operation limit exceeded"};
		if (Kind == ReplicationMessageKind::Baseline)
			CandidateMetrics.BaselineDiscoveryCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - DiscoveryStarted
				)
					.count()
			);
		const auto EncodeStarted = std::chrono::steady_clock::now();
		auto Encoded = EncodeReplicationFrame(Frame);
		if (Kind == ReplicationMessageKind::Baseline)
			CandidateMetrics.BaselineEncodeCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - EncodeStarted)
					.count()
			);
		if (!Encoded) return {{}, Encoded.error().Format()};
		auto Next = CandidatePeer.NextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		CandidatePeer.NextSequence = *Next;
		CandidatePeer.DesiredObjects = std::set<ObjectId>(
			Selection.DesiredObjects.begin(), Selection.DesiredObjects.end()
		);
		CandidatePeer.View.RelevantObjects.clear();
		std::set_intersection(
			Closure.begin(),
			Closure.end(),
			CandidatePeer.View.KnownObjects.begin(),
			CandidatePeer.View.KnownObjects.end(),
			std::inserter(CandidatePeer.View.RelevantObjects, CandidatePeer.View.RelevantObjects.end())
		);
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		CandidateMetrics.RelevanceTransitions += Entering.size() + Leaving.size();
		CandidateMetrics.RelevanceTransitionCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
		);
		if (Kind == ReplicationMessageKind::Baseline) {
			CandidatePeer.JournalCursor = CatalogCursor;
			CandidateMetrics.BaselineObjects += Entering.size();
			CandidateMetrics.BaselineBytes += Encoded->size();
		} else
			CandidateMetrics.IncrementalBytes += Encoded->size();
		Peer->second = std::move(CandidatePeer);
		Metrics = CandidateMetrics;
		return {std::move(Frame), {}};
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(ConnectionId Connection, ReplicationEpoch Epoch) {
		std::string Error;
		if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
		PeerRelevanceSelection Selection;
		for (const auto &[Object, State] : Catalog) {
			if (!State.Parent || !IsInitiallyRelevant || IsInitiallyRelevant(Object))
				Selection.DesiredObjects.push_back(Object);
		}
		auto Result = AddPeer(Connection, Epoch, Selection);
		if (Result.Succeeded()) Peers.at(Connection).PolicyManaged = false;
		return Result;
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(
		ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection
	) {
		if (!SourceRoot || !Connection.IsValid() || !Epoch.IsValid()) return {{}, "Invalid replication peer or source"};
		if (Peers.contains(Connection)) return {{}, "Replication peer is already registered"};
		for (const auto &[Existing, State] : Peers)
			if (Existing.Slot == Connection.Slot && State.View.Connection.IsValid())
				return {{}, "A live replication peer already owns this connection slot"};
		PeerState State{{Connection, Epoch}, CatalogCursor, ReliableReplicationSequence(1)};
		State.PolicyManaged = true;
		Peers.emplace(Connection, std::move(State));
		auto Result = ProduceRelevanceFrame(Connection, Selection, ReplicationMessageKind::Baseline);
		if (!Result.Succeeded()) Peers.erase(Connection);
		return Result;
	}

	ReplicationProduceResult
	ReplicationCoordinator::UpdateRelevance(ConnectionId Connection, const PeerRelevanceSelection &Selection) {
		return ProduceRelevanceFrame(Connection, Selection, ReplicationMessageKind::Incremental);
	}

	ReplicationProduceResult
	ReplicationCoordinator::ProduceIncremental(ConnectionId Connection, std::size_t MaximumJournalRecords) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		std::string CatalogError;
		if (!RefreshCatalog(CatalogError)) return {{}, std::move(CatalogError)};
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
		std::set<ObjectId> PublishedThisFrame;
		std::set<ObjectId> BatchPublishObjects;
		for (const auto &Record : Read.Records)
			if (std::holds_alternative<ObjectCreatedChange>(Record.Payload) && Catalog.contains(Record.Object) &&
				(!CandidatePeer.PolicyManaged || CandidatePeer.View.RelevantObjects.contains(Record.Object)))
				BatchPublishObjects.insert(Record.Object);
		for (const auto &Record : Read.Records) {
			const auto Known = CandidatePeer.View.Knows(Record.Object);
			const auto Relevant = CandidatePeer.View.RelevantObjects.contains(Record.Object);
			if (CandidatePeer.PolicyManaged && !Relevant) continue;
			if (!Known && !CandidatePeer.PolicyManaged && IsInitiallyRelevant && !IsInitiallyRelevant(Record.Object))
				continue;
			bool FailedReference = false;
			std::visit(
				[&](const auto &Change) {
					using Type = std::decay_t<decltype(Change)>;
					if constexpr (std::is_same_v<Type, ObjectCreatedChange>) {
						if (!Relevant) CandidatePeer.View.RelevantObjects.insert(Record.Object);
						if (Known) return;
						auto Found = Catalog.find(Record.Object);
						if (Found == Catalog.end()) return;
						std::set<ObjectId> Available(
							CandidatePeer.View.KnownObjects.begin(), CandidatePeer.View.KnownObjects.end()
						);
						Available.insert(BatchPublishObjects.begin(), BatchPublishObjects.end());
						const auto Publish = CandidatePeer.PolicyManaged ? MakePeerPublish(Found->second, Available)
																		 : MakePublish(Found->second);
						if (Publish.Parent && !CandidatePeer.View.Knows(*Publish.Parent)) {
							CandidatePeer.View.RelevantObjects.erase(Record.Object);
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
							auto Value = Change.Value;
							auto CurrentObject = Catalog.find(Record.Object);
							if (!Change.DeclaringClassSchemaId && CurrentObject != Catalog.end()) {
								auto CurrentValue = CurrentObject->second.Properties.find(Change.PropertyName);
								if (CurrentValue != CurrentObject->second.Properties.end())
									Value = CurrentValue->second;
							}
							if (!ReferencesKnown(CandidatePeer.View, Value)) {
								if (CandidatePeer.PolicyManaged && CurrentObject != Catalog.end()) {
									if (!IsHardReference(CurrentObject->second, Change.PropertyName)) {
										Value = std::monostate{};
										++CandidateMetrics.SoftReferenceFixups;
									} else {
										// The desired-set pass has already discovered this hard target. If its
										// atomic enter is still behind the transition budget, that enter emits
										// the current referrer fixup after the target becomes known.
										++CandidateMetrics.OperationsCoalesced;
										return;
									}
								} else {
									FailedReference = true;
									return;
								}
							}
							PropertyReplicationUpdate Update{Record.Object, Change.PropertyName, std::move(Value)};
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
		if (Frame.Operations.empty()) {
			CandidatePeer.JournalCursor = Read.Cursor;
			CandidateMetrics.ReplicationBacklog = Read.Records.size() == MaximumJournalRecords ? 1 : 0;
			Peer->second = std::move(CandidatePeer);
			Metrics = CandidateMetrics;
			return {{}, "No relevant replication changes are available"};
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

	bool ReplicationCoordinator::HasPendingRelevance(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer != Peers.end() && Peer->second.PendingRelevanceTransitions != 0;
	}
}

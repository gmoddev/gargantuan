#include "gargantuan/network/ReplicationCoordinator.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace gargantuan::network {
	namespace {
		constexpr std::size_t MaximumDependencyClosureDepth = 64;
		constexpr std::size_t MaximumDependencyClosureObjects = MaximumPeerDesiredObjects;
		constexpr std::size_t MaximumCatalogRefreshBatches = 16;

		PublishReplication MakePublish(SnapshotObject Object) {
			return {
				Object.Id.ToObjectId(),
				Object.ClassSchemaId,
				Object.ClassDefinitionVersion,
				Object.Parent ? std::optional(Object.Parent->ToObjectId()) : std::nullopt,
				std::move(Object.ClassName),
				std::move(Object.Name),
				std::move(Object.Properties),
				std::move(Object.Attributes),
				std::move(Object.Extensions),
				std::move(Object.CustomProperties),
				std::move(Object.Tags),
			};
		}

		std::size_t DynamicWireValueBytes(const WireValue &Value) {
			return std::visit(
				[](const auto &Item) -> std::size_t {
					using Type = std::decay_t<decltype(Item)>;
					if constexpr (std::is_same_v<Type, std::string>)
						return Item.size();
					else if constexpr (std::is_same_v<Type, WireEnumItem>)
						return Item.EnumType.size() + Item.Item.size();
					else
						return 0;
				},
				Value
			);
		}

		std::size_t ValueMapBytes(const std::map<std::string, WireValue> &Values) {
			std::size_t Bytes = Values.size() * sizeof(std::pair<const std::string, WireValue>);
			for (const auto &[Name, Value] : Values)
				Bytes += Name.size() + DynamicWireValueBytes(Value);
			return Bytes;
		}

		std::size_t RetainedTemplateBytes(const PublishReplication &Publication) {
			std::size_t Bytes = sizeof(StructuralMaterializationTemplate) + Publication.ClassName.size() +
								Publication.Name.size() + ValueMapBytes(Publication.Properties) +
								ValueMapBytes(Publication.Attributes) + Publication.Tags.size() * sizeof(std::string);
			for (const auto &State : Publication.Extensions)
				Bytes += sizeof(State) + ValueMapBytes(State.Properties);
			for (const auto &State : Publication.CustomProperties)
				Bytes += sizeof(State) + ValueMapBytes(State.Properties);
			for (const auto &Tag : Publication.Tags)
				Bytes += Tag.size();
			return Bytes;
		}

		std::shared_ptr<const StructuralMaterializationTemplate>
		MakeTemplate(ObjectId World, std::uint64_t StructuralRevision, SnapshotObject Object) {
			auto Publication = MakePublish(std::move(Object));
			if (!IsValidPublishReplication(Publication))
				throw std::invalid_argument("Authoritative structural template is invalid");
			const auto Key = StructuralMaterializationTemplateKey{World, Publication.Object, StructuralRevision};
			const auto Bytes = RetainedTemplateBytes(Publication);
			return std::make_shared<const StructuralMaterializationTemplate>(
				StructuralMaterializationTemplate{Key, std::move(Publication), Bytes}
			);
		}

		void SaturatingAdd(std::uint64_t &Value, std::uint64_t Added) {
			Value = Added > std::numeric_limits<std::uint64_t>::max() - Value
						? std::numeric_limits<std::uint64_t>::max()
						: Value + Added;
		}

		bool ReferencesKnown(const ReplicationView &View, const WireValue &Value) {
			if (const auto *Reference = std::get_if<WireObjectReference>(&Value))
				return View.Knows(Reference->Object.ToObjectId());
			return true;
		}

		bool PublishReferencesKnown(
			const ReplicationView &View,
			const PreparedPublishReplication &Publish,
			const std::set<ObjectId> &AdditionalKnown = {}
		) {
			auto Known = [&](const WireValue &Value) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				return !Reference || View.Knows(Reference->Object.ToObjectId()) ||
					   AdditionalKnown.contains(Reference->Object.ToObjectId());
			};
			const auto &Publication = Publish.Template->Publication;
			for (const auto &[Name, Value] : Publication.Properties) {
				if (Publish.NilProperties.Contains(Name)) continue;
				if (!Known(Value)) return false;
			}
			for (const auto &[Name, Value] : Publication.Attributes) {
				(void)Name;
				if (!Known(Value)) return false;
			}
			for (const auto &State : Publication.Extensions)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (!Known(Value)) return false;
				}
			for (const auto &State : Publication.CustomProperties)
				for (const auto &[Name, Value] : State.Properties) {
					(void)Name;
					if (!Known(Value)) return false;
				}
			return true;
		}

		bool ReferencesAny(const PublishReplication &Object, const std::set<ObjectId> &Targets) {
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

		const InstanceProperty *FindNativeProperty(const PublishReplication &Object, std::string_view Name) {
			const auto *Definition = GetActiveRuntimeSchemaRegistry().FindClassById(Object.ClassSchemaId);
			if (!Definition) return nullptr;
			const auto Found = std::ranges::find_if(Definition->AllProperties, [&](const auto &Entry) {
				return Entry.first == Name;
			});
			return Found == Definition->AllProperties.end() ? nullptr : Found->second;
		}

		bool IsHardReference(const PublishReplication &Object, std::string_view Name) {
			const auto *Property = FindNativeProperty(Object, Name);
			return Property && Property->SemanticType == InstanceProperty::DataType::ObjectReference &&
				   (Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard ||
					!Property->Nullable);
		}

		std::size_t AncestryDepth(
			ObjectId Object, const std::map<ObjectId, std::shared_ptr<const StructuralMaterializationTemplate>> &Catalog
		) {
			std::size_t Depth = 0;
			std::set<ObjectId> Visited;
			while (Depth <= MaximumDependencyClosureDepth && Visited.insert(Object).second) {
				auto Found = Catalog.find(Object);
				if (Found == Catalog.end() || !Found->second->Publication.Parent) break;
				Object = *Found->second->Publication.Parent;
				++Depth;
			}
			return Depth;
		}

		void RefreshRelevantObjects(ReplicationView &View, const std::set<ObjectId> &Closure) {
			View.RelevantObjects.clear();
			for (const auto Object : Closure)
				if (View.Knows(Object)) View.RelevantObjects.insert(Object);
		}
	}

	bool StructuralReplicationConfiguration::IsValid() const {
		return MaximumTransitionsPerPeerTick != 0 &&
			   MaximumTransitionsPerPeerTick <= MaximumRelevanceTransitionsPerFrame && MaximumTransitionsPerTick != 0 &&
			   MaximumTransitionsPerTick <= MaximumStructuralTransitionsPerTick &&
			   MaximumTransitionsPerPeerTick <= MaximumTransitionsPerTick && PeerQuantum != 0 &&
			   PeerQuantum <= MaximumTransitionsPerPeerTick && MaximumPendingTransitionsPerPeer != 0 &&
			   MaximumPendingTransitionsPerPeer <= MaximumPeerDesiredObjects && MaximumPendingTransitions != 0 &&
			   MaximumPendingTransitionsPerPeer <= MaximumPendingTransitions &&
			   MaximumPendingTransitions <= MaximumStructuralPendingTransitions && TransitionDeadlineTicks != 0;
	}

	ReplicationCoordinator::ReplicationCoordinator(
		std::shared_ptr<Instance> SourceRoot,
		InitialRelevancePolicy IsInitiallyRelevantValue,
		bool StructuralTemplateReuseEnabledValue,
		StructuralReplicationConfiguration ConfigurationValue
	)
		: SourceRoot(std::move(SourceRoot)), IsInitiallyRelevant(std::move(IsInitiallyRelevantValue)),
		  StructuralTemplateReuseEnabled(StructuralTemplateReuseEnabledValue), Configuration(ConfigurationValue) {
		if (!Configuration.IsValid()) throw std::invalid_argument("Structural replication configuration is invalid");
		if (!this->SourceRoot) return;
		auto SnapshotValue = CaptureSnapshot(this->SourceRoot);
		CatalogCursor = SnapshotValue.Cursor;
		WorldGeneration = SnapshotValue.Cursor.Scope;
		const auto StructuralRevision = SnapshotValue.Cursor.NextSequence - 1;
		for (auto &Object : SnapshotValue.Objects) {
			const auto Id = Object.Id.ToObjectId();
			auto Template = MakeTemplate(WorldGeneration, StructuralRevision, std::move(Object));
			SaturatingAdd(Metrics.StructuralTemplateBytes, Template->RetainedBytes);
			Catalog.emplace(Id, std::move(Template));
			SaturatingAdd(Metrics.StructuralTemplateBuilds, 1);
		}
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
				auto CatalogObject = Catalog.find(*Iterator);
				if (CatalogObject != Catalog.end()) {
					Metrics.StructuralTemplateBytes -= std::min<std::uint64_t>(
						Metrics.StructuralTemplateBytes, CatalogObject->second->RetainedBytes
					);
					Catalog.erase(CatalogObject);
				}
				RequestedTemplates.erase(*Iterator);
				Iterator = RetiredObjects.erase(Iterator);
			}
			auto Read = ChangeJournal::Get().Read(CatalogCursor, MaximumWireJournalRecords);
			if (Read.Status == ChangeReadStatus::ResnapshotRequired) {
				try {
					auto SnapshotValue = CaptureSnapshot(SourceRoot);
					std::map<ObjectId, std::shared_ptr<const StructuralMaterializationTemplate>> Replacement;
					std::uint64_t ReplacementBytes = 0;
					const auto StructuralRevision = SnapshotValue.Cursor.NextSequence - 1;
					for (auto &Object : SnapshotValue.Objects) {
						const auto Id = Object.Id.ToObjectId();
						auto Template = MakeTemplate(SnapshotValue.Cursor.Scope, StructuralRevision, std::move(Object));
						SaturatingAdd(ReplacementBytes, Template->RetainedBytes);
						Replacement.emplace(Id, std::move(Template));
					}
					SaturatingAdd(Metrics.StructuralTemplateInvalidations, Catalog.size());
					SaturatingAdd(Metrics.StructuralTemplateBuilds, Replacement.size());
					Catalog.swap(Replacement);
					RequestedTemplates.clear();
					RetiredObjects.clear();
					WorldGeneration = SnapshotValue.Cursor.Scope;
					Metrics.StructuralTemplateBytes = ReplacementBytes;
					CatalogCursor = SnapshotValue.Cursor;
					Metrics.CatalogObjects = Catalog.size();
					++Metrics.CatalogRefreshes;
					return true;
				} catch (const std::exception &Failure) {
					Error = Failure.what();
					return false;
				}
			}
			try {
				std::map<ObjectId, std::uint64_t> Touched;
				std::set<ObjectId> PendingRetired;
				for (const auto &Record : Read.Records) {
					const bool AffectsTemplate = std::visit(
						[](const auto &Change) {
							using Type = std::decay_t<decltype(Change)>;
							if constexpr (std::is_same_v<Type, PropertyUpdatedChange>) return Change.Replicated;
							return true;
						},
						Record.Payload
					);
					if (AffectsTemplate) Touched[Record.Object] = Record.Sequence;
				}
				std::map<ObjectId, std::shared_ptr<const StructuralMaterializationTemplate>> Replacements;
				for (const auto &[Object, StructuralRevision] : Touched) {
					auto Live = ObjectRegistry::Get().Lookup(Object);
					if (!Live || Live->GetDestroyed() || Live->IsDestroying()) {
						PendingRetired.insert(Object);
						continue;
					}
					Replacements.emplace(
						Object, MakeTemplate(WorldGeneration, StructuralRevision, CaptureSnapshotObject(Live))
					);
				}
				for (auto Iterator = Replacements.begin(); Iterator != Replacements.end();) {
					auto Existing = Catalog.find(Iterator->first);
					if (Existing == Catalog.end()) {
						++Iterator;
						continue;
					}
					Metrics.StructuralTemplateBytes -= std::min<std::uint64_t>(
						Metrics.StructuralTemplateBytes, Existing->second->RetainedBytes
					);
					SaturatingAdd(Metrics.StructuralTemplateBytes, Iterator->second->RetainedBytes);
					SaturatingAdd(Metrics.StructuralTemplateInvalidations, 1);
					SaturatingAdd(Metrics.StructuralTemplateBuilds, 1);
					RequestedTemplates.erase(Iterator->first);
					RetiredObjects.erase(Iterator->first);
					Existing->second = std::move(Iterator->second);
					Iterator = Replacements.erase(Iterator);
				}
				for (const auto &[Object, Template] : Replacements) {
					(void)Object;
					SaturatingAdd(Metrics.StructuralTemplateBytes, Template->RetainedBytes);
					SaturatingAdd(Metrics.StructuralTemplateBuilds, 1);
				}
				Catalog.merge(Replacements);
				for (const auto Object : PendingRetired)
					if (!RetiredObjects.contains(Object)) {
						RequestedTemplates.erase(Object);
						SaturatingAdd(Metrics.StructuralTemplateInvalidations, Catalog.contains(Object) ? 1 : 0);
					}
				RetiredObjects.merge(PendingRetired);
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
			const auto &Publication = Found->second->Publication;
			if (Publication.Parent) Pending.push_back({*Publication.Parent, Depth + 1});
			for (const auto &[Name, Value] : Publication.Properties) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				if (Reference && IsHardReference(Publication, Name))
					Pending.push_back({Reference->Object.ToObjectId(), Depth + 1});
			}
		}
		Metrics.DependencyObjects += Closure.size();
		return true;
	}

	void ReplicationCoordinator::CompactPendingQueues(PeerState &Peer) {
		if (Peer.PendingTransitions.empty()) {
			Peer.CriticalQueue.clear();
			Peer.OrdinaryQueue.clear();
			Peer.LeavingDependents.clear();
			return;
		}
		const auto MaximumQueueEntries = Peer.PendingTransitions.size() * 2 + 64;
		if (Peer.CriticalQueue.size() + Peer.OrdinaryQueue.size() <= MaximumQueueEntries) return;
		std::vector<std::pair<ObjectId, PendingTransition>> Current;
		Current.reserve(Peer.PendingTransitions.size());
		for (const auto &[Object, Transition] : Peer.PendingTransitions)
			Current.emplace_back(Object, Transition);
		std::ranges::sort(Current, [](const auto &Left, const auto &Right) {
			return Left.second.PendingSinceTick != Right.second.PendingSinceTick
					   ? Left.second.PendingSinceTick < Right.second.PendingSinceTick
					   : Left.first < Right.first;
		});
		Peer.CriticalQueue.clear();
		Peer.OrdinaryQueue.clear();
		for (const auto &[Object, Transition] : Current) {
			auto &Queue = Transition.Critical ? Peer.CriticalQueue : Peer.OrdinaryQueue;
			Queue.push_back({Object, Transition.Token});
		}
	}

	void ReplicationCoordinator::ApplyPreparedCommit(PeerState &Peer, PreparedStructuralCommit Commit) {
		Peer.NextSequence = Commit.NextSequence;
		if (Commit.JournalCursor) Peer.JournalCursor = *Commit.JournalCursor;
		for (const auto Object : Commit.Entering) {
			Peer.View.KnownObjects.insert(Object);
			PendingTransitionCount -= std::min<std::size_t>(
				PendingTransitionCount, Peer.PendingTransitions.erase(Object)
			);
		}
		for (const auto Object : Commit.Leaving) {
			Peer.View.ForgetReplica(Object);
			PendingTransitionCount -= std::min<std::size_t>(
				PendingTransitionCount, Peer.PendingTransitions.erase(Object)
			);
		}
		RefreshRelevantObjects(Peer.View, Peer.DesiredObjects);
		SaturatingAdd(Metrics.ObjectsPublished, Commit.PublishedObjects);
		SaturatingAdd(Metrics.ObjectsUnpublished, Commit.UnpublishedObjects);
		SaturatingAdd(Metrics.ObjectsDestroyed, Commit.DestroyedObjects);
		SaturatingAdd(Metrics.StructuralDeadlineMisses, Commit.DeadlineMisses);
		CompactPendingQueues(Peer);
	}

	void ReplicationCoordinator::RefreshPendingMetrics(ReplicationMetrics &Snapshot) const {
		Snapshot.MaterializationBacklog = 0;
		Snapshot.StructuralPendingEnters = 0;
		Snapshot.StructuralPendingLeaves = 0;
		Snapshot.StructuralPendingCritical = 0;
		Snapshot.StructuralActivePeers = 0;
		Snapshot.StructuralOldestPendingAgeTicks = 0;
		Snapshot.StructuralCriticalOldestAgeTicks = 0;
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			SaturatingAdd(Snapshot.MaterializationBacklog, Peer.PendingTransitions.size());
			if (!Peer.PendingTransitions.empty()) SaturatingAdd(Snapshot.StructuralActivePeers, 1);
			for (const auto &[Object, Transition] : Peer.PendingTransitions) {
				(void)Object;
				if (Transition.Kind == PendingTransitionKind::Enter)
					SaturatingAdd(Snapshot.StructuralPendingEnters, 1);
				else
					SaturatingAdd(Snapshot.StructuralPendingLeaves, 1);
				if (Transition.Critical) SaturatingAdd(Snapshot.StructuralPendingCritical, 1);
				const auto Age = LatestSchedulingTick >= Transition.PendingSinceTick
									 ? LatestSchedulingTick - Transition.PendingSinceTick
									 : std::uint64_t{0};
				Snapshot.StructuralOldestPendingAgeTicks = std::max(Snapshot.StructuralOldestPendingAgeTicks, Age);
				if (Transition.Critical)
					Snapshot.StructuralCriticalOldestAgeTicks = std::max(
						Snapshot.StructuralCriticalOldestAgeTicks, Age
					);
			}
		}
	}

	ReplicationMetrics ReplicationCoordinator::GetMetrics() const {
		auto Snapshot = Metrics;
		RefreshPendingMetrics(Snapshot);
		return Snapshot;
	}

	bool ReplicationCoordinator::RecordDesiredState(
		PeerState &Peer, const PeerRelevanceSelection &Selection, std::uint64_t SimulationTick, std::string &Error
	) {
		if (Peer.PreparedCommit) {
			Error = "A structural frame is awaiting scheduler acceptance";
			return false;
		}
		if (Selection == Peer.LastSelection && Peer.DesiredCatalogCursor.Scope == CatalogCursor.Scope &&
			Peer.DesiredCatalogCursor.NextSequence == CatalogCursor.NextSequence && !Peer.DesiredObjects.empty()) {
			for (auto Iterator = Peer.PendingTransitions.begin(); Iterator != Peer.PendingTransitions.end();) {
				const bool StaleEnter = Iterator->second.Kind == PendingTransitionKind::Enter &&
										(RetiredObjects.contains(Iterator->first) ||
										 !Catalog.contains(Iterator->first));
				if (StaleEnter) {
					Iterator = Peer.PendingTransitions.erase(Iterator);
					--PendingTransitionCount;
					SaturatingAdd(Metrics.StructuralTransitionsCancelled, 1);
					continue;
				}
				if (Iterator->second.Kind == PendingTransitionKind::Leave && RetiredObjects.contains(Iterator->first) &&
					!Iterator->second.Critical) {
					if (Peer.NextPendingToken == std::numeric_limits<std::uint64_t>::max()) {
						Error = "Structural pending transition token is exhausted";
						return false;
					}
					Iterator->second.Critical = true;
					Iterator->second.Token = Peer.NextPendingToken++;
					Peer.CriticalQueue.push_back({Iterator->first, Iterator->second.Token});
					SaturatingAdd(Metrics.StructuralTransitionsReplanned, 1);
				}
				++Iterator;
			}
			CompactPendingQueues(Peer);
			return true;
		}

		std::set<ObjectId> Desired;
		if (!BuildDependencyClosure(Selection, Desired, Error)) return false;
		PeerRelevanceSelection RequiredSelection{
			.RequiredObjects = Selection.RequiredObjects,
			.DesiredObjects = Selection.RequiredObjects,
		};
		if (RequiredSelection.DesiredObjects.empty() && SourceRoot)
			RequiredSelection.RequiredObjects = RequiredSelection.DesiredObjects = {SourceRoot->GetObjectId()};
		std::set<ObjectId> Required;
		if (!BuildDependencyClosure(RequiredSelection, Required, Error)) return false;

		for (auto Iterator = Peer.PendingTransitions.begin(); Iterator != Peer.PendingTransitions.end();) {
			const bool Known = Peer.View.Knows(Iterator->first);
			const bool DesiredNow = Desired.contains(Iterator->first);
			const bool StillRequired = Iterator->second.Kind == PendingTransitionKind::Enter
										   ? DesiredNow && !Known && Catalog.contains(Iterator->first) &&
												 !RetiredObjects.contains(Iterator->first)
										   : !DesiredNow && Known;
			if (StillRequired) {
				++Iterator;
				continue;
			}
			Iterator = Peer.PendingTransitions.erase(Iterator);
			--PendingTransitionCount;
			SaturatingAdd(Metrics.StructuralTransitionsCancelled, 1);
		}

		auto AddPending = [&](ObjectId Object, PendingTransitionKind Kind, bool Critical) -> bool {
			auto Existing = Peer.PendingTransitions.find(Object);
			if (Existing != Peer.PendingTransitions.end()) {
				if (Existing->second.Kind == Kind && Existing->second.Critical == Critical) return true;
				if (Peer.NextPendingToken == std::numeric_limits<std::uint64_t>::max()) {
					Error = "Structural pending transition token is exhausted";
					return false;
				}
				Existing->second.Kind = Kind;
				Existing->second.Critical = Critical;
				Existing->second.Token = Peer.NextPendingToken++;
				auto &Queue = Critical ? Peer.CriticalQueue : Peer.OrdinaryQueue;
				Queue.push_back({Object, Existing->second.Token});
				SaturatingAdd(Metrics.StructuralTransitionsReplanned, 1);
				return true;
			}
			if (Peer.PendingTransitions.size() >= Configuration.MaximumPendingTransitionsPerPeer) {
				SaturatingAdd(Metrics.StructuralBacklogLimitFailures, 1);
				Error = "Peer structural pending transition limit exceeded";
				return false;
			}
			if (PendingTransitionCount >= Configuration.MaximumPendingTransitions) {
				SaturatingAdd(Metrics.StructuralBacklogLimitFailures, 1);
				Error = "Session structural pending transition limit exceeded";
				return false;
			}
			if (Peer.NextPendingToken == std::numeric_limits<std::uint64_t>::max()) {
				Error = "Structural pending transition token is exhausted";
				return false;
			}
			PendingTransition Transition{Kind, SimulationTick, Peer.NextPendingToken++, Critical};
			Peer.PendingTransitions.emplace(Object, Transition);
			++PendingTransitionCount;
			auto &Queue = Critical ? Peer.CriticalQueue : Peer.OrdinaryQueue;
			Queue.push_back({Object, Transition.Token});
			return true;
		};

		for (const auto Object : Desired)
			if (!Peer.View.Knows(Object) && !RetiredObjects.contains(Object) &&
				!AddPending(Object, PendingTransitionKind::Enter, Required.contains(Object)))
				return false;
		std::vector<ObjectId> KnownObjects(Peer.View.KnownObjects.begin(), Peer.View.KnownObjects.end());
		std::ranges::sort(KnownObjects);
		for (const auto Object : KnownObjects) {
			const auto Id = Object;
			if (!Desired.contains(Id) && !AddPending(Id, PendingTransitionKind::Leave, RetiredObjects.contains(Id)))
				return false;
		}

		Peer.LeavingDependents.clear();
		for (const auto &[Object, Transition] : Peer.PendingTransitions) {
			if (Transition.Kind != PendingTransitionKind::Leave) continue;
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end()) continue;
			const auto &Publication = Found->second->Publication;
			if (Publication.Parent) {
				auto Parent = Peer.PendingTransitions.find(*Publication.Parent);
				if (Parent != Peer.PendingTransitions.end() && Parent->second.Kind == PendingTransitionKind::Leave)
					Peer.LeavingDependents[*Publication.Parent].push_back(Object);
			}
			for (const auto &[Name, Value] : Publication.Properties) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				if (!Reference || !IsHardReference(Publication, Name)) continue;
				const auto Target = Reference->Object.ToObjectId();
				auto Dependency = Peer.PendingTransitions.find(Target);
				if (Dependency != Peer.PendingTransitions.end() &&
					Dependency->second.Kind == PendingTransitionKind::Leave)
					Peer.LeavingDependents[Target].push_back(Object);
			}
		}
		for (auto &[Object, Dependents] : Peer.LeavingDependents) {
			(void)Object;
			std::ranges::sort(Dependents);
			Dependents.erase(std::unique(Dependents.begin(), Dependents.end()), Dependents.end());
		}

		Peer.DesiredObjects = std::move(Desired);
		Peer.RequiredObjects = std::move(Required);
		Peer.LastSelection = Selection;
		Peer.DesiredCatalogCursor = CatalogCursor;
		RefreshRelevantObjects(Peer.View, Peer.DesiredObjects);
		CompactPendingQueues(Peer);
		return true;
	}

	PreparedPublishReplication ReplicationCoordinator::MakePeerPublish(
		ObjectId Object,
		const std::set<ObjectId> &Known,
		ReplicationMetrics &CandidateMetrics,
		std::set<ObjectId> &CandidateRequestedTemplates,
		bool AllowSoftReferencePatches
	) {
		auto Found = Catalog.find(Object);
		if (Found == Catalog.end()) return {};
		PreparedPublishReplication Publish{Object, Found->second};
		if (!StructuralTemplateReuseEnabled ||
			(!RequestedTemplates.contains(Object) && CandidateRequestedTemplates.insert(Object).second))
			SaturatingAdd(CandidateMetrics.StructuralTemplateMisses, 1);
		else {
			SaturatingAdd(CandidateMetrics.StructuralTemplateHits, 1);
			SaturatingAdd(CandidateMetrics.StructuralBytesReused, Found->second->RetainedBytes);
		}
		if (AllowSoftReferencePatches)
			for (const auto &[Name, Value] : Found->second->Publication.Properties) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				if (Reference && !Known.contains(Reference->Object.ToObjectId()) &&
					!IsHardReference(Found->second->Publication, Name))
					Publish.NilProperties.Add(Name);
			}
		SaturatingAdd(CandidateMetrics.PeerPatchOperations, 1);
		SaturatingAdd(CandidateMetrics.ReferencePatchOperations, Publish.NilProperties.Size());
		return Publish;
	}

	PreparedPublishReplication ReplicationCoordinator::MakePeerPublish(
		ObjectId Object,
		const ReplicationView &View,
		const std::set<ObjectId> &Entering,
		const std::set<ObjectId> &Leaving,
		ReplicationMetrics &CandidateMetrics,
		std::set<ObjectId> &CandidateRequestedTemplates
	) {
		auto Found = Catalog.find(Object);
		if (Found == Catalog.end()) return {};
		PreparedPublishReplication Publish{Object, Found->second};
		if (!StructuralTemplateReuseEnabled ||
			(!RequestedTemplates.contains(Object) && CandidateRequestedTemplates.insert(Object).second))
			SaturatingAdd(CandidateMetrics.StructuralTemplateMisses, 1);
		else {
			SaturatingAdd(CandidateMetrics.StructuralTemplateHits, 1);
			SaturatingAdd(CandidateMetrics.StructuralBytesReused, Found->second->RetainedBytes);
		}
		for (const auto &[Name, Value] : Found->second->Publication.Properties) {
			const auto *Reference = std::get_if<WireObjectReference>(&Value);
			if (!Reference) continue;
			const auto Target = Reference->Object.ToObjectId();
			const bool KnownAfterFrame = (View.Knows(Target) && !Leaving.contains(Target)) || Entering.contains(Target);
			if (!KnownAfterFrame && !IsHardReference(Found->second->Publication, Name)) Publish.NilProperties.Add(Name);
		}
		SaturatingAdd(CandidateMetrics.PeerPatchOperations, 1);
		SaturatingAdd(CandidateMetrics.ReferencePatchOperations, Publish.NilProperties.Size());
		return Publish;
	}

	ReplicationIntent ReplicationCoordinator::FinalizePeerPublish(PreparedPublishReplication Publish) const {
		if (StructuralTemplateReuseEnabled) return ReplicationIntent(std::move(Publish));
		auto Publication = Publish.Template->Publication;
		for (std::size_t Index = 0; Index < Publish.NilProperties.Size(); ++Index) {
			const auto Name = Publish.NilProperties[Index];
			auto Value = std::ranges::find_if(Publication.Properties, [&](const auto &Entry) {
				return Entry.first == Name;
			});
			if (Value != Publication.Properties.end()) Value->second = std::monostate{};
		}
		return ReplicationIntent(std::move(Publication));
	}

	ReplicationProduceResult ReplicationCoordinator::ProduceRelevanceFrame(
		ConnectionId Connection,
		ReplicationMessageKind Kind,
		std::size_t MaximumTransitions,
		std::uint64_t SimulationTick,
		bool CriticalOnly,
		std::size_t MaximumFrameBytes
	) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		if (Peer->second.PreparedCommit) return {{}, "A structural frame is awaiting scheduler acceptance"};
		if (MaximumTransitions == 0 || MaximumTransitions > MaximumReplicationOperationsPerFrame ||
			MaximumFrameBytes == 0 || MaximumFrameBytes > MaximumReplicationFrameBytes)
			return {{}, "Structural transition work limit is invalid"};
		const auto Started = std::chrono::steady_clock::now();
		const auto RefreshStarted = std::chrono::steady_clock::now();
		std::string Error;
		if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
		Metrics.SnapshotCaptureCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - RefreshStarted)
				.count()
		);
		const auto SelectionStarted = std::chrono::steady_clock::now();
		auto &CurrentPeer = Peer->second;
		auto CandidateMetrics = Metrics;
		std::set<ObjectId> CandidateRequestedTemplates;
		SaturatingAdd(CandidateMetrics.PeerMaterializationPlans, 1);
		SaturatingAdd(CandidateMetrics.StructuralSchedulingTicks, 1);
		SaturatingAdd(CandidateMetrics.StructuralTransitionsOffered, CurrentPeer.PendingTransitions.size());
		ReplicationFrame Frame{ReplicationProtocolVersion, Kind, CurrentPeer.View.Epoch, CurrentPeer.NextSequence};
		if (Kind == ReplicationMessageKind::Baseline) Frame.Schema = CaptureReplicationSchemaCompatibility();
		std::set<ObjectId> Leaving;
		std::set<ObjectId> Entering;
		std::map<ObjectId, std::size_t> EnterReferenceFixupCosts;
		std::map<ObjectId, std::size_t> LeaveReferenceFixupCosts;
		std::size_t HardNilLeaveFixupCost = 0;
		if (Kind == ReplicationMessageKind::Incremental) {
			for (const auto Referrer : CurrentPeer.DesiredObjects) {
				if (!CurrentPeer.View.Knows(Referrer)) continue;
				auto Object = Catalog.find(Referrer);
				if (Object == Catalog.end()) continue;
				for (const auto &[Name, Value] : Object->second->Publication.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (Reference) {
						++EnterReferenceFixupCosts[Reference->Object.ToObjectId()];
						if (!IsHardReference(Object->second->Publication, Name))
							++LeaveReferenceFixupCosts[Reference->Object.ToObjectId()];
					} else if (std::holds_alternative<std::monostate>(Value) &&
							   IsHardReference(Object->second->Publication, Name)) {
						++HardNilLeaveFixupCost;
					}
				}
			}
		}
		std::size_t Remaining = MaximumTransitions;
		std::size_t LargestSelectedGroupWork = 1;
		const auto MaximumExamined = std::max<std::size_t>(64, MaximumTransitions * 4);
		std::size_t Examined = 0;
		auto CollectGroup = [&](ObjectId Candidate, std::set<ObjectId> &Group) -> bool {
			std::vector<ObjectId> Pending{Candidate};
			for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
				SaturatingAdd(CandidateMetrics.StructuralDependencyPlanOperations, 1);
				const auto Object = Pending[Index];
				auto Transition = CurrentPeer.PendingTransitions.find(Object);
				if (Transition == CurrentPeer.PendingTransitions.end() ||
					Transition->second.Kind != CurrentPeer.PendingTransitions.at(Candidate).Kind ||
					!Group.insert(Object).second)
					continue;
				if (Transition->second.Kind == PendingTransitionKind::Leave) {
					auto Dependents = CurrentPeer.LeavingDependents.find(Object);
					if (Dependents != CurrentPeer.LeavingDependents.end())
						Pending.insert(Pending.end(), Dependents->second.begin(), Dependents->second.end());
					continue;
				}
				auto Found = Catalog.find(Object);
				if (Found == Catalog.end() || RetiredObjects.contains(Object)) continue;
				const auto &Publication = Found->second->Publication;
				if (Publication.Parent && !CurrentPeer.View.Knows(*Publication.Parent))
					Pending.push_back(*Publication.Parent);
				for (const auto &[Name, Value] : Publication.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (Reference && IsHardReference(Publication, Name) &&
						!CurrentPeer.View.Knows(Reference->Object.ToObjectId()))
						Pending.push_back(Reference->Object.ToObjectId());
				}
			}
			return Group.size() <= Configuration.PeerQuantum && Group.size() <= MaximumRelevanceTransitionsPerFrame;
		};
		auto ProcessQueue = [&](std::deque<PendingQueueEntry> &Queue, bool Critical) -> bool {
			const auto QueueEntries = Queue.size();
			for (std::size_t Index = 0; Index < QueueEntries && Remaining != 0 && Examined < MaximumExamined; ++Index) {
				const auto Entry = Queue.front();
				Queue.pop_front();
				Queue.push_back(Entry);
				++Examined;
				auto Transition = CurrentPeer.PendingTransitions.find(Entry.Object);
				if (Transition == CurrentPeer.PendingTransitions.end() || Transition->second.Token != Entry.Token ||
					Transition->second.Critical != Critical)
					continue;
				auto &Selected = Transition->second.Kind == PendingTransitionKind::Enter ? Entering : Leaving;
				if (Selected.contains(Entry.Object)) continue;
				std::set<ObjectId> Group;
				if (!CollectGroup(Entry.Object, Group)) {
					SaturatingAdd(CandidateMetrics.DependencyLimitFailures, 1);
					Error = "Atomic replication dependency group exceeds the transition work limit";
					return false;
				}
				std::size_t NewWork = 0;
				for (const auto Object : Group) {
					if (Selected.contains(Object)) continue;
					++NewWork;
					const auto &FixupCosts = Transition->second.Kind == PendingTransitionKind::Enter
												 ? EnterReferenceFixupCosts
												 : LeaveReferenceFixupCosts;
					if (auto Cost = FixupCosts.find(Object); Cost != FixupCosts.end()) NewWork += Cost->second;
				}
				if (Transition->second.Kind == PendingTransitionKind::Leave && Leaving.empty())
					NewWork += HardNilLeaveFixupCost;
				if (NewWork > Configuration.PeerQuantum) {
					Error = "Structural dependency group and reference fixups exceed the peer quantum";
					return false;
				}
				if (NewWork > Remaining) continue;
				LargestSelectedGroupWork = std::max(LargestSelectedGroupWork, NewWork);
				Selected.insert(Group.begin(), Group.end());
				Remaining -= NewWork;
			}
			return true;
		};
		if (!ProcessQueue(CurrentPeer.CriticalQueue, true)) return {{}, std::move(Error)};
		if (!CriticalOnly && !ProcessQueue(CurrentPeer.OrdinaryQueue, false)) return {{}, std::move(Error)};
		const auto SelectedObjectTransitionCount = Entering.size() + Leaving.size();
		SaturatingAdd(
			CandidateMetrics.StructuralTransitionsDeferredByBudget,
			CurrentPeer.PendingTransitions.size() > SelectedObjectTransitionCount
				? CurrentPeer.PendingTransitions.size() - SelectedObjectTransitionCount
				: 0
		);
		CandidateMetrics.StructuralSelectionCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - SelectionStarted)
				.count()
		);
		Frame.Operations.reserve(Entering.size() + Leaving.size());

		if (Kind == ReplicationMessageKind::Incremental && !Leaving.empty()) {
			for (const auto Referrer : CurrentPeer.DesiredObjects) {
				if (!CurrentPeer.View.Knows(Referrer) || Entering.contains(Referrer)) continue;
				auto Object = Catalog.find(Referrer);
				if (Object == Catalog.end()) continue;
				const auto &Publication = Object->second->Publication;
				for (const auto &[Name, Value] : Publication.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					const auto *Property = FindNativeProperty(Publication, Name);
					const bool CurrentHardReferenceIsNil = Property &&
														   Property->SemanticType ==
															   InstanceProperty::DataType::ObjectReference &&
														   IsHardReference(Publication, Name) &&
														   std::holds_alternative<std::monostate>(Value);
					const bool SoftTargetLeaves = Reference && !IsHardReference(Publication, Name) &&
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
			auto Publish = MakePeerPublish(
				Object, CurrentPeer.View, Entering, Leaving, CandidateMetrics, CandidateRequestedTemplates
			);
			if (!PublishReferencesKnown(CurrentPeer.View, Publish, Entering))
				return {{}, "Hard materialization dependency is not available"};
			Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
		}

		std::uint64_t UnpublishedObjects = 0;
		std::uint64_t DestroyedObjects = 0;
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
					++DestroyedObjects;
				} else {
					Frame.Operations.push_back({Frame.Epoch, UnpublishReplication{Object}});
					++UnpublishedObjects;
				}
			}
			for (const auto Referrer : CurrentPeer.DesiredObjects) {
				if (Entering.contains(Referrer) || !CurrentPeer.View.Knows(Referrer)) continue;
				auto Object = Catalog.find(Referrer);
				if (Object == Catalog.end()) continue;
				for (const auto &[Name, Value] : Object->second->Publication.Properties) {
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
			Metrics = CandidateMetrics;
			CompactPendingQueues(CurrentPeer);
			return {{}, "No replication relevance changes are available"};
		}
		if (Frame.Operations.size() > MaximumTransitions ||
			Frame.Operations.size() > MaximumReplicationOperationsPerFrame)
			return {{}, "Replication relevance frame exceeded its selected work limit"};
		const auto SelectedWorkCount = Frame.Operations.size();
		SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, SelectedWorkCount);
		if (Kind == ReplicationMessageKind::Baseline)
			CandidateMetrics.BaselineDiscoveryCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - SelectionStarted
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
		if (Encoded->size() > MaximumFrameBytes) {
			// Selection has not committed Known, sequence, or journal state. Retry a
			// smaller dependency-closed slice under the negotiated transport ceiling.
			// This exceptional path is logarithmically bounded; ordinary frames pay
			// only the comparison, and no transition or byte ceiling is increased.
			const auto MinimumWork = Kind == ReplicationMessageKind::Baseline
				? std::max(LargestSelectedGroupWork, GetPendingCriticalTransitionCount(Connection))
				: LargestSelectedGroupWork;
			const auto Reduced = std::max(MinimumWork, MaximumTransitions / 2);
			if (Reduced >= MaximumTransitions)
				return {{}, "Atomic structural group exceeds the negotiated reliable message limit"};
			return ProduceRelevanceFrame(Connection, Kind, Reduced, SimulationTick,
				CriticalOnly || (Kind == ReplicationMessageKind::Baseline && CurrentPeer.PendingTransitions.size() > Reduced),
				MaximumFrameBytes);
		}
		SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, SelectedWorkCount);
		CandidateMetrics.ScratchHighWaterBytes = std::max<std::uint64_t>(
			CandidateMetrics.ScratchHighWaterBytes, Frame.Operations.capacity() * sizeof(ReplicationOperation)
		);
		auto Next = CurrentPeer.NextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		std::uint64_t DeadlineMisses = 0;
		for (const auto Object : Entering) {
			auto Transition = CurrentPeer.PendingTransitions.find(Object);
			if (Transition != CurrentPeer.PendingTransitions.end()) {
				const auto Age = SimulationTick >= Transition->second.PendingSinceTick
									 ? SimulationTick - Transition->second.PendingSinceTick
									 : std::uint64_t{0};
				if (Age > Configuration.TransitionDeadlineTicks) SaturatingAdd(DeadlineMisses, 1);
			}
		}
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		CandidateMetrics.RelevanceTransitions += SelectedObjectTransitionCount;
		SaturatingAdd(CandidateMetrics.StructuralTransitionsPrepared, SelectedWorkCount);
		CandidateMetrics.RelevanceTransitionCpuNanoseconds += static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
		);
		if (Kind == ReplicationMessageKind::Baseline) {
			CandidateMetrics.BaselineObjects += Entering.size();
			CandidateMetrics.BaselineBytes += Encoded->size();
		} else
			CandidateMetrics.IncrementalBytes += Encoded->size();
		RequestedTemplates.merge(CandidateRequestedTemplates);
		Metrics = CandidateMetrics;
		PreparedStructuralCommit Commit{
			.Sequence = Frame.Sequence,
			.NextSequence = *Next,
			.JournalCursor = Kind == ReplicationMessageKind::Baseline ? std::optional<ChangeCursor>(CatalogCursor)
																	  : std::nullopt,
			.Entering = std::vector<ObjectId>(Entering.begin(), Entering.end()),
			.Leaving = std::vector<ObjectId>(Leaving.begin(), Leaving.end()),
			.PublishedObjects = Entering.size(),
			.UnpublishedObjects = UnpublishedObjects,
			.DestroyedObjects = DestroyedObjects,
			.DeadlineMisses = DeadlineMisses,
			.TransitionCount = SelectedWorkCount,
		};
		if (CurrentPeer.ExplicitSchedulerCommit)
			CurrentPeer.PreparedCommit = std::move(Commit);
		else
			ApplyPreparedCommit(CurrentPeer, std::move(Commit));
		return {std::move(Frame), {}, SelectedWorkCount};
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(ConnectionId Connection, ReplicationEpoch Epoch) {
		std::string Error;
		if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
		PeerRelevanceSelection Selection;
		for (const auto &[Object, State] : Catalog) {
			if (!State->Publication.Parent || !IsInitiallyRelevant || IsInitiallyRelevant(Object))
				Selection.DesiredObjects.push_back(Object);
		}
		Selection.RequiredObjects = Selection.DesiredObjects;
		auto Result = AddPeer(Connection, Epoch, Selection);
		if (Result.Succeeded()) Peers.at(Connection).PolicyManaged = false;
		return Result;
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(
		ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection
	) {
		return AddPeer(Connection, Epoch, Selection, false);
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeerBounded(
		ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection
	) {
		return AddPeer(Connection, Epoch, Selection, true);
	}

	ReplicationScheduleResult ReplicationCoordinator::RegisterPeerBounded(
		ConnectionId Connection, ReplicationEpoch Epoch, const PeerRelevanceSelection &Selection
	) {
		auto Registered = RegisterPeer(Connection, Epoch, Selection, true);
		if (!Registered.Succeeded()) return Registered;
		const auto CriticalTransitionCount = GetPendingCriticalTransitionCount(Connection);
		if (CriticalTransitionCount > Configuration.PeerQuantum) {
			RemovePeer(Connection);
			return {"Critical structural bootstrap exceeds its bounded transition quantum"};
		}
		return {};
	}

	ReplicationScheduleResult ReplicationCoordinator::RegisterPeer(
		ConnectionId Connection,
		ReplicationEpoch Epoch,
		const PeerRelevanceSelection &Selection,
		bool ExplicitSchedulerCommit
	) {
		if (!SourceRoot || !Connection.IsValid() || !Epoch.IsValid()) return {"Invalid replication peer or source"};
		if (Peers.contains(Connection)) return {"Replication peer is already registered"};
		for (const auto &[Existing, State] : Peers)
			if (Existing.Slot == Connection.Slot && State.View.Connection.IsValid())
				return {"A live replication peer already owns this connection slot"};
		PeerState State{{Connection, Epoch}, CatalogCursor, ReliableReplicationSequence(1)};
		State.PolicyManaged = true;
		State.ExplicitSchedulerCommit = ExplicitSchedulerCommit;
		auto [Peer, Added] = Peers.emplace(Connection, std::move(State));
		if (!Added) return {"Replication peer is already registered"};
		std::string Error;
		if (!RefreshCatalog(Error) || !RecordDesiredState(Peer->second, Selection, 0, Error)) {
			RemovePeer(Connection);
			return {std::move(Error)};
		}
		return {};
	}

	ReplicationProduceResult ReplicationCoordinator::AddPeer(
		ConnectionId Connection,
		ReplicationEpoch Epoch,
		const PeerRelevanceSelection &Selection,
		bool BoundOrdinaryTransitions
	) {
		auto Registered = RegisterPeer(Connection, Epoch, Selection, BoundOrdinaryTransitions);
		if (!Registered.Succeeded()) return {{}, std::move(Registered.Error)};
		const auto MaximumTransitions = BoundOrdinaryTransitions ? Configuration.MaximumTransitionsPerPeerTick
																 : std::min<std::size_t>(
																	   MaximumReplicationOperationsPerFrame,
																	   Peers.at(Connection).PendingTransitions.size()
																   );
		const bool CriticalOnly = BoundOrdinaryTransitions && Peers.at(Connection).PendingTransitions.size() >
																  Configuration.MaximumTransitionsPerPeerTick;
		const auto CriticalTransitionCount = static_cast<std::size_t>(std::ranges::count_if(
			Peers.at(Connection).PendingTransitions, [](const auto &Entry) { return Entry.second.Critical; }
		));
		if (BoundOrdinaryTransitions && CriticalTransitionCount > MaximumTransitions) {
			RemovePeer(Connection);
			return {{}, "Critical structural bootstrap exceeds its bounded transition limit"};
		}
		auto Result = ProduceRelevanceFrame(
			Connection, ReplicationMessageKind::Baseline, MaximumTransitions, 0, CriticalOnly
		);
		if (!Result.Succeeded()) RemovePeer(Connection);
		return Result;
	}

	ReplicationProduceResult
	ReplicationCoordinator::UpdateRelevance(ConnectionId Connection, const PeerRelevanceSelection &Selection) {
		if (StandaloneSchedulingTick != std::numeric_limits<std::uint64_t>::max()) ++StandaloneSchedulingTick;
		auto Recorded = RecordDesiredState(Connection, Selection, StandaloneSchedulingTick);
		if (!Recorded.Succeeded()) return {{}, std::move(Recorded.Error)};
		return ProducePendingRelevance(Connection, MaximumRelevanceTransitionsPerFrame, StandaloneSchedulingTick);
	}

	ReplicationScheduleResult ReplicationCoordinator::RecordDesiredState(
		ConnectionId Connection, const PeerRelevanceSelection &Selection, std::uint64_t SimulationTick
	) {
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {"Replication peer is not registered"};
		std::string Error;
		if (!RefreshCatalog(Error) || !RecordDesiredState(Peer->second, Selection, SimulationTick, Error))
			return {std::move(Error)};
		return {};
	}

	ReplicationProduceResult ReplicationCoordinator::ProducePendingRelevance(
		ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes
	) {
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		return ProduceRelevanceFrame(
			Connection, ReplicationMessageKind::Incremental, MaximumTransitions, SimulationTick, false, MaximumFrameBytes
		);
	}

	ReplicationProduceResult ReplicationCoordinator::ProducePendingBaseline(
		ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes
	) {
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		const bool CriticalOnly = Peer->second.PendingTransitions.size() > MaximumTransitions;
		return ProduceRelevanceFrame(
			Connection, ReplicationMessageKind::Baseline, MaximumTransitions, SimulationTick, CriticalOnly, MaximumFrameBytes
		);
	}

	ReplicationProduceResult
	ReplicationCoordinator::ProduceIncremental(ConnectionId Connection, std::size_t MaximumTransitions, std::size_t MaximumFrameBytes) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		if (Peer->second.PreparedCommit) return {{}, "A structural frame is awaiting scheduler acceptance"};
		std::string CatalogError;
		if (!RefreshCatalog(CatalogError)) return {{}, std::move(CatalogError)};
		auto CandidateMetrics = Metrics;
		std::set<ObjectId> CandidateRequestedTemplates;
		if (MaximumTransitions == 0 || MaximumTransitions > MaximumReplicationOperationsPerFrame ||
			MaximumFrameBytes == 0 || MaximumFrameBytes > MaximumReplicationFrameBytes)
			return {{}, "Replication transition work limit is invalid"};
		const bool PolicyManaged = Peer->second.PolicyManaged;
		CandidateMetrics.StructuralMaximumJournalLagRecords = std::max(
			CandidateMetrics.StructuralMaximumJournalLagRecords,
			CatalogCursor.NextSequence >= Peer->second.JournalCursor.NextSequence
				? CatalogCursor.NextSequence - Peer->second.JournalCursor.NextSequence
				: std::uint64_t{0}
		);
		auto Read = ChangeJournal::Get().Read(
			Peer->second.JournalCursor, PolicyManaged ? MaximumWireJournalRecords : MaximumTransitions
		);
		if (Read.Status == ChangeReadStatus::ResnapshotRequired) {
			SaturatingAdd(CandidateMetrics.StructuralJournalLagFailures, 1);
			Metrics = CandidateMetrics;
			return {{}, "Authoritative journal cursor requires a new baseline"};
		}
		SaturatingAdd(CandidateMetrics.StructuralTransitionsOffered, Read.Records.size());
		if (Read.Records.empty()) return {{}, "No replication changes are available"};
		auto CandidateView = Peer->second.View;
		auto CandidateNextSequence = Peer->second.NextSequence;
		ReplicationFrame Frame{
			ReplicationProtocolVersion, ReplicationMessageKind::Incremental, CandidateView.Epoch, CandidateNextSequence
		};
		Frame.Operations.reserve(std::min(MaximumTransitions, Read.Records.size()));
		auto ProcessedCursor = Peer->second.JournalCursor;
		std::set<ObjectId> PublishedThisFrame;
		std::set<ObjectId> DestroyedThisFrame;
		std::set<ObjectId> BatchPublishObjects;
		for (const auto &Record : Read.Records)
			if (std::holds_alternative<ObjectCreatedChange>(Record.Payload) && Catalog.contains(Record.Object) &&
				(!PolicyManaged || CandidateView.RelevantObjects.contains(Record.Object)))
				BatchPublishObjects.insert(Record.Object);
		for (const auto &Record : Read.Records) {
			if (Frame.Operations.size() == MaximumTransitions) break;
			ProcessedCursor.NextSequence = Record.Sequence + 1;
			const auto Known = CandidateView.Knows(Record.Object);
			const auto Relevant = CandidateView.RelevantObjects.contains(Record.Object);
			if (PolicyManaged && !Relevant) continue;
			if (!Known && !PolicyManaged && IsInitiallyRelevant && !IsInitiallyRelevant(Record.Object)) continue;
			bool FailedReference = false;
			std::visit(
				[&](const auto &Change) {
					using Type = std::decay_t<decltype(Change)>;
					if constexpr (std::is_same_v<Type, ObjectCreatedChange>) {
						if (!Relevant) CandidateView.RelevantObjects.insert(Record.Object);
						if (Known) return;
						auto Found = Catalog.find(Record.Object);
						if (Found == Catalog.end()) return;
						std::set<ObjectId> Available(
							CandidateView.KnownObjects.begin(), CandidateView.KnownObjects.end()
						);
						Available.insert(BatchPublishObjects.begin(), BatchPublishObjects.end());
						auto Publish = MakePeerPublish(
							Record.Object, Available, CandidateMetrics, CandidateRequestedTemplates, PolicyManaged
						);
						const auto &Publication = Publish.Template->Publication;
						if (Publication.Parent && !CandidateView.Knows(*Publication.Parent) &&
							!BatchPublishObjects.contains(*Publication.Parent)) {
							CandidateView.RelevantObjects.erase(Record.Object);
							return;
						}
						if (!PublishReferencesKnown(CandidateView, Publish, BatchPublishObjects)) {
							FailedReference = true;
							return;
						}
						Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
						CandidateView.KnownObjects.insert(Record.Object);
						PublishedThisFrame.insert(Record.Object);
					} else if constexpr (std::is_same_v<Type, ObjectDestroyedChange>) {
						if (!Known) return;
						Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Record.Object}});
						CandidateView.ForgetReplica(Record.Object);
						CandidateView.RelevantObjects.erase(Record.Object);
						DestroyedThisFrame.insert(Record.Object);
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
								auto CurrentValue = CurrentObject->second->Publication.Properties.find(
									Change.PropertyName
								);
								if (CurrentValue != CurrentObject->second->Publication.Properties.end())
									Value = CurrentValue->second;
							}
							if (!ReferencesKnown(CandidateView, Value)) {
								if (PolicyManaged && CurrentObject != Catalog.end()) {
									if (!IsHardReference(CurrentObject->second->Publication, Change.PropertyName)) {
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
							if (Change.Value && !ReferencesKnown(CandidateView, *Change.Value)) {
								FailedReference = true;
								return;
							}
							Frame.Operations.push_back(
								{Frame.Epoch,
								 AttributeReplicationUpdate{Record.Object, Change.AttributeName, Change.Value}}
							);
						} else if constexpr (std::is_same_v<Type, ExtensionPropertyUpdatedChange>) {
							if (!ReferencesKnown(CandidateView, Change.Value)) {
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
							if (Change.Parent && !CandidateView.Knows(*Change.Parent)) {
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
			Peer->second.View = std::move(CandidateView);
			Peer->second.JournalCursor = ProcessedCursor;
			CandidateMetrics.ReplicationBacklog = ProcessedCursor.NextSequence < CatalogCursor.NextSequence ? 1 : 0;
			Metrics = CandidateMetrics;
			return {{}, "No relevant replication changes are available"};
		}
		if (Frame.Operations.size() > MaximumReplicationOperationsPerFrame)
			return {{}, "Replication frame operation limit exceeded"};
		auto Encoded = EncodeReplicationFrame(Frame);
		if (!Encoded) return {{}, Encoded.error().Format()};
		if (Encoded->size() > MaximumFrameBytes) {
			if (MaximumTransitions == 1)
				return {{}, "Structural operation exceeds the negotiated reliable message limit"};
			return ProduceIncremental(Connection, MaximumTransitions / 2, MaximumFrameBytes);
		}
		SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, Frame.Operations.size());
		CandidateMetrics.ScratchHighWaterBytes = std::max<std::uint64_t>(
			CandidateMetrics.ScratchHighWaterBytes, Frame.Operations.capacity() * sizeof(ReplicationOperation)
		);
		auto Next = CandidateNextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		CandidateNextSequence = *Next;
		RequestedTemplates.merge(CandidateRequestedTemplates);
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, Frame.Operations.size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsPrepared, Frame.Operations.size());
		CandidateMetrics.IncrementalBytes += Encoded->size();
		CandidateMetrics.ReplicationBacklog = ProcessedCursor.NextSequence < CatalogCursor.NextSequence ? 1 : 0;
		if (!Peer->second.ExplicitSchedulerCommit) {
			SaturatingAdd(CandidateMetrics.ObjectsPublished, PublishedThisFrame.size());
			SaturatingAdd(CandidateMetrics.ObjectsDestroyed, DestroyedThisFrame.size());
		}
		Metrics = CandidateMetrics;
		const auto OperationCount = Frame.Operations.size();
		if (Peer->second.ExplicitSchedulerCommit) {
			Peer->second.PreparedCommit = PreparedStructuralCommit{
				.Sequence = Frame.Sequence,
				.NextSequence = CandidateNextSequence,
				.JournalCursor = ProcessedCursor,
				.Entering = std::vector<ObjectId>(PublishedThisFrame.begin(), PublishedThisFrame.end()),
				.Leaving = std::vector<ObjectId>(DestroyedThisFrame.begin(), DestroyedThisFrame.end()),
				.PublishedObjects = PublishedThisFrame.size(),
				.DestroyedObjects = DestroyedThisFrame.size(),
				.TransitionCount = OperationCount,
			};
		} else {
			Peer->second.View = std::move(CandidateView);
			Peer->second.NextSequence = CandidateNextSequence;
			Peer->second.JournalCursor = ProcessedCursor;
		}
		return {std::move(Frame), {}, OperationCount};
	}

	ReplicationProduceResult
	ReplicationCoordinator::SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end() || !Object.IsValid()) return {{}, "Replication peer or object is invalid"};
		std::string Error;
		if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
		auto CandidatePeer = Peer->second;
		auto CandidateMetrics = Metrics;
		std::set<ObjectId> CandidateRequestedTemplates;
		SaturatingAdd(CandidateMetrics.PeerMaterializationPlans, 1);
		ReplicationFrame Frame{
			ReplicationProtocolVersion,
			ReplicationMessageKind::Incremental,
			CandidatePeer.View.Epoch,
			CandidatePeer.NextSequence
		};
		if (Relevant) {
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end() || RetiredObjects.contains(Object))
				return {{}, "Cannot publish a stale authoritative object"};
			if (CandidatePeer.View.Knows(Object)) return {{}, "Object is already published"};
			auto Publish = MakePeerPublish(Object, {Object}, CandidateMetrics, CandidateRequestedTemplates, false);
			const auto &Publication = Publish.Template->Publication;
			if (Publication.Parent && !CandidatePeer.View.Knows(*Publication.Parent))
				return {{}, "Publish parent is not materialized for this peer"};
			if (!PublishReferencesKnown(CandidatePeer.View, Publish, {Object}))
				return {{}, "Publish references an object not materialized for this peer"};
			Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
			CandidatePeer.View.RelevantObjects.insert(Object);
			CandidatePeer.View.KnownObjects.insert(Object);
			++CandidateMetrics.ObjectsPublished;
		} else {
			if (!CandidatePeer.View.Knows(Object)) return {{}, "Object is already unpublished"};
			std::vector<ObjectId> Removed{Object};
			std::set<ObjectId> RemovedSet{Object};
			for (std::size_t Index = 0; Index < Removed.size(); ++Index) {
				for (const auto &[Candidate, Template] : Catalog) {
					if (!CandidatePeer.View.Knows(Candidate) || RemovedSet.contains(Candidate)) continue;
					const auto &Publication = Template->Publication;
					if ((Publication.Parent && RemovedSet.contains(*Publication.Parent)) ||
						ReferencesAny(Publication, RemovedSet)) {
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
		SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, Frame.Operations.size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsPrepared, Frame.Operations.size());
		auto Encoded = EncodeReplicationFrame(Frame);
		if (!Encoded) return {{}, Encoded.error().Format()};
		SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, Frame.Operations.size());
		CandidateMetrics.ScratchHighWaterBytes = std::max<std::uint64_t>(
			CandidateMetrics.ScratchHighWaterBytes, Frame.Operations.capacity() * sizeof(ReplicationOperation)
		);
		auto Next = CandidatePeer.NextSequence.TryNext();
		if (!Next) return {{}, "Reliable replication sequence is exhausted"};
		CandidatePeer.NextSequence = *Next;
		RequestedTemplates.merge(CandidateRequestedTemplates);
		Peer->second = std::move(CandidatePeer);
		CandidateMetrics.OperationsGenerated += Frame.Operations.size();
		CandidateMetrics.IncrementalBytes += Encoded->size();
		Metrics = CandidateMetrics;
		const auto OperationCount = Frame.Operations.size();
		return {std::move(Frame), {}, OperationCount};
	}

	bool ReplicationCoordinator::RemovePeer(ConnectionId Connection) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return false;
		PendingTransitionCount -= std::min(PendingTransitionCount, Peer->second.PendingTransitions.size());
		Peers.erase(Peer);
		return true;
	}

	const ReplicationView *ReplicationCoordinator::GetView(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer == Peers.end() ? nullptr : &Peer->second.View;
	}

	bool ReplicationCoordinator::HasPendingRelevance(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer != Peers.end() && !Peer->second.PendingTransitions.empty();
	}

	std::size_t ReplicationCoordinator::GetPendingCriticalTransitionCount(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return 0;
		return static_cast<std::size_t>(std::ranges::count_if(Peer->second.PendingTransitions, [](const auto &Entry) {
			return Entry.second.Critical;
		}));
	}

	bool ReplicationCoordinator::HasPendingStructuralWork() const {
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (!Peer.PendingTransitions.empty()) return true;
		}
		return false;
	}

	ReplicationScheduleResult
	ReplicationCoordinator::CommitSchedulerAcceptance(ConnectionId Connection, ReliableReplicationSequence Sequence) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {"Replication peer is not registered"};
		if (!Peer->second.PreparedCommit) return {"No structural frame is awaiting scheduler acceptance"};
		if (Peer->second.PreparedCommit->Sequence != Sequence)
			return {"Structural scheduler acceptance does not match the prepared frame"};
		auto Commit = std::move(*Peer->second.PreparedCommit);
		Peer->second.PreparedCommit.reset();
		SaturatingAdd(Metrics.StructuralTransitionsAccepted, Commit.TransitionCount);
		SaturatingAdd(Metrics.StructuralTransitionsCommitted, Commit.TransitionCount);
		ApplyPreparedCommit(Peer->second, std::move(Commit));
		return {};
	}

	void ReplicationCoordinator::RecordPeerFairnessRotation() {
		SaturatingAdd(Metrics.StructuralPeerFairnessRotations, 1);
	}

	void ReplicationCoordinator::RecordGlobalBudgetExhaustion() {
		SaturatingAdd(Metrics.StructuralGlobalBudgetExhaustions, 1);
	}
}

#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "PlanningLookup.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <utility>

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

		bool SameDependencies(const PublishReplication &Left, const PublishReplication &Right) {
			if (Left.Parent != Right.Parent || Left.ClassSchemaId != Right.ClassSchemaId) return false;
			auto EdgesMatch = [](const PublishReplication &Source, const PublishReplication &Other) {
				for (const auto &[Name, Value] : Source.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (!Reference || !IsHardReference(Source, Name)) continue;
					const auto Found = Other.Properties.find(Name);
					const auto *OtherReference = Found == Other.Properties.end()
						? nullptr : std::get_if<WireObjectReference>(&Found->second);
					if (!OtherReference || Reference->Object != OtherReference->Object) return false;
				}
				return true;
			};
			return EdgesMatch(Left, Right) && EdgesMatch(Right, Left);
		}

		template <class ParentReader> std::size_t AncestryDepth(ObjectId Object, const ParentReader &ReadParent) {
			std::size_t Depth = 0;
			std::array<ObjectId, MaximumDependencyClosureDepth + 1> Visited{};
			while (Depth <= MaximumDependencyClosureDepth) {
				const auto End = Visited.begin() + static_cast<std::ptrdiff_t>(Depth);
				if (std::find(Visited.begin(), End, Object) != End) break;
				Visited[Depth] = Object;
				const auto Parent = ReadParent(Object);
				if (!Parent) break;
				Object = *Parent;
				++Depth;
			}
			return Depth;
		}

		struct AncestryOrderedObject {
			ObjectId Object;
			std::size_t Depth;
		};
		template <class ParentReader> std::vector<AncestryOrderedObject> OrderByAncestry(
			const std::set<ObjectId> &Objects,
			const ParentReader &ReadParent,
			bool ParentFirst
		) {
			std::vector<AncestryOrderedObject> Ordered;
			Ordered.reserve(Objects.size());
			// Only already-selected objects reach this scratch vector. Compute the
			// bounded ancestry walk once per object, never inside O(n log n) comparisons.
			for (const auto Object : Objects) Ordered.push_back({Object, AncestryDepth(Object, ReadParent)});
			std::ranges::sort(Ordered, [ParentFirst](const auto &Left, const auto &Right) {
				if (Left.Depth == Right.Depth) return Left.Object < Right.Object;
				return ParentFirst ? Left.Depth < Right.Depth : Left.Depth > Right.Depth;
			});
			return Ordered;
		}

		void RefreshRelevantObjects(ReplicationView &View, const std::set<ObjectId> &Closure) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RelevantRebuild);
			runtime_detail::CountWork(runtime_detail::WorkCounter::RelevantRebuilt, Closure.size());
			View.RelevantObjects.clear();
			for (const auto Object : Closure)
				if (View.Knows(Object)) View.RelevantObjects.insert(Object);
		}
	}

	#include "ReplicationPlanning.hpp"

	bool StructuralReplicationConfiguration::IsValid() const {
		return MaximumTransitionsPerPeerTick != 0 &&
			   MaximumTransitionsPerPeerTick <= MaximumRelevanceTransitionsPerFrame && MaximumTransitionsPerTick != 0 &&
			   MaximumTransitionsPerTick <= MaximumStructuralTransitionsPerTick &&
			   MaximumTransitionsPerPeerTick <= MaximumTransitionsPerTick && PeerQuantum != 0 &&
			   PeerQuantum <= MaximumTransitionsPerPeerTick && MaximumPendingTransitionsPerPeer != 0 &&
			   MaximumPendingTransitionsPerPeer <= MaximumPeerDesiredObjects && MaximumPendingTransitions != 0 &&
			   MaximumPendingTransitionsPerPeer <= MaximumPendingTransitions &&
			   MaximumPendingTransitions <= MaximumStructuralPendingTransitions && TransitionDeadlineTicks != 0 &&
			   MaximumJournalRecordsPerPeerTick != 0 &&
			   MaximumJournalRecordsPerPeerTick <= MaximumStructuralJournalRecordsPerCall &&
			   MaximumJournalRecordsPerPeerTick <= MaximumJournalRecordsPerTick &&
			   MaximumJournalRecordsPerTick <= MaximumStructuralJournalRecordsPerTick &&
			   PlanningWorkPerTick >= 2 && PlanningWorkPerTick <= MaximumPlanningWorkPerTick &&
			   PlanningPeerQuantum != 0 && PlanningPeerQuantum <= PlanningWorkPerTick;
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
		PlanningRemaining = Configuration.PlanningWorkPerTick; // Tick zero also owns one finite allowance.
		if (!this->SourceRoot) return;
		auto SnapshotValue = CaptureSnapshot(this->SourceRoot);
		CatalogCursor = SnapshotValue.Cursor;
		DependencyCursor = CatalogCursor;
		PlanningCursor = CatalogCursor;
		WorldGeneration = SnapshotValue.Cursor.Scope;
		const auto StructuralRevision = SnapshotValue.Cursor.NextSequence - 1;
		for (auto &Object : SnapshotValue.Objects) {
			const auto Id = Object.Id.ToObjectId();
			auto Template = MakeTemplate(WorldGeneration, StructuralRevision, std::move(Object));
			SaturatingAdd(Metrics.StructuralTemplateBytes, Template->RetainedBytes);
			const auto [Entry, Added] = Catalog.emplace(Id, std::move(Template));
			(void)Added;
			SaturatingAdd(Metrics.CatalogReferenceIndexBytes, Entry->second.GetReferenceIndexBytes());
			SaturatingAdd(Metrics.StructuralTemplateBuilds, 1);
		}
		Metrics.CatalogObjects = Catalog.size();
	}

	void ReplicationCoordinator::BeginRetirementTick(std::uint64_t SimulationTick) {
		if (SimulationTick <= RetirementTick) return;
		RetirementTick = SimulationTick;
		RetirementRemaining = MaximumCatalogRetirementExaminationsPerTick;
	}

	ReplicationCoordinator::CatalogEntry::CatalogEntry(
		std::shared_ptr<const StructuralMaterializationTemplate> Value, std::shared_ptr<const ObjectId> RetentionValue)
		: Template(std::move(Value)), Retention(RetentionValue ? std::move(RetentionValue)
			: std::make_shared<const ObjectId>(Template->Publication.Object)) {
		ReferenceProperties.reserve(static_cast<std::size_t>(std::ranges::count_if(Template->Publication.Properties,
			[](const auto &Entry) { return std::holds_alternative<WireObjectReference>(Entry.second); })));
		for (const auto &Entry : Template->Publication.Properties) {
			if (!std::holds_alternative<WireObjectReference>(Entry.second)) continue;
			const auto *Property = FindNativeProperty(Template->Publication, Entry.first);
			if (Property && Property->SemanticType == InstanceProperty::DataType::ObjectReference)
				ReferenceProperties.push_back(&Entry);
		}
	}

	void ReplicationCoordinator::ProcessCatalogRetirement(std::uint64_t SimulationTick) {
		BeginRetirementTick(SimulationTick);
		ReclaimRetiredTemplates();
	}

	void ReplicationCoordinator::ReclaimRetiredTemplates() {
		if (RetiredObjects.empty() || RetirementRemaining == 0) return;
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::CatalogRetirement);
		// Visit at most one cycle per call, sharing a hard tick budget across all
		// calls. Full identities, not retained iterators, survive map mutations.
		const auto Count = std::min(RetirementRemaining, RetiredObjects.size());
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Iterator = RetiredObjects.upper_bound(RetirementAfter);
			if (Iterator == RetiredObjects.end()) Iterator = RetiredObjects.begin();
			RetirementAfter = *Iterator;
			--RetirementRemaining;
			SaturatingAdd(Metrics.CatalogRetirementExaminations, 1);
			runtime_detail::CountWork(runtime_detail::WorkCounter::RetirementObjectsExamined);
			auto Object = Catalog.find(*Iterator);
			// Every Known identity has accepted ancestry metadata. Its lease, and
			// any prepared-acceptance lease, prevents reclamation without probing
			// peers. No semantic decision is derived from this ownership count.
			if (Object != Catalog.end() && Object->second.Retention.use_count() != 1) continue;
			if (Object != Catalog.end()) {
				Metrics.CatalogReferenceIndexBytes -= Object->second.GetReferenceIndexBytes();
				Metrics.StructuralTemplateBytes -= std::min<std::uint64_t>(
					Metrics.StructuralTemplateBytes, Object->second->RetainedBytes);
				Catalog.erase(Object);
				SaturatingAdd(Metrics.CatalogRetirementReleases, 1);
				runtime_detail::CountWork(runtime_detail::WorkCounter::RetirementTemplatesReleased);
			}
			RequestedTemplates.erase(*Iterator);
			RetiredObjects.erase(Iterator);
		}
		Metrics.CatalogRetirementMaximumTickExaminations = std::max<std::uint64_t>(
			Metrics.CatalogRetirementMaximumTickExaminations,
			MaximumCatalogRetirementExaminationsPerTick - RetirementRemaining);
		Metrics.CatalogRetiredObjects = RetiredObjects.size();
		Metrics.CatalogObjects = Catalog.size();
	}

	bool ReplicationCoordinator::RefreshCatalog(std::string &Error) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::CatalogRefresh);
		if (!SourceRoot || !CatalogCursor.Scope.IsValid()) {
			Error = "Replication catalog source is invalid";
			return false;
		}
		for (std::size_t Batch = 0; Batch < MaximumCatalogRefreshBatches; ++Batch) {
			auto Read = ChangeJournal::Get().Read(CatalogCursor, MaximumWireJournalRecords);
			if (Read.Status == ChangeReadStatus::ResnapshotRequired) {
				try {
					auto SnapshotValue = CaptureSnapshot(SourceRoot);
					std::map<ObjectId, CatalogEntry> Replacement;
					std::uint64_t ReplacementBytes = 0;
					std::uint64_t ReplacementIndexBytes = 0;
					const auto StructuralRevision = SnapshotValue.Cursor.NextSequence - 1;
					for (auto &Object : SnapshotValue.Objects) {
						const auto Id = Object.Id.ToObjectId();
						auto Template = MakeTemplate(SnapshotValue.Cursor.Scope, StructuralRevision, std::move(Object));
						SaturatingAdd(ReplacementBytes, Template->RetainedBytes);
						const auto Previous = Catalog.find(Id);
						const auto [Entry, Added] = Replacement.emplace(Id, CatalogEntry(std::move(Template),
							Previous != Catalog.end() ? Previous->second.Retention : nullptr));
						(void)Added;
						SaturatingAdd(ReplacementIndexBytes, Entry->second.GetReferenceIndexBytes());
					}
					SaturatingAdd(Metrics.StructuralTemplateInvalidations, Catalog.size());
					SaturatingAdd(Metrics.StructuralTemplateBuilds, Replacement.size());
					Catalog.swap(Replacement);
					RequestedTemplates.clear();
					RetiredObjects.clear();
					RetirementAfter = {};
					Metrics.CatalogRetiredObjects = 0;
					WorldGeneration = SnapshotValue.Cursor.Scope;
					Metrics.StructuralTemplateBytes = ReplacementBytes;
					Metrics.CatalogReferenceIndexBytes = ReplacementIndexBytes;
					CatalogCursor = SnapshotValue.Cursor;
					DependencyCursor = CatalogCursor;
					PlanningCursor = CatalogCursor;
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
				std::map<ObjectId, CatalogEntry> Replacements;
				for (const auto &[Object, StructuralRevision] : Touched) {
					auto Live = ObjectRegistry::Get().Lookup(Object);
					if (!Live || Live->GetDestroyed() || Live->IsDestroying()) {
						PendingRetired.insert(Object);
						continue;
					}
					const auto Previous = Catalog.find(Object);
					Replacements.emplace(Object, CatalogEntry(
						MakeTemplate(WorldGeneration, StructuralRevision, CaptureSnapshotObject(Live)),
						Previous != Catalog.end() ? Previous->second.Retention : nullptr));
				}
				const auto NewRetirements = std::ranges::count_if(PendingRetired,
					[&](ObjectId Object) { return !RetiredObjects.contains(Object); });
				if (static_cast<std::size_t>(NewRetirements) > MaximumRetiredCatalogObjects - RetiredObjects.size()) {
					Error = "Structural retired catalog limit exceeded";
					SaturatingAdd(Metrics.StructuralBacklogLimitFailures, 1);
					return false;
				}
				bool DependenciesChanged = false;
				bool ReferencesChanged = false;
				for (const auto &[Object, Template] : Replacements) {
					const auto Existing = Catalog.find(Object);
					DependenciesChanged = DependenciesChanged || Existing == Catalog.end() ||
						RetiredObjects.contains(Object) || !SameDependencies(Existing->second->Publication, Template->Publication);
					if (Existing != Catalog.end()) {
						const auto &Before = Existing->second.ReferenceProperties;
						const auto &After = Template.ReferenceProperties;
						ReferencesChanged = ReferencesChanged || Before.size() != After.size();
						for (std::size_t Index = 0; !ReferencesChanged && Index < Before.size(); ++Index)
							ReferencesChanged = *Before[Index] != *After[Index];
					}
				}
				for (const auto Object : PendingRetired)
					DependenciesChanged = DependenciesChanged || !RetiredObjects.contains(Object);
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
					Metrics.CatalogReferenceIndexBytes -= Existing->second.GetReferenceIndexBytes();
					SaturatingAdd(Metrics.CatalogReferenceIndexBytes, Iterator->second.GetReferenceIndexBytes());
					Existing->second = std::move(Iterator->second);
					Iterator = Replacements.erase(Iterator);
				}
				for (const auto &[Object, Template] : Replacements) {
					(void)Object;
					SaturatingAdd(Metrics.CatalogReferenceIndexBytes, Template.GetReferenceIndexBytes());
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
				Metrics.CatalogRetiredObjects = RetiredObjects.size();
				Metrics.CatalogRetiredHighWater = std::max<std::uint64_t>(
					Metrics.CatalogRetiredHighWater, RetiredObjects.size());
				if (DependenciesChanged) DependencyCursor = Read.Cursor;
				if (DependenciesChanged || ReferencesChanged) PlanningCursor = Read.Cursor;
				if (!Read.Records.empty()) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::CatalogBatches);
					runtime_detail::CountWork(DependenciesChanged ? runtime_detail::WorkCounter::CatalogTopologyBatches
						: runtime_detail::WorkCounter::CatalogPayloadOnlyBatches);
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
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::DependencyClosure);
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
		std::set<ObjectId> Required;
		{
		runtime_detail::WorkScope SeedWork(runtime_detail::WorkPhase::DependencySeedPreparation);
		Pending.reserve(Selection.DesiredObjects.size());
		Required.insert(Selection.RequiredObjects.begin(), Selection.RequiredObjects.end());
		for (const auto Object : Selection.DesiredObjects)
			Pending.push_back({Object, 0});
		for (const auto Object : Selection.RequiredObjects)
			Pending.push_back({Object, 0});
		runtime_detail::CountWork(runtime_detail::WorkCounter::DependencySeeds, Pending.size());
		}
		runtime_detail::WorkScope EdgeWork(runtime_detail::WorkPhase::DependencyEdgeWalk);
		for (std::size_t Index = 0; Index < Pending.size(); ++Index) {
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::DependencyClosure);
			const auto [Object, Depth] = Pending[Index];
			if (!Object.IsValid() || Closure.contains(Object)) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyDuplicates);
				runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::DependencyClosure, false);
				continue;
			}
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end() || RetiredObjects.contains(Object)) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyStale);
				runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::DependencyClosure, false);
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
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::DependencyClosure, true);
			if (Closure.size() > MaximumDependencyClosureObjects) {
				Error = "Replication dependency closure object limit exceeded";
				++Metrics.DependencyLimitFailures;
				return false;
			}
			const auto &Publication = Found->second->Publication;
			if (Publication.Parent) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyEdges);
				Pending.push_back({*Publication.Parent, Depth + 1});
			}
			for (const auto &[Name, Value] : Publication.Properties) {
				const auto *Reference = std::get_if<WireObjectReference>(&Value);
				if (Reference && IsHardReference(Publication, Name)) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyEdges);
					Pending.push_back({Reference->Object.ToObjectId(), Depth + 1});
				}
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

	ReplicationCoordinator::AcceptedParentMap ReplicationCoordinator::CaptureAcceptedParents(
		const PeerState &Peer, const ReplicationFrame &Frame, std::uint64_t JournalEnd) {
		AcceptedParentMap Parents;
		auto GetState = [&](ObjectId Object) -> AcceptedParentState & {
			auto [Entry, Inserted] = Parents.try_emplace(Object);
			if (Inserted) {
				const auto Previous = Peer.AcceptedParents.find(Object);
				if (Previous != Peer.AcceptedParents.end()) Entry->second = Previous->second;
			}
			return Entry->second;
		};
		auto SetReference = [&](AcceptedParentState &State, const InstanceProperty *Property, const WireValue &Value) {
			if (!Property || Property->SemanticType != InstanceProperty::DataType::ObjectReference) return;
			const auto Existing = std::ranges::find_if(State.References, [&](const auto &Entry) { return Entry.first == Property; });
			const auto *Reference = std::get_if<WireObjectReference>(&Value);
			if (Reference) {
				if (Existing != State.References.end()) Existing->second = Reference->Object.ToObjectId();
				else {
					if (State.References.size() == MaximumSnapshotPropertiesPerObject)
						throw std::length_error("Accepted native reference property limit exceeded");
					if (State.References.size() == State.References.capacity())
						State.References.reserve(std::min(MaximumSnapshotPropertiesPerObject,
							std::max<std::size_t>(1, State.References.capacity() * 2)));
					State.References.emplace_back(Property, Reference->Object.ToObjectId());
				}
			} else if (Existing != State.References.end()) State.References.erase(Existing);
		};
		auto PublishState = [&](const PublishReplication &Value, const PreparedPublishReplication *Prepared) {
			auto &State = GetState(Value.Object);
			State.Parent = Value.Parent.value_or(ObjectId{});
			State.JournalEnd = JournalEnd;
			State.References.clear();
			for (const auto &[Name, PropertyValue] : Value.Properties) {
				if (!std::holds_alternative<WireObjectReference>(PropertyValue) ||
					(Prepared && Prepared->NilProperties.Contains(Name))) continue;
				SetReference(State, FindNativeProperty(Value, Name), PropertyValue);
			}
		};
		for (const auto &Operation : Frame.Operations) std::visit([&](const auto &Value) {
			using Type = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<Type, PublishReplication>) PublishState(Value, nullptr);
			else if constexpr (std::is_same_v<Type, PreparedPublishReplication>)
				PublishState(Value.Template->Publication, &Value);
			else if constexpr (std::is_same_v<Type, ReparentReplication>) {
				auto &State = GetState(Value.Object);
				State.Parent = Value.Parent.value_or(ObjectId{});
				State.JournalEnd = JournalEnd;
			} else if constexpr (std::is_same_v<Type, PropertyReplicationUpdate>) {
				if (Value.DeclaringClassSchemaId) return; // Custom properties cannot hold references.
				if (!std::holds_alternative<WireObjectReference>(Value.Value) && !std::holds_alternative<std::monostate>(Value.Value)) return;
				const auto Object = Catalog.find(Value.Object);
				if (Object == Catalog.end()) throw std::logic_error("Prepared reference has no catalog identity");
				const auto *Property = FindNativeProperty(Object->second->Publication, Value.PropertyName);
				if (Property && Property->SemanticType == InstanceProperty::DataType::ObjectReference)
					SetReference(GetState(Value.Object), Property, Value.Value);
			}
		}, Operation.Intent);
		for (auto &[Object, Parent] : Parents) {
			const auto Entry = Catalog.find(Object);
			if (Entry == Catalog.end()) throw std::logic_error("Prepared ancestry has no structural catalog identity");
			Parent.CatalogRetention = Entry->second.Retention;
		}
		return Parents;
	}

	void ReplicationCoordinator::ApplyAcceptedParents(PeerState &Peer, AcceptedParentMap Parents) {
		for (auto Iterator = Parents.begin(); Iterator != Parents.end();) {
			if (!Peer.View.Knows(Iterator->first)) { Iterator = Parents.erase(Iterator); continue; }
			auto Existing = Peer.AcceptedParents.find(Iterator->first);
			if (Existing == Peer.AcceptedParents.end() || Existing->second.Parent != Iterator->second.Parent ||
				Existing->second.References != Iterator->second.References)
				SaturatingAdd(Peer.AcceptedRevision, 1);
			Peer.AcceptedReferenceBytes += Iterator->second.References.capacity() * sizeof(decltype(Iterator->second.References)::value_type);
			if (Existing == Peer.AcceptedParents.end()) { ++Iterator; continue; }
			Peer.AcceptedReferenceBytes -= Existing->second.References.capacity() * sizeof(decltype(Existing->second.References)::value_type);
			Existing->second = std::move(Iterator->second);
			Iterator = Parents.erase(Iterator);
		}
		// Nodes were allocated during preparation, before scheduler acceptance.
		Peer.AcceptedParents.merge(Parents);
	}

	void ReplicationCoordinator::ApplyPreparedCommit(PeerState &Peer, PreparedStructuralCommit Commit) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::StructuralCommit);
		Peer.NextSequence = Commit.NextSequence;
		if (!Commit.Entering.empty() || !Commit.Leaving.empty()) SaturatingAdd(Peer.AcceptedRevision, 1);
		if (Commit.JournalCursor) Peer.JournalCursor = *Commit.JournalCursor;
		{
		runtime_detail::WorkScope KnownWork(runtime_detail::WorkPhase::KnownCommit);
		for (const auto Object : Commit.Entering) {
			Peer.View.KnownObjects.insert(Object);
			if (Peer.DesiredObjects.contains(Object)) Peer.View.RelevantObjects.insert(Object);
			if (Commit.PublicationJournalEnd > Peer.JournalCursor.NextSequence)
				Peer.PublicationJournalEnds[Object] = Commit.PublicationJournalEnd;
			PendingTransitionCount -= std::min<std::size_t>(
				PendingTransitionCount, Peer.PendingTransitions.erase(Object)
			);
		}
		for (const auto Object : Commit.Leaving) {
			Peer.View.ForgetReplica(Object);
			Peer.View.RelevantObjects.erase(Object);
			Peer.PublicationJournalEnds.erase(Object);
			if (const auto Accepted = Peer.AcceptedParents.find(Object); Accepted != Peer.AcceptedParents.end())
				Peer.AcceptedReferenceBytes -= Accepted->second.References.capacity() * sizeof(decltype(Accepted->second.References)::value_type);
			Peer.AcceptedParents.erase(Object);
			PendingTransitionCount -= std::min<std::size_t>(
				PendingTransitionCount, Peer.PendingTransitions.erase(Object)
			);
		}
		}
		runtime_detail::MeasureWork(runtime_detail::WorkPhase::AcceptedMetadataCommit, [&] { ApplyAcceptedParents(Peer, std::move(Commit.Parents)); });
		// Desired cannot change while a prepared frame awaits acceptance. Only
		// its accepted Enters/Leaves change Desired intersect Known here; ordinary
		// property frames must not rebuild every unrelated membership node.
		runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::StructuralCommit, Commit.Entering.size() + Commit.Leaving.size());
		SaturatingAdd(Metrics.ObjectsPublished, Commit.PublishedObjects);
		SaturatingAdd(Metrics.ObjectsUnpublished, Commit.UnpublishedObjects);
		SaturatingAdd(Metrics.ObjectsDestroyed, Commit.DestroyedObjects);
		SaturatingAdd(Metrics.StructuralDeadlineMisses, Commit.DeadlineMisses);
		CompactPendingQueues(Peer);
		if (Peer.Planned && (!Peer.PendingTransitions.empty() || Peer.Planning)) {
			if (!PlanningPeers.contains(Peer.View.Connection)) Peer.LastPlanningServiceTick = std::max<std::uint64_t>(1, PlanningTick);
			PlanningPeers.insert(Peer.View.Connection);
		}
	}

	void ReplicationCoordinator::RefreshPendingMetrics(ReplicationMetrics &Snapshot) const {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GraphMetrics);
		Snapshot.MaterializationBacklog = 0;
		Snapshot.StructuralPendingEnters = 0;
		Snapshot.StructuralPendingLeaves = 0;
		Snapshot.StructuralPendingCritical = 0;
		Snapshot.StructuralActivePeers = 0;
		Snapshot.StructuralOldestPendingAgeTicks = 0;
		Snapshot.StructuralCriticalOldestAgeTicks = 0;
		Snapshot.AcceptedAncestryObjects = 0;
		Snapshot.AcceptedAncestryLogicalBytes = 0;
		Snapshot.CatalogRetentionLogicalBytes = Catalog.size() *
			(sizeof(CatalogEntry::Retention) + sizeof(ObjectId));
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			SaturatingAdd(Snapshot.AcceptedAncestryObjects, Peer.AcceptedParents.size());
			SaturatingAdd(Snapshot.AcceptedAncestryLogicalBytes, Peer.AcceptedParents.size() * sizeof(AcceptedParentMap::value_type));
			SaturatingAdd(Snapshot.AcceptedAncestryLogicalBytes, Peer.AcceptedReferenceBytes);
			SaturatingAdd(Snapshot.MaterializationBacklog, Peer.PendingTransitions.size());
			if (!Peer.PendingTransitions.empty()) SaturatingAdd(Snapshot.StructuralActivePeers, 1);
			for (const auto &[Object, Transition] : Peer.PendingTransitions) {
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::GraphMetrics);
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
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::DesiredState);
		if (Peer.PreparedCommit) {
			Error = "A structural frame is awaiting scheduler acceptance";
			return false;
		}
		const bool SelectionChanged = Selection != Peer.LastSelection;
		const bool TopologyChanged = Peer.DesiredDependencyCursor.Scope != DependencyCursor.Scope ||
			Peer.DesiredDependencyCursor.NextSequence != DependencyCursor.NextSequence;
		if (!SelectionChanged && !TopologyChanged && !Peer.DesiredObjects.empty()) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::PlanHits);
			SaturatingAdd(Metrics.DependencyPlanCacheHits, 1);
			for (auto Iterator = Peer.PendingTransitions.begin(); Iterator != Peer.PendingTransitions.end();) {
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::DesiredState);
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
		if (SelectionChanged) runtime_detail::CountWork(runtime_detail::WorkCounter::PlanMissSelection);
		if (TopologyChanged) runtime_detail::CountWork(runtime_detail::WorkCounter::PlanMissTopology);
		if (Peer.DesiredObjects.empty()) runtime_detail::CountWork(runtime_detail::WorkCounter::PlanMissEmpty);
		SaturatingAdd(Metrics.DependencyPlanRebuilds, 1);
		if (!BuildDependencyClosure(Selection, Desired, Error)) return false;
		PeerRelevanceSelection RequiredSelection{
			.RequiredObjects = Selection.RequiredObjects,
			.DesiredObjects = Selection.RequiredObjects,
		};
		if (RequiredSelection.DesiredObjects.empty() && SourceRoot)
			RequiredSelection.RequiredObjects = RequiredSelection.DesiredObjects = {SourceRoot->GetObjectId()};
		std::set<ObjectId> Required;
		if (!BuildDependencyClosure(RequiredSelection, Required, Error)) return false;

		{
		runtime_detail::WorkScope DiscoveryWork(runtime_detail::WorkPhase::PendingDiscovery);
		runtime_detail::CountWork(runtime_detail::WorkCounter::DiscoveryPeers);
		for (auto Iterator = Peer.PendingTransitions.begin(); Iterator != Peer.PendingTransitions.end();) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::PendingExamined);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::PendingDiscovery);
			const bool Known = Peer.View.Knows(Iterator->first);
			const bool DesiredNow = Desired.contains(Iterator->first);
			const bool StillRequired = Iterator->second.Kind == PendingTransitionKind::Enter
										   ? DesiredNow && !Known && Catalog.contains(Iterator->first) &&
												 !RetiredObjects.contains(Iterator->first)
										   : !DesiredNow && Known;
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::PendingDiscovery, StillRequired);
			if (StillRequired) {
				++Iterator;
				continue;
			}
			Iterator = Peer.PendingTransitions.erase(Iterator);
			--PendingTransitionCount;
			runtime_detail::CountWork(runtime_detail::WorkCounter::CandidateCancelled);
			SaturatingAdd(Metrics.StructuralTransitionsCancelled, 1);
		}

		auto AddPending = [&](ObjectId Object, PendingTransitionKind Kind, bool Critical) -> bool {
			runtime_detail::CountWork(runtime_detail::WorkCounter::CandidateAttempts);
			auto Existing = Peer.PendingTransitions.find(Object);
			if (Existing != Peer.PendingTransitions.end()) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::AlreadyPending);
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
				runtime_detail::CountWork(runtime_detail::WorkCounter::CandidateReplanned);
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
			runtime_detail::CountWork(runtime_detail::WorkCounter::CandidateInserted);
			++PendingTransitionCount;
			auto &Queue = Critical ? Peer.CriticalQueue : Peer.OrdinaryQueue;
			Queue.push_back({Object, Transition.Token});
			return true;
		};

		for (const auto Object : Desired) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::DesiredExamined);
			if (Peer.View.Knows(Object)) runtime_detail::CountWork(runtime_detail::WorkCounter::AlreadyKnown);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::PendingDiscovery);
			const bool NeedsEnter = !Peer.View.Knows(Object) && !RetiredObjects.contains(Object);
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::PendingDiscovery, NeedsEnter);
			if (NeedsEnter &&
				!AddPending(Object, PendingTransitionKind::Enter, Required.contains(Object)))
				return false;
		}
		std::vector<ObjectId> KnownObjects(Peer.View.KnownObjects.begin(), Peer.View.KnownObjects.end());
		std::ranges::sort(KnownObjects);
		for (const auto Object : KnownObjects) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::KnownExamined);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::PendingDiscovery);
			const auto Id = Object;
			const bool NeedsLeave = !Desired.contains(Id);
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::PendingDiscovery, NeedsLeave);
			if (NeedsLeave && !AddPending(Id, PendingTransitionKind::Leave, RetiredObjects.contains(Id)))
				return false;
		}
		}

		{
		runtime_detail::WorkScope IndexWork(runtime_detail::WorkPhase::LeaveDependencyIndex);
		Peer.LeavingDependents.clear();
		for (const auto &[Object, Transition] : Peer.PendingTransitions) {
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::LeaveDependencyIndex);
			if (Transition.Kind != PendingTransitionKind::Leave) continue;
			auto Found = Catalog.find(Object);
			if (Found == Catalog.end()) continue;
			const auto &Publication = Found->second->Publication;
			const auto AcceptedParent = Peer.AcceptedParents.find(Object);
			if (AcceptedParent == Peer.AcceptedParents.end()) {
				Error = "Known structural object has no accepted ancestry metadata";
				return false;
			}
			if (AcceptedParent->second.Parent.IsValid()) {
				auto Parent = Peer.PendingTransitions.find(AcceptedParent->second.Parent);
				if (Parent != Peer.PendingTransitions.end() && Parent->second.Kind == PendingTransitionKind::Leave)
					Peer.LeavingDependents[AcceptedParent->second.Parent].push_back(Object);
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
		}

		Peer.DesiredObjects = std::move(Desired);
		Peer.RequiredObjects = std::move(Required);
		Peer.LastSelection = Selection;
		Peer.DesiredDependencyCursor = DependencyCursor;
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
		PreparedPublishReplication Publish{Object, Found->second.Template};
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
		PreparedPublishReplication Publish{Object, Found->second.Template};
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
		std::size_t MaximumFrameBytes,
		std::size_t AvailableFrameBytes
	) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::StructuralSelection);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		if (Peer->second.Planned)
			return ProducePlannedFrame(Connection, Kind, MaximumTransitions, SimulationTick, MaximumFrameBytes, AvailableFrameBytes);
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
		// A bounded 3E pass may not have revisited this peer since an admission,
		// eviction, or hard-dependency change. Never publish its obsolete plan.
		// RecordDesiredState owns replanning; selection must not bypass its budget.
		if (CurrentPeer.DesiredDependencyCursor.Scope != DependencyCursor.Scope ||
			CurrentPeer.DesiredDependencyCursor.NextSequence != DependencyCursor.NextSequence)
			return {{}, "No replication relevance changes are available"};
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
		std::map<ObjectId, ReparentReplication> ParentMoves;
		std::map<ObjectId, std::vector<ObjectId>> EnterParentFixups;
		std::map<ObjectId, std::vector<ObjectId>> LeaveParentFixups;
		struct RemovalReference {
			ObjectId Referrer;
			const InstanceProperty *Property;
			ObjectId CurrentTarget; // Invalid is the current nil value.
		};
		// Call-local derived grouping, not a persistent reverse semantic graph.
		std::map<ObjectId, std::vector<RemovalReference>> RemovalReferences;
		if (Kind == ReplicationMessageKind::Incremental) {
			runtime_detail::WorkScope FixupWork(runtime_detail::WorkPhase::StructuralFixupPlanning);
			runtime_detail::CountWork(runtime_detail::WorkCounter::FixupPlanningCalls);
			{
			runtime_detail::WorkScope DesiredWork(runtime_detail::WorkPhase::FixupDesiredScan);
			auto CatalogPosition = Catalog.cbegin();
			auto AcceptedPosition = CurrentPeer.AcceptedParents.cbegin();
			for (const auto Referrer : CurrentPeer.DesiredObjects) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::FixupReferrers);
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::StructuralFixupPlanning);
				if (!CurrentPeer.View.Knows(Referrer)) continue;
				auto Object = detail::FindPlanningObject(Catalog, CatalogPosition, Referrer);
				if (Object == Catalog.end()) continue;
				const auto Accepted = detail::FindPlanningObject(CurrentPeer.AcceptedParents, AcceptedPosition, Referrer);
				if (Accepted == CurrentPeer.AcceptedParents.end())
					return {{}, "Known structural object has no accepted ancestry metadata"};
				const auto Parent = Object->second->Publication.Parent;
				if (Accepted->second.Parent != Parent.value_or(ObjectId{})) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::FixupParentChanges);
					ParentMoves.emplace(Referrer, ReparentReplication{Referrer, Parent});
					if (Parent && !CurrentPeer.View.Knows(*Parent)) {
						++EnterReferenceFixupCosts[*Parent];
						EnterParentFixups[*Parent].push_back(Referrer);
					}
					if (Accepted->second.Parent.IsValid()) {
						++LeaveReferenceFixupCosts[Accepted->second.Parent];
						LeaveParentFixups[Accepted->second.Parent].push_back(Referrer);
					}
				}
				if (Object->second.ReferenceProperties.empty())
					runtime_detail::CountWork(runtime_detail::WorkCounter::FixupEmptyReferenceObjects);
				std::optional<runtime_detail::WorkScope> ReferenceWork;
				if (!Object->second.ReferenceProperties.empty()) ReferenceWork.emplace(runtime_detail::WorkPhase::FixupReferenceCosts);
				for (const auto *ReferenceProperty : Object->second.ReferenceProperties) {
					const auto &[Name, Value] = *ReferenceProperty;
					runtime_detail::CountWork(runtime_detail::WorkCounter::FixupProperties);
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (Reference) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::FixupReferenceValues);
						++EnterReferenceFixupCosts[Reference->Object.ToObjectId()];
						runtime_detail::CountWork(runtime_detail::WorkCounter::FixupEnterCostCandidates);
						if (runtime_detail::ActiveWorkSample) {
							const auto Pending = CurrentPeer.PendingTransitions.find(Reference->Object.ToObjectId());
							if (Pending != CurrentPeer.PendingTransitions.end() && Pending->second.Kind == PendingTransitionKind::Enter)
								runtime_detail::CountWork(runtime_detail::WorkCounter::FixupPendingEnterCosts);
						}
					} else if (std::holds_alternative<std::monostate>(Value) &&
							   IsHardReference(Object->second->Publication, Name)) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::FixupHardNilValues);
					} else {
						runtime_detail::CountWork(runtime_detail::WorkCounter::FixupScalarValues);
					}
				}
			}
			}
			runtime_detail::WorkScope AcceptedWork(runtime_detail::WorkPhase::FixupAcceptedScan);
			for (const auto &[Referrer, Accepted] : CurrentPeer.AcceptedParents) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::AcceptedReferenceObjectsExamined);
				if (Accepted.References.empty()) runtime_detail::CountWork(runtime_detail::WorkCounter::AcceptedEmptyReferenceObjects);
				for (const auto &[Property, Target] : Accepted.References) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::AcceptedReferencesExamined);
					const auto Pending = CurrentPeer.PendingTransitions.find(Target);
					if (Pending == CurrentPeer.PendingTransitions.end() || Pending->second.Kind != PendingTransitionKind::Leave) continue;
					const auto Object = Catalog.find(Referrer);
					if (Object == Catalog.end()) return {{}, "Accepted reference referrer has no catalog identity"};
					const auto Value = Object->second->Publication.Properties.find(Property->Name);
					if (Value == Object->second->Publication.Properties.end())
						return {{}, "Accepted reference has no canonical current property"};
					const auto *Reference = std::get_if<WireObjectReference>(&Value->second);
					RemovalReferences[Target].push_back({Referrer, Property,
						Reference ? Reference->Object.ToObjectId() : ObjectId{}});
					runtime_detail::CountWork(runtime_detail::WorkCounter::RemovalReferencesPlanned);
				}
			}
		}
		std::size_t Remaining = MaximumTransitions;
		std::size_t LargestSelectedGroupWork = 1;
		const auto MaximumExamined = std::max<std::size_t>(64, MaximumTransitions * 4);
		std::size_t Examined = 0;
		// Reuse call-local scratch for ordinary singleton groups. Do not allocate
		// a tree node and a queue for every independent selected Part. A visited
		// set is needed only once a real dependency is discovered; its cardinality
		// and the queue are checked before expansion, not after draining a region.
		std::vector<ObjectId> Group;
		auto CollectGroup = [&](ObjectId Candidate) -> bool {
			runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyGroupsBuilt);
			Group.clear();
			Group.push_back(Candidate);
			std::set<ObjectId> Visited;
			const auto Kind = CurrentPeer.PendingTransitions.at(Candidate).Kind;
			const auto Limit = std::min(Configuration.PeerQuantum, MaximumRelevanceTransitionsPerFrame);
			auto Enqueue = [&](ObjectId Object) {
				const auto Transition = CurrentPeer.PendingTransitions.find(Object);
				if (Transition == CurrentPeer.PendingTransitions.end() || Transition->second.Kind != Kind || Object == Candidate)
					return true;
				if (Visited.contains(Object)) return true;
				if (Group.size() == Limit) return false;
				Visited.insert(Object);
				Group.push_back(Object);
				return true;
			};
			for (std::size_t Index = 0; Index < Group.size(); ++Index) {
				SaturatingAdd(CandidateMetrics.StructuralDependencyPlanOperations, 1);
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::StructuralGroupSelection);
				const auto Object = Group[Index];
				if (Kind == PendingTransitionKind::Leave) {
					auto Dependents = CurrentPeer.LeavingDependents.find(Object);
					if (Dependents != CurrentPeer.LeavingDependents.end())
						for (const auto Dependent : Dependents->second)
							if (!Enqueue(Dependent)) return false;
					// Preserve accepted hard dependencies when both endpoints leave.
					// Soft edges can be cleared; do not turn an arbitrary soft graph
					// into a new atomic dependency closure.
					if (const auto References = RemovalReferences.find(Object); References != RemovalReferences.end())
						for (const auto &Reference : References->second)
							if ((!Reference.Property->Nullable || Reference.Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard) &&
								!Enqueue(Reference.Referrer)) return false;
					continue;
				}
				auto Found = Catalog.find(Object);
				if (Found == Catalog.end() || RetiredObjects.contains(Object)) continue;
				const auto &Publication = Found->second->Publication;
				if (Publication.Parent && !CurrentPeer.View.Knows(*Publication.Parent))
					if (!Enqueue(*Publication.Parent)) return false;
				for (const auto &[Name, Value] : Publication.Properties) {
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (Reference && IsHardReference(Publication, Name) &&
						!CurrentPeer.View.Knows(Reference->Object.ToObjectId()))
						if (!Enqueue(Reference->Object.ToObjectId())) return false;
				}
			}
			std::ranges::sort(Group);
			return true;
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
				if (!CollectGroup(Entry.Object)) {
					SaturatingAdd(CandidateMetrics.DependencyLimitFailures, 1);
					Error = "Atomic replication dependency group exceeds the transition work limit";
					return false;
				}
				if (Transition->second.Kind == PendingTransitionKind::Leave) {
					const bool BlockedParentMove = std::ranges::any_of(Group, [&](ObjectId Object) {
						const auto Moves = LeaveParentFixups.find(Object);
						return Moves != LeaveParentFixups.end() && std::ranges::any_of(Moves->second, [&](ObjectId Child) {
							const auto Parent = ParentMoves.at(Child).Parent;
							return Parent && !CurrentPeer.View.Knows(*Parent) && !Entering.contains(*Parent);
						});
					});
					// The current parent must enter before removing the accepted old
					// parent. Continue ordinary selection so that dependency can progress.
					if (BlockedParentMove) continue;
					bool BlockedReference = false;
					for (const auto Object : Group) {
						const auto References = RemovalReferences.find(Object);
						if (References == RemovalReferences.end()) continue;
						for (const auto &Reference : References->second) {
							if (Leaving.contains(Reference.Referrer) || std::ranges::binary_search(Group, Reference.Referrer)) continue;
							const bool Hard = !Reference.Property->Nullable || Reference.Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard;
							if (Hard && Reference.CurrentTarget.IsValid() &&
								((!CurrentPeer.View.Knows(Reference.CurrentTarget) && !Entering.contains(Reference.CurrentTarget)) ||
								 Leaving.contains(Reference.CurrentTarget) || std::ranges::binary_search(Group, Reference.CurrentTarget)))
								BlockedReference = true;
						}
					}
					if (BlockedReference) continue;
				}
				runtime_detail::WorkScope CostWork(runtime_detail::WorkPhase::FixupGroupCosts);
				std::size_t NewWork = 0;
				for (const auto Object : Group) {
					if (Selected.contains(Object)) continue;
					++NewWork;
					const auto &FixupCosts = Transition->second.Kind == PendingTransitionKind::Enter
												 ? EnterReferenceFixupCosts
												 : LeaveReferenceFixupCosts;
					if (auto Cost = FixupCosts.find(Object); Cost != FixupCosts.end()) NewWork += Cost->second;
				}
				std::size_t DisappearingReferenceWork = 0;
				if (Transition->second.Kind == PendingTransitionKind::Leave)
					for (const auto Object : Group) {
						if (Leaving.contains(Object)) continue;
						const auto References = RemovalReferences.find(Object);
						if (References == RemovalReferences.end()) continue;
						for (const auto &Reference : References->second)
							if (!Leaving.contains(Reference.Referrer) && !std::ranges::binary_search(Group, Reference.Referrer)) {
								++NewWork;
								const auto ReferrerLeave = CurrentPeer.PendingTransitions.find(Reference.Referrer);
								if (ReferrerLeave != CurrentPeer.PendingTransitions.end() && ReferrerLeave->second.Kind == PendingTransitionKind::Leave)
									++DisappearingReferenceWork;
							}
					}
				if (NewWork > Configuration.PeerQuantum) {
					// Pending soft referrers can leave independently and eliminate
					// this cost. Let them progress rather than failing a temporary
					// oversized clear set or making soft edges a hard closure.
					if (NewWork - DisappearingReferenceWork <= Configuration.PeerQuantum) continue;
					Error = "Structural dependency group and reference fixups exceed the peer quantum";
					return false;
				}
				if (NewWork > Remaining) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyGroupsDeferred);
					continue;
				}
				LargestSelectedGroupWork = std::max(LargestSelectedGroupWork, NewWork);
				Selected.insert(Group.begin(), Group.end());
				Remaining -= NewWork;
			}
			return true;
		};
		{
		runtime_detail::WorkScope GroupWork(runtime_detail::WorkPhase::StructuralGroupSelection);
		if (!ProcessQueue(CurrentPeer.CriticalQueue, true)) return {{}, std::move(Error)};
		if (!CriticalOnly && !ProcessQueue(CurrentPeer.OrdinaryQueue, false)) return {{}, std::move(Error)};
		}
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
			runtime_detail::WorkScope FixupWork(runtime_detail::WorkPhase::StructuralClearFixups);
			for (const auto Target : Leaving) {
				const auto References = RemovalReferences.find(Target);
				if (References == RemovalReferences.end()) continue;
				for (const auto &Reference : References->second) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::ClearFixupReferrers);
					runtime_detail::CountWork(runtime_detail::WorkCounter::ClearFixupProperties);
					if (Leaving.contains(Reference.Referrer)) continue;
					WireValue Value = std::monostate{};
					if (Reference.CurrentTarget.IsValid() && !Leaving.contains(Reference.CurrentTarget) &&
						(CurrentPeer.View.Knows(Reference.CurrentTarget) || Entering.contains(Reference.CurrentTarget)))
						Value = WireObjectReference{WireObjectId::FromObjectId(Reference.CurrentTarget)};
					Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Reference.Referrer, Reference.Property->Name, Value}});
					++CandidateMetrics.SoftReferenceFixups;
					runtime_detail::CountWork(runtime_detail::WorkCounter::FixupsEmitted);
				}
			}
		}

		auto CurrentParent = [&](ObjectId Object) -> std::optional<ObjectId> {
			const auto Found = Catalog.find(Object);
			return Found == Catalog.end() ? std::nullopt : Found->second->Publication.Parent;
		};
		auto AcceptedParent = [&](ObjectId Object) -> std::optional<ObjectId> {
			const auto Found = CurrentPeer.AcceptedParents.find(Object);
			return Found != CurrentPeer.AcceptedParents.end() && Found->second.Parent.IsValid()
				? std::optional(Found->second.Parent) : std::nullopt;
		};
		const auto EnterOrder = OrderByAncestry(Entering, CurrentParent, true);
		auto OrderingScratchBytes = EnterOrder.capacity() * sizeof(AncestryOrderedObject);
		for (const auto &[Object, Depth] : EnterOrder) {
			(void)Depth;
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
			std::set<ObjectId> ParentFixups;
			for (const auto Parent : Entering)
				if (const auto Found = EnterParentFixups.find(Parent); Found != EnterParentFixups.end())
					ParentFixups.insert(Found->second.begin(), Found->second.end());
			for (const auto Parent : Leaving)
				if (const auto Found = LeaveParentFixups.find(Parent); Found != LeaveParentFixups.end())
					ParentFixups.insert(Found->second.begin(), Found->second.end());
			const auto ParentOrder = OrderByAncestry(ParentFixups, CurrentParent, true);
			OrderingScratchBytes += ParentOrder.capacity() * sizeof(AncestryOrderedObject);
			for (const auto &[Object, Depth] : ParentOrder) {
				(void)Depth;
				Frame.Operations.push_back({Frame.Epoch, ParentMoves.at(Object)});
			}
			const auto LeaveOrder = OrderByAncestry(Leaving, AcceptedParent, false);
			OrderingScratchBytes += LeaveOrder.capacity() * sizeof(AncestryOrderedObject);
			for (const auto &[Object, Depth] : LeaveOrder) {
				(void)Depth;
				if (RetiredObjects.contains(Object)) {
					Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Object}});
					++DestroyedObjects;
				} else {
					Frame.Operations.push_back({Frame.Epoch, UnpublishReplication{Object}});
					++UnpublishedObjects;
				}
			}
			{
			runtime_detail::WorkScope FixupWork(runtime_detail::WorkPhase::StructuralRestoreFixups);
			auto CatalogPosition = Catalog.cbegin();
			for (const auto Referrer : CurrentPeer.DesiredObjects) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::RestoreFixupReferrers);
				if (Entering.contains(Referrer) || !CurrentPeer.View.Knows(Referrer)) continue;
				auto Object = detail::FindPlanningObject(Catalog, CatalogPosition, Referrer);
				if (Object == Catalog.end()) continue;
				for (const auto *ReferenceProperty : Object->second.ReferenceProperties) {
					const auto &[Name, Value] = *ReferenceProperty;
					runtime_detail::CountWork(runtime_detail::WorkCounter::RestoreFixupProperties);
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					if (!Reference) continue;
					if (Entering.contains(Reference->Object.ToObjectId())) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::RestoreMatchedReferences);
						Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Referrer, Name, Value}});
						++CandidateMetrics.SoftReferenceFixups;
						runtime_detail::CountWork(runtime_detail::WorkCounter::FixupsEmitted);
					}
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
		auto Encoded = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralValidationEncode, [&] { return EncodeReplicationFrame(Frame); });
		if (Kind == ReplicationMessageKind::Baseline)
			CandidateMetrics.BaselineEncodeCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - EncodeStarted)
					.count()
			);
		if (!Encoded) return {{}, Encoded.error().Format()};
		if (Encoded->size() > MaximumFrameBytes) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::EncodeRetries);
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
				MaximumFrameBytes, AvailableFrameBytes);
		}
		if (Encoded->size() > AvailableFrameBytes) {
			SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
			SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, SelectedWorkCount);
			Metrics = CandidateMetrics;
			return {{}, {}, SelectedWorkCount, 0, {}, true, Encoded->size()};
		}
		SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, SelectedWorkCount);
		CandidateMetrics.ScratchHighWaterBytes = std::max<std::uint64_t>(
			CandidateMetrics.ScratchHighWaterBytes, Frame.Operations.capacity() * sizeof(ReplicationOperation) +
				OrderingScratchBytes + Group.capacity() * sizeof(ObjectId) + sizeof(std::array<ObjectId, MaximumDependencyClosureDepth + 1>)
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
		if (CurrentPeer.PublicationJournalEnds.size() + Entering.size() > MaximumPeerDesiredObjects)
			return {{}, "Publication journal watermark limit exceeded"};
		PreparedStructuralCommit Commit{
			.Sequence = Frame.Sequence,
			.NextSequence = *Next,
			.JournalCursor = Kind == ReplicationMessageKind::Baseline ? std::optional<ChangeCursor>(CatalogCursor)
																	  : std::nullopt,
			.PublicationJournalEnd = CatalogCursor.NextSequence,
			.Entering = std::vector<ObjectId>(Entering.begin(), Entering.end()),
			.Leaving = std::vector<ObjectId>(Leaving.begin(), Leaving.end()),
			.PublishedObjects = Entering.size(),
			.UnpublishedObjects = UnpublishedObjects,
			.DestroyedObjects = DestroyedObjects,
			.DeadlineMisses = DeadlineMisses,
			.TransitionCount = SelectedWorkCount,
			.Parents = CaptureAcceptedParents(CurrentPeer, Frame, CatalogCursor.NextSequence),
		};
		if (CurrentPeer.ExplicitSchedulerCommit)
			CurrentPeer.PreparedCommit = std::move(Commit);
		else
			ApplyPreparedCommit(CurrentPeer, std::move(Commit));
		return {std::move(Frame), {}, SelectedWorkCount, 0, std::move(*Encoded)};
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
		ProcessCatalogRetirement(SimulationTick);
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {"Replication peer is not registered"};
		if (Peer->second.Planned) return {"Continuation peers require immutable planning input"};
		std::string Error;
		if (!RefreshCatalog(Error) || !RecordDesiredState(Peer->second, Selection, SimulationTick, Error))
			return {std::move(Error)};
		return {};
	}

	ReplicationProduceResult ReplicationCoordinator::ProducePendingRelevance(
		ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes,
		std::size_t AvailableFrameBytes
	) {
		BeginRetirementTick(SimulationTick);
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		return ProduceRelevanceFrame(
			Connection, ReplicationMessageKind::Incremental, MaximumTransitions, SimulationTick, false, MaximumFrameBytes, AvailableFrameBytes
		);
	}

	ReplicationProduceResult ReplicationCoordinator::ProducePendingBaseline(
		ConnectionId Connection, std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes,
		std::size_t AvailableFrameBytes
	) {
		BeginRetirementTick(SimulationTick);
		LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		const bool CriticalOnly = Peer->second.PendingTransitions.size() > MaximumTransitions;
		return ProduceRelevanceFrame(
			Connection, ReplicationMessageKind::Baseline, MaximumTransitions, SimulationTick, CriticalOnly, MaximumFrameBytes, AvailableFrameBytes
		);
	}

	std::uint64_t ReplicationCoordinator::GetJournalLag(ConnectionId Connection) const {
		const auto Peer = Peers.find(Connection);
		runtime_detail::MaximumWork(runtime_detail::WorkCounter::JournalCatalogSequence, CatalogCursor.NextSequence);
		return Peer != Peers.end() && CatalogCursor.NextSequence >= Peer->second.JournalCursor.NextSequence
			? CatalogCursor.NextSequence - Peer->second.JournalCursor.NextSequence : 0;
	}

	ReplicationProduceResult ReplicationCoordinator::ProduceIncremental(
		ConnectionId Connection, std::size_t MaximumTransitions, std::size_t MaximumFrameBytes,
		std::size_t MaximumJournalRecords, std::size_t AvailableFrameBytes) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::IncrementalPreparation);
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return {{}, "Replication peer is not registered"};
		if (Peer->second.PreparedCommit) return {{}, "A structural frame is awaiting scheduler acceptance"};
		std::string CatalogError;
		if (!RefreshCatalog(CatalogError)) return {{}, std::move(CatalogError)};
		if (Peer->second.ExplicitSchedulerCommit &&
			(Peer->second.DesiredDependencyCursor.Scope != DependencyCursor.Scope ||
			 Peer->second.DesiredDependencyCursor.NextSequence != DependencyCursor.NextSequence))
			return {{}, "No replication changes are available"};
		auto CandidateMetrics = Metrics;
		std::set<ObjectId> CandidateRequestedTemplates;
		if ((MaximumTransitions == 0 && !Peer->second.Planned) || MaximumTransitions > MaximumReplicationOperationsPerFrame ||
			MaximumFrameBytes == 0 || MaximumFrameBytes > MaximumReplicationFrameBytes ||
			MaximumJournalRecords == 0 || MaximumJournalRecords > MaximumStructuralJournalRecordsPerCall)
			return {{}, "Replication transition work limit is invalid"};
		const bool PolicyManaged = Peer->second.PolicyManaged;
		CandidateMetrics.StructuralMaximumJournalLagRecords = std::max(
			CandidateMetrics.StructuralMaximumJournalLagRecords,
			CatalogCursor.NextSequence >= Peer->second.JournalCursor.NextSequence
				? CatalogCursor.NextSequence - Peer->second.JournalCursor.NextSequence
				: std::uint64_t{0}
		);
		// Reserve a geometric tail for byte-limit retries. Every copied record is
		// charged, including history coalesced by an accepted complete Enter.
		const auto ReadLimit = std::min(PolicyManaged ? MaximumWireJournalRecords : MaximumTransitions,
			(MaximumJournalRecords + 1) / 2);
		auto Read = ChangeJournal::Get().Read(Peer->second.JournalCursor, ReadLimit);
		auto Finish = [&](ReplicationProduceResult Result) {
			Result.JournalRecordsExamined += Read.Records.size();
			return Result;
		};
		if (Read.Status == ChangeReadStatus::ResnapshotRequired) {
			SaturatingAdd(CandidateMetrics.StructuralJournalLagFailures, 1);
			Metrics = CandidateMetrics;
			return {{}, "Authoritative journal cursor requires a new baseline"};
		}
		SaturatingAdd(CandidateMetrics.StructuralTransitionsOffered, Read.Records.size());
		if (Read.Records.empty()) return {{}, "No replication changes are available"};
		if (MaximumTransitions == 0) {
			// A saturated structural selector must not pin a prefix that cannot
			// produce any operation. This narrow path skips non-replicated property
			// records only; it stops before every lifecycle/replicated mutation.
			// It uses the same shared journal read budget and never prepares a frame.
			for (const auto &Record : Read.Records) {
				const auto *Property = std::get_if<PropertyUpdatedChange>(&Record.Payload);
				if (!Property || Property->Replicated) break;
				Peer->second.JournalCursor.NextSequence = Record.Sequence + 1;
			}
			CandidateMetrics.ReplicationBacklog = Peer->second.JournalCursor.NextSequence < CatalogCursor.NextSequence ? 1 : 0;
			Metrics = CandidateMetrics;
			return Finish({{}, "No relevant replication changes are available"});
		}
		// Property updates and coalesced history only read accepted membership.
		// Copy it lazily if a legacy journal create/destroy actually changes it;
		// budgeted no-op slices must not copy the entire Known set each tick.
		std::optional<ReplicationView> CandidateView;
		auto ReadView = [&]() -> const ReplicationView & {
			return CandidateView ? *CandidateView : Peer->second.View;
		};
		auto WriteView = [&]() -> ReplicationView & {
			if (!CandidateView) runtime_detail::MeasureWork(runtime_detail::WorkPhase::PeerViewCopy,
				[&] {
					runtime_detail::CountWork(runtime_detail::WorkCounter::PeerViewCopiedIdentities,
						Peer->second.View.KnownObjects.size() + Peer->second.View.RelevantObjects.size());
					CandidateView.emplace(Peer->second.View);
				});
			return *CandidateView;
		};
		auto CandidateNextSequence = Peer->second.NextSequence;
		ReplicationFrame Frame{
			ReplicationProtocolVersion, ReplicationMessageKind::Incremental, Peer->second.View.Epoch, CandidateNextSequence
		};
		Frame.Operations.reserve(std::min(MaximumTransitions, Read.Records.size()));
		auto ProcessedCursor = Peer->second.JournalCursor;
		std::set<ObjectId> PublishedThisFrame;
		std::set<ObjectId> DestroyedThisFrame;
		std::set<ObjectId> BatchPublishObjects;
		for (const auto &Record : Read.Records)
			if (std::holds_alternative<ObjectCreatedChange>(Record.Payload) && Catalog.contains(Record.Object) &&
				(!PolicyManaged || ReadView().RelevantObjects.contains(Record.Object)))
				BatchPublishObjects.insert(Record.Object);
		for (const auto &Record : Read.Records) {
			if (Frame.Operations.size() == MaximumTransitions) break;
			ProcessedCursor.NextSequence = Record.Sequence + 1;
			const auto Published = Peer->second.PublicationJournalEnds.find(Record.Object);
			if (Published != Peer->second.PublicationJournalEnds.end() && Record.Sequence < Published->second) {
				// Complete Enter already represented this object's pre-publication
				// history. Replaying it wastes work and can roll Attributes/tags back.
				// Do not advance unrelated objects or swallow post-prepare changes.
				++CandidateMetrics.OperationsCoalesced;
				continue;
			}
			const auto Known = ReadView().Knows(Record.Object);
			const auto Relevant = ReadView().RelevantObjects.contains(Record.Object);
			if (PolicyManaged && !Relevant) continue;
			if (!Known && !PolicyManaged && IsInitiallyRelevant && !IsInitiallyRelevant(Record.Object)) continue;
			if (std::holds_alternative<ObjectReparentedChange>(Record.Payload)) {
				const auto Parent = Peer->second.AcceptedParents.find(Record.Object);
				if (Parent != Peer->second.AcceptedParents.end() && Record.Sequence < Parent->second.JournalEnd) {
					++CandidateMetrics.OperationsCoalesced;
					continue;
				}
			}
			bool FailedReference = false;
			std::visit(
				[&](const auto &Change) {
					using Type = std::decay_t<decltype(Change)>;
					if constexpr (std::is_same_v<Type, ObjectCreatedChange>) {
						if (!Relevant) WriteView().RelevantObjects.insert(Record.Object);
						if (Known) return;
						auto Found = Catalog.find(Record.Object);
						if (Found == Catalog.end()) return;
						std::set<ObjectId> Available(
							ReadView().KnownObjects.begin(), ReadView().KnownObjects.end()
						);
						Available.insert(BatchPublishObjects.begin(), BatchPublishObjects.end());
						auto Publish = MakePeerPublish(
							Record.Object, Available, CandidateMetrics, CandidateRequestedTemplates, PolicyManaged
						);
						const auto &Publication = Publish.Template->Publication;
						if (Publication.Parent && !ReadView().Knows(*Publication.Parent) &&
							!BatchPublishObjects.contains(*Publication.Parent)) {
							WriteView().RelevantObjects.erase(Record.Object);
							return;
						}
						if (!PublishReferencesKnown(ReadView(), Publish, BatchPublishObjects)) {
							FailedReference = true;
							return;
						}
						Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
						WriteView().KnownObjects.insert(Record.Object);
						PublishedThisFrame.insert(Record.Object);
					} else if constexpr (std::is_same_v<Type, ObjectDestroyedChange>) {
						if (!Known) return;
						Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Record.Object}});
						WriteView().ForgetReplica(Record.Object);
						WriteView().RelevantObjects.erase(Record.Object);
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
							if (!ReferencesKnown(ReadView(), Value)) {
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
							if (Change.Value && !ReferencesKnown(ReadView(), *Change.Value)) {
								FailedReference = true;
								return;
							}
							Frame.Operations.push_back(
								{Frame.Epoch,
								 AttributeReplicationUpdate{Record.Object, Change.AttributeName, Change.Value}}
							);
						} else if constexpr (std::is_same_v<Type, ExtensionPropertyUpdatedChange>) {
							if (!ReferencesKnown(ReadView(), Change.Value)) {
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
							if (Change.Parent && !ReadView().Knows(*Change.Parent)) {
								if (PolicyManaged) {
									// Parent is a hard structural dependency. Its accepted Enter
									// emits the current-parent fixup under the same 3J work cap.
									++CandidateMetrics.OperationsCoalesced;
									return;
								}
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
				return Finish({{}, "Replication operation references an object not materialized for this peer"});
			}
		}
		if (Frame.Operations.empty()) {
			if (CandidateView) Peer->second.View = std::move(*CandidateView);
			Peer->second.JournalCursor = ProcessedCursor;
			CandidateMetrics.ReplicationBacklog = ProcessedCursor.NextSequence < CatalogCursor.NextSequence ? 1 : 0;
			Metrics = CandidateMetrics;
			return Finish({{}, "No relevant replication changes are available"});
		}
		if (Frame.Operations.size() > MaximumReplicationOperationsPerFrame)
			return Finish({{}, "Replication frame operation limit exceeded"});
		auto Encoded = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralValidationEncode, [&] { return EncodeReplicationFrame(Frame); });
		if (!Encoded) return Finish({{}, Encoded.error().Format()});
		if (Encoded->size() > MaximumFrameBytes) {
			runtime_detail::CountWork(runtime_detail::WorkCounter::EncodeRetries);
			if (MaximumTransitions == 1)
				return Finish({{}, "Structural operation exceeds the negotiated reliable message limit"});
			// A one-record read cannot be made smaller; its operation is atomic.
			if (Read.Records.size() == 1)
				return Finish({{}, "Structural operation exceeds the negotiated reliable message limit"});
			return Finish(ProduceIncremental(Connection, MaximumTransitions / 2, MaximumFrameBytes,
				MaximumJournalRecords - Read.Records.size(), AvailableFrameBytes));
		}
		if (Encoded->size() > AvailableFrameBytes) {
			SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, Frame.Operations.size());
			SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, Frame.Operations.size());
			SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
			Metrics = CandidateMetrics;
			return Finish({{}, {}, Frame.Operations.size(), 0, {}, true, Encoded->size()});
		}
		SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, Frame.Operations.size());
		CandidateMetrics.ScratchHighWaterBytes = std::max<std::uint64_t>(
			CandidateMetrics.ScratchHighWaterBytes, Frame.Operations.capacity() * sizeof(ReplicationOperation)
		);
		auto Next = CandidateNextSequence.TryNext();
		if (!Next) return Finish({{}, "Reliable replication sequence is exhausted"});
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
		auto AcceptedParents = CaptureAcceptedParents(Peer->second, Frame, ProcessedCursor.NextSequence);
		if (Peer->second.ExplicitSchedulerCommit) {
			Peer->second.PreparedCommit = PreparedStructuralCommit{
				.Sequence = Frame.Sequence,
				.NextSequence = CandidateNextSequence,
				.JournalCursor = ProcessedCursor,
				.PublicationJournalEnd = CatalogCursor.NextSequence,
				.Entering = std::vector<ObjectId>(PublishedThisFrame.begin(), PublishedThisFrame.end()),
				.Leaving = std::vector<ObjectId>(DestroyedThisFrame.begin(), DestroyedThisFrame.end()),
				.PublishedObjects = PublishedThisFrame.size(),
				.DestroyedObjects = DestroyedThisFrame.size(),
				.TransitionCount = OperationCount,
				.Parents = std::move(AcceptedParents),
			};
		} else {
			if (CandidateView) Peer->second.View = std::move(*CandidateView);
			Peer->second.NextSequence = CandidateNextSequence;
			Peer->second.JournalCursor = ProcessedCursor;
			for (const auto Object : DestroyedThisFrame) {
				Peer->second.PublicationJournalEnds.erase(Object);
				if (const auto Accepted = Peer->second.AcceptedParents.find(Object); Accepted != Peer->second.AcceptedParents.end())
					Peer->second.AcceptedReferenceBytes -= Accepted->second.References.capacity() * sizeof(decltype(Accepted->second.References)::value_type);
				Peer->second.AcceptedParents.erase(Object);
			}
			ApplyAcceptedParents(Peer->second, std::move(AcceptedParents));
		}
		return Finish({std::move(Frame), {}, OperationCount, 0, std::move(*Encoded)});
	}

	ReplicationProduceResult
	ReplicationCoordinator::SetRelevant(ConnectionId Connection, ObjectId Object, bool Relevant) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end() || !Object.IsValid()) return {{}, "Replication peer or object is invalid"};
		if (Peer->second.Planned) return {{}, "Continuation peers require scheduler-accepted structural work"};
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
			if (CandidatePeer.PublicationJournalEnds.size() >= MaximumPeerDesiredObjects)
				return {{}, "Publication journal watermark limit exceeded"};
			auto Publish = MakePeerPublish(Object, {Object}, CandidateMetrics, CandidateRequestedTemplates, false);
			const auto &Publication = Publish.Template->Publication;
			if (Publication.Parent && !CandidatePeer.View.Knows(*Publication.Parent))
				return {{}, "Publish parent is not materialized for this peer"};
			if (!PublishReferencesKnown(CandidatePeer.View, Publish, {Object}))
				return {{}, "Publish references an object not materialized for this peer"};
			Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
			CandidatePeer.View.RelevantObjects.insert(Object);
			CandidatePeer.View.KnownObjects.insert(Object);
			if (CatalogCursor.NextSequence > CandidatePeer.JournalCursor.NextSequence)
				CandidatePeer.PublicationJournalEnds[Object] = CatalogCursor.NextSequence;
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
				CandidatePeer.PublicationJournalEnds.erase(*Iterator);
				if (const auto Accepted = CandidatePeer.AcceptedParents.find(*Iterator); Accepted != CandidatePeer.AcceptedParents.end())
					CandidatePeer.AcceptedReferenceBytes -= Accepted->second.References.capacity() * sizeof(decltype(Accepted->second.References)::value_type);
				CandidatePeer.AcceptedParents.erase(*Iterator);
				CandidatePeer.View.RelevantObjects.erase(*Iterator);
			}
			CandidateMetrics.ObjectsUnpublished += Removed.size();
		}
		SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, Frame.Operations.size());
		SaturatingAdd(CandidateMetrics.StructuralTransitionsPrepared, Frame.Operations.size());
		auto Encoded = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralValidationEncode, [&] { return EncodeReplicationFrame(Frame); });
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
		ApplyAcceptedParents(CandidatePeer, CaptureAcceptedParents(CandidatePeer, Frame, CatalogCursor.NextSequence));
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
		if (Peer->second.Planning) {
			auto Plan = Peer->second.Planning;
			// Cancel the coroutine that borrows Peer before erasing the peer node.
			// Disposal owns only its detached scratch and consumes the shared budget.
			Plan->Work = {};
			Plan->Ready = false; Plan->Disposing = true; Plan->Work = Plan->Dispose();
			DetachedPlanning.emplace(Connection, std::move(Plan));
			PlanningPeers.insert(Connection);
		} else PlanningPeers.erase(Connection);
		PlanningRecordCount -= Peer->second.PlanningInputRecords;
		Metrics.PlanningRecords = PlanningRecordCount;
		Peers.erase(Peer);
		return true;
	}

	const ReplicationView *ReplicationCoordinator::GetView(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer == Peers.end() ? nullptr : &Peer->second.View;
	}

	bool ReplicationCoordinator::HasPendingRelevance(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		return Peer != Peers.end() && (!Peer->second.PendingTransitions.empty() ||
			(Peer->second.Planned && (Peer->second.Planning || !Peer->second.PlanningError.empty() || PlanningPeers.contains(Connection))));
	}

	std::size_t ReplicationCoordinator::GetPendingCriticalTransitionCount(ConnectionId Connection) const {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end()) return 0;
		// Runtime bootstrap asks this only for a complete plan. Its count was
		// accumulated during charged discovery, not by rescanning Pending here.
		if (Peer->second.Planned)
			return IsPlanningReady(Connection) ? Peer->second.Planning->CriticalTransitions : 0;
		return static_cast<std::size_t>(std::ranges::count_if(Peer->second.PendingTransitions, [](const auto &Entry) {
			return Entry.second.Critical;
		}));
	}

	bool ReplicationCoordinator::HasPendingStructuralWork() const {
		if (!PlanningPeers.empty()) return true;
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (!Peer.PendingTransitions.empty()) return true;
		}
		return false;
	}

	ReplicationScheduleResult
	ReplicationCoordinator::CommitSchedulerAcceptance(ConnectionId Connection, ReliableReplicationSequence Sequence) {
		runtime_detail::WorkScope AcceptanceWork(runtime_detail::WorkPhase::StructuralAcceptance);
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

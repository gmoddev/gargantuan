#include "gargantuan/network/ReplicationRelevance.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace gargantuan::network {
	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter, Value Amount = 1) {
			Counter = Amount > std::numeric_limits<Value>::max() - Counter ? std::numeric_limits<Value>::max()
																		   : Counter + Amount;
		}

		bool Finite(glm::vec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool Equal(glm::dvec3 Left, glm::dvec3 Right) {
			return Left.x == Right.x && Left.y == Right.y && Left.z == Right.z;
		}

		SpatialBounds PartBounds(const BasePart &Part) {
			const auto Frame = Part.GetCFrame();
			const glm::dvec3 Half = glm::abs(glm::dvec3(Part.GetSize())) * 0.5;
			const glm::dmat3 Rotation(Frame.Rotation);
			const glm::dvec3 Extents = glm::abs(Rotation[0]) * Half.x + glm::abs(Rotation[1]) * Half.y +
									   glm::abs(Rotation[2]) * Half.z;
			const glm::dvec3 Position(Frame.Position);
			return {Position - Extents, Position + Extents};
		}
	}

	bool ReplicationRelevanceConfiguration::IsValid() const {
		return std::isfinite(EnterRadius) && EnterRadius > 0.0f && std::isfinite(LeaveRadius) &&
			   LeaveRadius >= EnterRadius && UpdateIntervalTicks != 0 && MaximumPeerEvaluationsPerTick >= 2 &&
			   MaximumPeerEvaluationsPerTick <= MaximumReplicationRelevancePeers && MaximumSpatialObjects != 0 &&
			   MaximumSpatialObjects <= MaximumReplicationSpatialObjects && MaximumSpatialRegions != 0 &&
			   MaximumSpatialRegions <= MaximumReplicationSpatialRegions && MaximumSpatialMemberships != 0 &&
			   MaximumSpatialMemberships <= MaximumReplicationSpatialMemberships &&
			   MaximumRegionsPerSpatialObject != 0 &&
			   MaximumRegionsPerSpatialObject <= MaximumReplicationRegionsPerSpatialObject &&
			   MaximumLargeSpatialObjects != 0 && MaximumLargeSpatialObjects <= MaximumReplicationLargeSpatialObjects &&
			   MaximumDesiredObjectsPerPeer != 0 && MaximumDesiredObjectsPerPeer <= MaximumPeerDesiredObjects &&
			   MaximumQueryRegions != 0 && MaximumQueryRegions <= MaximumReplicationQueryRegions &&
			   MaximumQueryMembershipVisits != 0 &&
			   MaximumQueryMembershipVisits <= MaximumReplicationQueryMembershipVisits &&
			   SpatialConfiguration().IsValid();
	}

	SpatialRegionIndexConfiguration ReplicationRelevanceConfiguration::SpatialConfiguration() const {
		return {
			.RegionSize = RegionSize,
			.MaximumObjects = MaximumSpatialObjects,
			.MaximumRegions = MaximumSpatialRegions,
			.MaximumMemberships = MaximumSpatialMemberships,
			.MaximumRegionsPerObject = MaximumRegionsPerSpatialObject,
			.MaximumLargeObjects = MaximumLargeSpatialObjects,
			.MaximumQueryVolumes = MaximumReplicationFocusPoints,
			.MaximumQueryRegions = MaximumQueryRegions,
			.MaximumQueryCandidates = MaximumDesiredObjectsPerPeer,
			.MaximumQueryMembershipVisits = MaximumQueryMembershipVisits,
		};
	}

	struct ReplicationRelevance::Implementation {
		struct ObjectLocation {
			std::optional<ObjectId> SpatialRoot;
		};

		struct SpatialEntry {
			std::weak_ptr<Instance> Object;
			bool CharacterRoot = false;
			std::set<ObjectId> Members;
			std::vector<SignalConnection::Pointer> SpatialChanged;
		};

		struct PeerState {
			ObjectId LocalPlayer;
			ObjectId OwnerCharacter;
			std::vector<glm::vec3> TrustedFocus;
			std::vector<glm::vec3> ResolvedFocus;
			std::set<ObjectId> RelevantSpatialRoots;
			std::vector<ObjectId> RuntimeCharacterCandidates;
			std::shared_ptr<const PeerRelevanceSelection> Selection = std::make_shared<const PeerRelevanceSelection>();
			std::uint64_t LastUpdateTick = 0;
			std::uint64_t WorldRevision = 0;
			std::uint64_t EvaluationPass = 0;
			std::uint64_t PendingSinceTick = 0;
			bool Dirty = true;
			bool CriticalDirty = false;
		};

		std::shared_ptr<Instance> SourceRoot;
		ExclusionPolicy IsExcluded;
		ReplicationRelevanceConfiguration Configuration;
		std::map<ObjectId, ObjectLocation> ObjectLocations;
		std::set<ObjectId> GlobalObjects;
		std::map<ObjectId, SpatialEntry> SpatialObjects;
		SpatialRuntimeProjectionStore Spatial;
		SpatialRegionQueryScratch QueryScratch;
		std::map<ConnectionId, PeerState> Peers;
		SignalConnection::Pointer DescendantAdded;
		SignalConnection::Pointer DescendantRemoved;
		ReplicationRelevanceMetrics Metrics;
		// No retained object result or peer pointer: cursors are full connection identities.
		ConnectionId OrdinaryCursor;
		ConnectionId CriticalCursor;
		std::uint64_t WorldRevision = 1;
		std::uint64_t EvaluationPass = 1;
		std::uint64_t NextWorkTick = 0;
		std::uint64_t LastSimulationTick = 0;
		std::size_t EvaluationsThisTick = 0;
		bool WorldDirty = false;
		bool Healthy = true;
		std::string Failure;

		Implementation(
			std::shared_ptr<Instance> SourceRootValue,
			ExclusionPolicy IsExcludedValue,
			ReplicationRelevanceConfiguration ConfigurationValue
		)
			: SourceRoot(std::move(SourceRootValue)), IsExcluded(std::move(IsExcludedValue)),
			  Configuration(ConfigurationValue),
			  Spatial({.Index = ConfigurationValue.SpatialConfiguration(), .MaximumSpaces = 1}) {
			if (!SourceRoot || !std::dynamic_pointer_cast<DataModel>(SourceRoot) || !Configuration.IsValid())
				throw std::invalid_argument("[Replication:Relevance] configuration or source root is invalid");
			QueryScratch.Reserve(Configuration.SpatialConfiguration());
			RegisterObject(SourceRoot);
			for (const auto &Object : SourceRoot->GetDescendants())
				RegisterObject(Object);
			DescendantRemoved = SourceRoot->DescendantRemoved->Connect([this](std::shared_ptr<Instance> Object) {
				UnregisterObject(Object ? Object->GetObjectId() : ObjectId{});
			});
			DescendantAdded = SourceRoot->DescendantAdded->Connect([this](std::shared_ptr<Instance> Object) {
				RegisterObject(std::move(Object));
			});
		}

		~Implementation() {
			if (DescendantAdded) DescendantAdded->Disconnect();
			if (DescendantRemoved) DescendantRemoved->Disconnect();
			for (auto &[Object, Entry] : SpatialObjects) {
				(void)Object;
				for (auto &Connection : Entry.SpatialChanged)
					if (Connection) Connection->Disconnect();
			}
		}

		void Fail(std::string Message) {
			if (!Healthy) return;
			Healthy = false;
			Failure = std::move(Message);
			SaturatingIncrement(Metrics.LimitFailures);
		}

		void FailSpatial(std::string_view Operation, SpatialRegionStatus Status) {
			Fail(std::string(Operation) + " failed: " + SpatialRegionStatusName(Status));
		}

		void FailProjection(std::string_view Operation, SpatialRuntimeProjectionStatus Status) {
			auto Message = std::string(Operation) + " failed: " + SpatialRuntimeProjectionStatusName(Status);
			if (Status == SpatialRuntimeProjectionStatus::IndexFailure)
				Message += std::string(" (") + SpatialRegionStatusName(Spatial.GetLastIndexFailure()) + ')';
			Fail(std::move(Message));
		}

		void MarkSpatialDirty(ObjectId Object) {
			if (!SpatialObjects.contains(Object)) return;
			const auto Status = Spatial.MarkDirty(Object);
			if (Status != SpatialRuntimeProjectionStatus::Success)
				FailProjection("Spatial projection dirty mark", Status);
		}

		std::optional<ObjectId> FindSpatialRoot(const std::shared_ptr<Instance> &Object) const {
			if (!Object || std::dynamic_pointer_cast<RemoteBase>(Object)) return std::nullopt;
			std::optional<ObjectId> PartRoot;
			for (auto Current = Object; Current;) {
				if (std::dynamic_pointer_cast<Character>(Current)) return Current->GetObjectId();
				if (!PartRoot && std::dynamic_pointer_cast<BasePart>(Current)) PartRoot = Current->GetObjectId();
				auto Parent = Current->GetParent();
				Current = Parent ? *Parent : nullptr;
			}
			return PartRoot;
		}

		std::optional<CFrame> PoseOf(const std::shared_ptr<Instance> &Object) const {
			if (auto CharacterValue = std::dynamic_pointer_cast<Character>(Object)) return CharacterValue->GetCFrame();
			if (auto PartValue = std::dynamic_pointer_cast<BasePart>(Object)) return PartValue->GetCFrame();
			return std::nullopt;
		}

		std::optional<SpatialBounds> BoundsOf(const std::shared_ptr<Instance> &Object) const {
			if (auto CharacterValue = std::dynamic_pointer_cast<Character>(Object))
				return SpatialBounds::Point(glm::dvec3(CharacterValue->GetPosition()));
			if (auto PartValue = std::dynamic_pointer_cast<BasePart>(Object)) return PartBounds(*PartValue);
			return std::nullopt;
		}

		bool AddSpatialRoot(const std::shared_ptr<Instance> &Object) {
			const auto Id = Object->GetObjectId();
			if (SpatialObjects.contains(Id)) return true;
			auto Pose = PoseOf(Object);
			auto Bounds = BoundsOf(Object);
			if (!Pose || !Finite(Pose->Position) || !Bounds) {
				Fail("Spatial object has invalid authoritative bounds");
				return false;
			}
			SpatialEntry Entry{.Object = Object, .CharacterRoot = static_cast<bool>(std::dynamic_pointer_cast<Character>(Object))};
			Entry.Members.insert(Id);
			const auto Status = Spatial.Register(Id, {Spatial.GetDefaultSpace(), *Pose}, *Bounds);
			if (Status != SpatialRuntimeProjectionStatus::Success) {
				FailProjection("Spatial root projection registration", Status);
				return false;
			}
			decltype(SpatialObjects)::iterator Found;
			bool Added = false;
			try {
				std::tie(Found, Added) = SpatialObjects.emplace(Id, std::move(Entry));
			} catch (...) {
				(void)Spatial.Remove(Id);
				throw;
			}
			if (!Added) {
				(void)Spatial.Remove(Id);
				return true;
			}
			try {
				Found->second.SpatialChanged.push_back(Object->GetPropertyChangedSignal("CFrame")->Connect(
					[this, Id](std::monostate) { MarkSpatialDirty(Id); }
				));
				if (std::dynamic_pointer_cast<BasePart>(Object))
					Found->second.SpatialChanged.push_back(Object->GetPropertyChangedSignal("Size")->Connect(
						[this, Id](std::monostate) { MarkSpatialDirty(Id); }
					));
			} catch (...) {
				for (auto &Connection : Found->second.SpatialChanged)
					if (Connection) Connection->Disconnect();
				SpatialObjects.erase(Found);
				(void)Spatial.Remove(Id);
				throw;
			}
			Metrics.SpatialEntries = SpatialObjects.size();
			return true;
		}

		void RegisterObject(std::shared_ptr<Instance> Object) {
			if (!Healthy || !Object || Object->GetDestroyed() || Object->IsDestroying()) return;
			const auto Id = Object->GetObjectId();
			if (!Id.IsValid() || ObjectLocations.contains(Id) || (IsExcluded && IsExcluded(Id))) return;
			auto SpatialRoot = FindSpatialRoot(Object);
			if (SpatialRoot) {
				if (*SpatialRoot == Id && !AddSpatialRoot(Object)) return;
				auto Root = SpatialObjects.find(*SpatialRoot);
				if (Root == SpatialObjects.end()) {
					auto RootObject = ObjectRegistry::Get().Lookup(*SpatialRoot);
					if (!RootObject || !AddSpatialRoot(RootObject)) return;
					Root = SpatialObjects.find(*SpatialRoot);
				}
				Root->second.Members.insert(Id);
				ObjectLocations.emplace(Id, ObjectLocation{*SpatialRoot});
			} else {
				GlobalObjects.insert(Id);
				ObjectLocations.emplace(Id, ObjectLocation{});
			}
			WorldDirty = true;
			NextWorkTick = 0;
		}

		void UnregisterObject(ObjectId Object) {
			if (!Object.IsValid()) return;
			auto Location = ObjectLocations.find(Object);
			if (Location == ObjectLocations.end()) return;
			if (Location->second.SpatialRoot) {
				auto Root = SpatialObjects.find(*Location->second.SpatialRoot);
				if (Root != SpatialObjects.end()) Root->second.Members.erase(Object);
				if (*Location->second.SpatialRoot == Object && Root != SpatialObjects.end()) {
					for (auto &Connection : Root->second.SpatialChanged)
						if (Connection) Connection->Disconnect();
					const auto Status = Spatial.Remove(Object);
					if (Status != SpatialRuntimeProjectionStatus::Success &&
						Status != SpatialRuntimeProjectionStatus::MissingProjection)
						FailProjection("Spatial root projection removal", Status);
					SpatialObjects.erase(Root);
					SaturatingIncrement(Metrics.SpatialRemovals);
				}
			} else {
				GlobalObjects.erase(Object);
			}
			ObjectLocations.erase(Location);
			Metrics.SpatialEntries = SpatialObjects.size();
			WorldDirty = true;
			NextWorkTick = 0;
		}

		void UpdateSpatialPosition(ObjectId Object) {
			auto Found = SpatialObjects.find(Object);
			if (Found == SpatialObjects.end()) return;
			auto InstanceValue = Found->second.Object.lock();
			auto Pose = PoseOf(InstanceValue);
			auto Bounds = BoundsOf(InstanceValue);
			if (!InstanceValue || !Pose || !Finite(Pose->Position) || !Bounds) {
				Fail("Spatial root lost valid authoritative bounds");
				return;
			}
			const auto Status = Spatial.Update(Object, {Spatial.GetDefaultSpace(), *Pose}, *Bounds);
			if (Status != SpatialRuntimeProjectionStatus::Success)
				FailProjection("Spatial root projection update", Status);
		}

		void RefreshDirtySpatialPositions() {
			for (const auto Object : Spatial.GetDirtyObjects()) {
				UpdateSpatialPosition(Object);
				if (!Healthy) return;
			}
			Spatial.ClearDirtyObjects();
		}

		bool WithinAnyFocus(glm::vec3 Position, std::span<const glm::vec3> Focus, float Radius) const {
			const auto RadiusSquared = static_cast<double>(Radius) * Radius;
			for (const auto Point : Focus) {
				const auto Difference = glm::dvec3(Position) - glm::dvec3(Point);
				if (glm::dot(Difference, Difference) <= RadiusSquared) return true;
			}
			return false;
		}

		bool Query(std::span<const glm::vec3> Focus) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RelevanceQuery);
			std::array<SpatialRegionQueryVolume, MaximumReplicationFocusPoints> Volumes;
			for (std::size_t Index = 0; Index < Focus.size(); ++Index)
				Volumes[Index] = {
					.Space = Spatial.GetDefaultSpace(),
					.Center = glm::dvec3(Focus[Index]),
					.Radius = Configuration.LeaveRadius,
				};
			const auto Status = Spatial.Query(std::span(Volumes).first(Focus.size()), QueryScratch);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RelevanceQuery, QueryScratch.Candidates.size());
			if (Status == SpatialRegionStatus::Success) return true;
			FailSpatial("Replication region candidate query", Status);
			return false;
		}

		std::vector<glm::vec3> ResolveFocus(const PeerState &Peer) const {
			if (!Peer.TrustedFocus.empty()) return Peer.TrustedFocus;
			auto Owner = SpatialObjects.find(Peer.OwnerCharacter);
			const auto *Projection = Owner == SpatialObjects.end() ? nullptr : Spatial.Get(Peer.OwnerCharacter);
			return Projection ? std::vector<glm::vec3>{Projection->Pose.LocalTransform.Position}
							  : std::vector<glm::vec3>{};
		}

		bool BuildSelection(PeerState &Peer) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RelevanceSelection);
			std::set<ObjectId> Required;
			std::set<ObjectId> Desired = GlobalObjects;
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RelevanceSelection, GlobalObjects.size());
			if (Peer.LocalPlayer.IsValid()) Required.insert(Peer.LocalPlayer);
			Peer.RuntimeCharacterCandidates.clear();
			for (const auto RootId : Peer.RelevantSpatialRoots) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceRoots);
				auto Root = SpatialObjects.find(RootId);
				if (Root == SpatialObjects.end()) continue;
				if (Root->second.CharacterRoot) Peer.RuntimeCharacterCandidates.push_back(RootId);
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RelevanceSelection, Root->second.Members.size());
				Desired.insert(Root->second.Members.begin(), Root->second.Members.end());
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceMembers, Root->second.Members.size());
				if (RootId == Peer.OwnerCharacter)
					Required.insert(Root->second.Members.begin(), Root->second.Members.end());
			}
			Desired.insert(Required.begin(), Required.end());
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::RelevanceSelection, Required.size());
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::RelevanceSelection, true, Desired.size());
			if (Desired.size() > Configuration.MaximumDesiredObjectsPerPeer ||
				Required.size() > Configuration.MaximumDesiredObjectsPerPeer) {
				Fail("Peer relevance selection exceeds its object limit");
				return false;
			}
			if (!std::ranges::equal(Required, Peer.Selection->RequiredObjects) ||
				!std::ranges::equal(Desired, Peer.Selection->DesiredObjects)) {
				runtime_detail::WorkScope CommitWork(runtime_detail::WorkPhase::RelevanceCommit);
				PeerRelevanceSelection Selection;
				Selection.RequiredObjects.assign(Required.begin(), Required.end());
				Selection.DesiredObjects.assign(Desired.begin(), Desired.end());
				Peer.Selection = std::make_shared<const PeerRelevanceSelection>(std::move(Selection));
			}
			return true;
		}

		void RefreshDesiredObjectGauge() {
			Metrics.DesiredObjects = 0;
			for (const auto &[Connection, Peer] : Peers) {
				(void)Connection;
				SaturatingIncrement(
					Metrics.DesiredObjects, static_cast<std::uint64_t>(Peer.Selection->DesiredObjects.size())
				);
			}
		}

		bool NeedsEvaluation(const PeerState &Peer, std::uint64_t SimulationTick) const {
			return Peer.Dirty || Peer.WorldRevision != WorldRevision || Peer.LastUpdateTick == 0 ||
				SimulationTick < Peer.LastUpdateTick ||
				SimulationTick - Peer.LastUpdateTick >= Configuration.UpdateIntervalTicks;
		}

		bool UpdatePeer(PeerState &Peer, std::uint64_t SimulationTick) {
			runtime_detail::WorkScope PeerWork(runtime_detail::WorkPhase::RelevancePeer);
			runtime_detail::CountWork(runtime_detail::WorkCounter::RelevancePeers);
			auto Focus = ResolveFocus(Peer);
			if (Focus.empty())
				QueryScratch.Clear();
			else if (!Query(Focus))
				return false;
			auto FinishEvaluation = [&] {
				runtime_detail::WorkScope CommitWork(runtime_detail::WorkPhase::RelevanceCommit);
				Peer.ResolvedFocus = std::move(Focus);
				Peer.LastUpdateTick = SimulationTick;
				Peer.WorldRevision = WorldRevision;
				Peer.EvaluationPass = EvaluationPass;
				Peer.PendingSinceTick = 0;
				Peer.Dirty = false;
				Peer.CriticalDirty = false;
			};
			// Check the same enter/leave predicates without constructing another set
			// when the result is unchanged. Current projection/focus still determine
			// relevance; the cached selection is reusable only at the same membership
			// revision and with unchanged required owner lifecycle.
			std::optional<runtime_detail::WorkScope> HysteresisWork;
			HysteresisWork.emplace(runtime_detail::WorkPhase::RelevanceHysteresis);
			// Query candidates and retained roots are ordered by full ObjectId.
			// Join them monotonically instead of searching the root tree per candidate.
			// This cursor is call-local; no iterator survives semantic mutation.
			auto KnownRoot = Peer.RelevantSpatialRoots.begin();
			const bool SameRoots =
				(!Peer.OwnerCharacter.IsValid() || !SpatialObjects.contains(Peer.OwnerCharacter) ||
					Peer.RelevantSpatialRoots.contains(Peer.OwnerCharacter)) &&
				std::ranges::all_of(Peer.RelevantSpatialRoots, [&](ObjectId Existing) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceOldRootVisits);
					// A healthy projection store and SpatialObjects have the same root
					// identities (transactional AddSpatialRoot / UnregisterObject).
					const auto *Projection = Spatial.Get(Existing);
					return Projection && (Existing == Peer.OwnerCharacter ||
						WithinAnyFocus(Projection->Pose.LocalTransform.Position, Focus, Configuration.LeaveRadius));
				}) && [&] {
					for (const auto Candidate : QueryScratch.Candidates) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceCandidateVisits);
						while (KnownRoot != Peer.RelevantSpatialRoots.end() && *KnownRoot < Candidate) ++KnownRoot;
						if (KnownRoot != Peer.RelevantSpatialRoots.end() && *KnownRoot == Candidate) continue;
						const auto *Projection = Spatial.Get(Candidate);
						if (Projection && WithinAnyFocus(Projection->Pose.LocalTransform.Position, Focus, Configuration.EnterRadius)) return false;
					}
					return true;
				}();
			if (SameRoots) {
				HysteresisWork.reset();
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceUnchangedRoots);
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceNoopQueryCandidates, QueryScratch.Candidates.size());
				const bool RebuildSelection = Peer.WorldRevision != WorldRevision || Peer.CriticalDirty || Peer.LastUpdateTick == 0;
				SaturatingIncrement(Metrics.RelevanceEvaluations, static_cast<std::uint64_t>(QueryScratch.Candidates.size()));
				if (RebuildSelection && !BuildSelection(Peer)) return false;
				if (!RebuildSelection) {
					SaturatingIncrement(Metrics.SelectionCacheHits);
					runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceUnchangedSelection);
				}
				FinishEvaluation();
				return true;
			}
			std::set<ObjectId> Relevant;
			for (const auto Existing : Peer.RelevantSpatialRoots) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceOldRootVisits);
				const auto *Projection = Spatial.Get(Existing);
				if (Projection &&
					(Existing == Peer.OwnerCharacter ||
					 WithinAnyFocus(Projection->Pose.LocalTransform.Position, Focus, Configuration.LeaveRadius)))
					Relevant.insert(Existing);
			}
			for (const auto Candidate : QueryScratch.Candidates) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::RelevanceCandidateVisits);
				const auto *Projection = Spatial.Get(Candidate);
				if (Projection &&
					WithinAnyFocus(Projection->Pose.LocalTransform.Position, Focus, Configuration.EnterRadius))
					Relevant.insert(Candidate);
			}
			if (Peer.OwnerCharacter.IsValid() && SpatialObjects.contains(Peer.OwnerCharacter))
				Relevant.insert(Peer.OwnerCharacter);
			for (const auto Object : Relevant)
				if (!Peer.RelevantSpatialRoots.contains(Object)) SaturatingIncrement(Metrics.RelevanceEnters);
			for (const auto Object : Peer.RelevantSpatialRoots)
				if (!Relevant.contains(Object)) SaturatingIncrement(Metrics.RelevanceLeaves);
			SaturatingIncrement(
				Metrics.RelevanceEvaluations, static_cast<std::uint64_t>(QueryScratch.Candidates.size())
			);
			HysteresisWork.reset();
			{
				runtime_detail::WorkScope CommitWork(runtime_detail::WorkPhase::RelevanceCommit);
				Peer.RelevantSpatialRoots = std::move(Relevant);
			}
			if (!BuildSelection(Peer)) return false;
			FinishEvaluation();
			return true;
		}

		bool ProcessPeers(std::uint64_t SimulationTick, std::size_t &Remaining, bool CriticalOnly) {
			if (Peers.empty()) return true;
			auto &Cursor = CriticalOnly ? CriticalCursor : OrdinaryCursor;
			auto Current = Peers.upper_bound(Cursor);
			for (std::size_t Visited = 0; Visited < Peers.size() && Remaining != 0; ++Visited) {
				if (Current == Peers.end()) Current = Peers.begin();
				auto &[Connection, Peer] = *Current++;
				Cursor = Connection;
				if ((CriticalOnly && !Peer.CriticalDirty) || !NeedsEvaluation(Peer, SimulationTick)) continue;
				if (SimulationTick >= Peer.PendingSinceTick && Peer.PendingSinceTick != 0)
					Metrics.MaximumPendingAgeTicks = std::max(Metrics.MaximumPendingAgeTicks,
						SimulationTick - Peer.PendingSinceTick);
				if (!UpdatePeer(Peer, SimulationTick)) return false;
				--Remaining;
				++EvaluationsThisTick;
				SaturatingIncrement(Metrics.PeerEvaluations);
				++Metrics.PeerEvaluationsLastUpdate;
			}
			return true;
		}
	};

	ReplicationRelevance::ReplicationRelevance(
		std::shared_ptr<Instance> SourceRoot,
		ExclusionPolicy IsExcluded,
		ReplicationRelevanceConfiguration Configuration
	)
		: State(std::make_unique<Implementation>(std::move(SourceRoot), std::move(IsExcluded), Configuration)) {}

	ReplicationRelevance::~ReplicationRelevance() = default;

	bool ReplicationRelevance::AddPeer(ConnectionId Connection, ObjectId LocalPlayer, ObjectId OwnerCharacter) {
		if (!State->Healthy || !Connection.IsValid() || !LocalPlayer.IsValid() ||
			State->Peers.size() >= MaximumReplicationRelevancePeers || State->Peers.contains(Connection))
			return false;
		State->RefreshDirtySpatialPositions();
		if (!State->Healthy) return false;
		for (const auto &[Existing, Peer] : State->Peers) {
			(void)Peer;
			if (Existing.Slot == Connection.Slot) return false;
		}
		auto [Iterator, Added] = State->Peers.emplace(
			Connection, Implementation::PeerState{.LocalPlayer = LocalPlayer, .OwnerCharacter = OwnerCharacter}
		);
		if (!Added || !State->UpdatePeer(Iterator->second, 0)) {
			State->Peers.erase(Connection);
			return false;
		}
		State->RefreshDesiredObjectGauge();
		State->NextWorkTick = 0;
		return true;
	}

	bool ReplicationRelevance::RemovePeer(ConnectionId Connection) {
		const bool Removed = State->Peers.erase(Connection) != 0;
		if (Removed) {
			State->RefreshDesiredObjectGauge();
			State->NextWorkTick = 0;
		}
		return Removed;
	}

	bool ReplicationRelevance::SetOwnerCharacter(ConnectionId Connection, ObjectId Character) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end()) return false;
		if (Character.IsValid() && !State->SpatialObjects.contains(Character)) return false;
		if (Peer->second.OwnerCharacter == Character) return true;
		Peer->second.OwnerCharacter = Character;
		Peer->second.Dirty = true;
		Peer->second.CriticalDirty = true;
		State->NextWorkTick = 0;
		return true;
	}

	bool ReplicationRelevance::SetTrustedFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end() || FocusPoints.size() > MaximumReplicationFocusPoints ||
			std::ranges::any_of(FocusPoints, [](glm::vec3 Point) { return !Finite(Point); }))
			return false;
		if (std::ranges::equal(Peer->second.TrustedFocus, FocusPoints)) return true;
		Peer->second.TrustedFocus.assign(FocusPoints.begin(), FocusPoints.end());
		Peer->second.Dirty = true;
		State->NextWorkTick = 0;
		return true;
	}

	bool ReplicationRelevance::Update(std::uint64_t SimulationTick) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::Relevance);
		if (!State->Healthy || SimulationTick == 0) return false;
		if (State->EvaluationPass == std::numeric_limits<std::uint64_t>::max() ||
			(State->WorldDirty && State->WorldRevision == std::numeric_limits<std::uint64_t>::max())) {
			State->Fail("Relevance revision exhausted");
			return false;
		}
		++State->EvaluationPass;
		State->Metrics.PeerEvaluationsLastUpdate = 0;
		if (SimulationTick != State->LastSimulationTick) State->EvaluationsThisTick = 0;
		const auto Started = std::chrono::steady_clock::now();
		auto RecordDuration = [&] {
			SaturatingIncrement(State->Metrics.UpdateCpuNanoseconds, static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()));
		};
		runtime_detail::MeasureWork(runtime_detail::WorkPhase::SpatialUpdate,
			[&] { State->RefreshDirtySpatialPositions(); });
		if (!State->Healthy) return false;
		if (State->WorldDirty) {
			++State->WorldRevision;
			State->WorldDirty = false;
		}
		if (SimulationTick >= State->LastSimulationTick && SimulationTick < State->NextWorkTick) {
			State->LastSimulationTick = SimulationTick;
			RecordDuration();
			return true; // No peer/world scan, queue allocation, or selection copy on the idle path.
		}
		State->LastSimulationTick = SimulationTick;
		for (auto &[Connection, Peer] : State->Peers) {
			(void)Connection;
			if (State->NeedsEvaluation(Peer, SimulationTick) && Peer.PendingSinceTick == 0)
				Peer.PendingSinceTick = SimulationTick;
		}
		// Reserve at most a quarter for prompt owner-lifecycle changes. The ordinary
		// rotating pass always retains service, including under sustained critical work.
		const auto Available = State->Configuration.MaximumPeerEvaluationsPerTick - State->EvaluationsThisTick;
		auto CriticalRemaining = std::min(Available,
			std::max<std::size_t>(1, State->Configuration.MaximumPeerEvaluationsPerTick / 4));
		if (!State->ProcessPeers(SimulationTick, CriticalRemaining, true)) return false;
		auto Remaining = State->Configuration.MaximumPeerEvaluationsPerTick - State->EvaluationsThisTick;
		if (!State->ProcessPeers(SimulationTick, Remaining, false)) return false;
		State->Metrics.PeerEvaluationsHighWater = std::max(
			State->Metrics.PeerEvaluationsHighWater, static_cast<std::uint64_t>(State->EvaluationsThisTick));
		State->Metrics.DeferredPeers = 0;
		State->Metrics.OldestPendingAgeTicks = 0;
		State->NextWorkTick = std::numeric_limits<std::uint64_t>::max();
		for (const auto &[Connection, Peer] : State->Peers) {
			(void)Connection;
			if (State->NeedsEvaluation(Peer, SimulationTick)) {
				++State->Metrics.DeferredPeers;
				State->NextWorkTick = SimulationTick;
				State->Metrics.OldestPendingAgeTicks = std::max(State->Metrics.OldestPendingAgeTicks,
					SimulationTick >= Peer.PendingSinceTick ? SimulationTick - Peer.PendingSinceTick : 0);
			} else {
				auto Due = Peer.LastUpdateTick;
				SaturatingIncrement(Due, State->Configuration.UpdateIntervalTicks);
				State->NextWorkTick = std::min(State->NextWorkTick, Due);
			}
		}
		State->Metrics.DeferredPeersHighWater = std::max(State->Metrics.DeferredPeersHighWater, State->Metrics.DeferredPeers);
		State->Metrics.MaximumPendingAgeTicks = std::max(State->Metrics.MaximumPendingAgeTicks, State->Metrics.OldestPendingAgeTicks);
		State->RefreshDesiredObjectGauge();
		RecordDuration();
		return State->Healthy;
	}

	const PeerRelevanceSelection *ReplicationRelevance::GetSelection(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? nullptr : Peer->second.Selection.get();
	}

	std::shared_ptr<const PeerRelevanceSelection> ReplicationRelevance::GetSelectionSnapshot(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? nullptr : Peer->second.Selection;
	}

	std::span<const glm::vec3> ReplicationRelevance::GetResolvedFocus(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? std::span<const glm::vec3>{}
										  : std::span<const glm::vec3>{Peer->second.ResolvedFocus};
	}

	std::span<const ObjectId> ReplicationRelevance::GetRuntimeCharacterCandidates(ConnectionId Connection) const {
		const auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? std::span<const ObjectId>{}
			: std::span<const ObjectId>{Peer->second.RuntimeCharacterCandidates};
	}

	bool ReplicationRelevance::IsRuntimeRelevant(ConnectionId Connection, ObjectId Object) const {
		auto Peer = State->Peers.find(Connection);
		return Peer != State->Peers.end() && State->SpatialObjects.contains(Object) &&
			Peer->second.RelevantSpatialRoots.contains(Object);
	}

	bool ReplicationRelevance::WasSelectionEvaluated(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer != State->Peers.end() && Peer->second.EvaluationPass == State->EvaluationPass;
	}

	bool ReplicationRelevance::IsHealthy() const {
		return State->Healthy;
	}

	const std::string &ReplicationRelevance::GetFailure() const {
		return State->Failure;
	}

	ReplicationRelevanceMetrics ReplicationRelevance::GetMetrics() const {
		auto Result = State->Metrics;
		for (const auto &[Connection, Peer] : State->Peers) {
			(void)Connection;
			SaturatingIncrement(Result.CharacterCandidateBytes,
				static_cast<std::uint64_t>(Peer.RuntimeCharacterCandidates.capacity() * sizeof(ObjectId)));
		}
		// Conservative padded storage bound for new fixed peer/cursor metadata.
		// Existing semantic selections and spatial-index storage are not copied.
		Result.StagingBytes = State->Peers.size() * (4 * sizeof(std::uint64_t)) +
			2 * sizeof(ConnectionId) + 5 * sizeof(std::uint64_t) + sizeof(std::size_t);
		const auto Spatial = State->Spatial.GetIndexMetrics();
		Result.SpatialQueries = Spatial.RegionQueries;
		Result.QueryRegions = Spatial.RegionsVisited;
		Result.CandidateObjects = Spatial.CandidateObjects;
		Result.CandidateMembershipVisits = Spatial.CandidateMembershipVisits;
		Result.CandidateDedupHits = Spatial.CandidateDedupHits;
		Result.SpatialEntries = Spatial.SpatialObjectCount;
		Result.SpatialMemberships = Spatial.MembershipCount;
		Result.SpatialRegions = Spatial.RegionCount;
		Result.LargeSpatialObjects = Spatial.LargeObjectCount;
		Result.SpatialMoves = Spatial.MembershipMoves;
		Result.SameRegionUpdates = Spatial.SameRegionUpdates;
		Result.SpatialRemovals = Spatial.ObjectRemovals;
		Result.RegionBucketsCreated = Spatial.RegionBucketsCreated;
		Result.RegionBucketsRemoved = Spatial.RegionBucketsRemoved;
		Result.PeakRegionOccupancy = Spatial.PeakRegionOccupancy;
		Result.SpatialQueryLimitFailures = Spatial.QueryLimitFailures;
		Result.SpatialCandidateLimitFailures = Spatial.CandidateLimitFailures;
		return Result;
	}

	std::optional<SpatialCellAddress> ReplicationRelevance::GetSpatialCellAddress(ObjectId Object) const {
		return State->Spatial.GetCellAddress(Object);
	}

	bool ReplicationRelevance::IsLargeSpatialObject(ObjectId Object) const {
		return State->Spatial.IsLargeObject(Object);
	}

	bool ReplicationRelevance::VerifySpatialIndex() const {
		if (!State->Spatial.VerifyConsistency() || State->Spatial.GetProjectionCount() != State->SpatialObjects.size())
			return false;
		for (const auto &[Object, Entry] : State->SpatialObjects) {
			auto InstanceValue = Entry.Object.lock();
			const auto *Projection = State->Spatial.Get(Object);
			auto Pose = State->PoseOf(InstanceValue);
			auto Bounds = State->BoundsOf(InstanceValue);
			if (!InstanceValue || !Projection || !Pose || !Bounds ||
				ObjectRegistry::Get().Lookup(Object).get() != InstanceValue.get() ||
				Projection->Pose.Space != State->Spatial.GetDefaultSpace() ||
				!Projection->Pose.LocalTransform.FuzzyEq(*Pose) ||
				!Equal(Projection->Bounds.Minimum, Bounds->Minimum) ||
				!Equal(Projection->Bounds.Maximum, Bounds->Maximum))
				return false;
		}
		return true;
	}
}

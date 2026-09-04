#include "gargantuan/network/ReplicationRelevance.hpp"

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
			   LeaveRadius >= EnterRadius && UpdateIntervalTicks != 0 && MaximumSpatialObjects != 0 &&
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
			glm::vec3 Position{0.0f};
			std::set<ObjectId> Members;
			std::vector<SignalConnection::Pointer> SpatialChanged;
			bool Dirty = false;
		};

		struct PeerState {
			ObjectId LocalPlayer;
			ObjectId OwnerCharacter;
			std::vector<glm::vec3> TrustedFocus;
			std::vector<glm::vec3> ResolvedFocus;
			std::set<ObjectId> RelevantSpatialRoots;
			PeerRelevanceSelection Selection;
			std::uint64_t LastUpdateTick = 0;
			bool Dirty = true;
			bool SelectionEvaluated = false;
		};

		std::shared_ptr<Instance> SourceRoot;
		ExclusionPolicy IsExcluded;
		ReplicationRelevanceConfiguration Configuration;
		std::map<ObjectId, ObjectLocation> ObjectLocations;
		std::set<ObjectId> GlobalObjects;
		std::map<ObjectId, SpatialEntry> SpatialObjects;
		std::vector<ObjectId> DirtySpatialObjects;
		SpatialRegionIndex Regions;
		SpatialRegionQueryScratch QueryScratch;
		std::map<ConnectionId, PeerState> Peers;
		SignalConnection::Pointer DescendantAdded;
		SignalConnection::Pointer DescendantRemoved;
		ReplicationRelevanceMetrics Metrics;
		bool Healthy = true;
		std::string Failure;

		Implementation(
			std::shared_ptr<Instance> SourceRootValue,
			ExclusionPolicy IsExcludedValue,
			ReplicationRelevanceConfiguration ConfigurationValue
		)
			: SourceRoot(std::move(SourceRootValue)), IsExcluded(std::move(IsExcludedValue)),
			  Configuration(ConfigurationValue), Regions(ConfigurationValue.SpatialConfiguration()) {
			if (!SourceRoot || !std::dynamic_pointer_cast<DataModel>(SourceRoot) || !Configuration.IsValid())
				throw std::invalid_argument("[Replication:Relevance] configuration or source root is invalid");
			QueryScratch.Reserve(Configuration.SpatialConfiguration());
			DirtySpatialObjects.reserve(Configuration.MaximumSpatialObjects);
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

		void MarkSpatialDirty(ObjectId Object) {
			auto Found = SpatialObjects.find(Object);
			if (Found == SpatialObjects.end() || Found->second.Dirty) return;
			if (DirtySpatialObjects.size() >= Configuration.MaximumSpatialObjects) {
				std::erase_if(DirtySpatialObjects, [this](ObjectId Candidate) {
					auto Current = SpatialObjects.find(Candidate);
					return Current == SpatialObjects.end() || !Current->second.Dirty;
				});
			}
			if (DirtySpatialObjects.size() >= Configuration.MaximumSpatialObjects) {
				Fail("Dirty spatial-object work exceeds its object limit");
				return;
			}
			Found->second.Dirty = true;
			DirtySpatialObjects.push_back(Object);
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

		std::optional<glm::vec3> PositionOf(const std::shared_ptr<Instance> &Object) const {
			if (auto CharacterValue = std::dynamic_pointer_cast<Character>(Object))
				return CharacterValue->GetPosition();
			if (auto PartValue = std::dynamic_pointer_cast<BasePart>(Object)) return PartValue->GetCFrame().Position;
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
			auto Position = PositionOf(Object);
			auto Bounds = BoundsOf(Object);
			if (!Position || !Finite(*Position) || !Bounds) {
				Fail("Spatial object has invalid authoritative bounds");
				return false;
			}
			SpatialEntry Entry{.Object = Object, .Position = *Position};
			Entry.Members.insert(Id);
			const auto Status = Regions.Register(Id, *Bounds);
			if (Status != SpatialRegionStatus::Success) {
				FailSpatial("Spatial root registration", Status);
				return false;
			}
			decltype(SpatialObjects)::iterator Found;
			bool Added = false;
			try {
				std::tie(Found, Added) = SpatialObjects.emplace(Id, std::move(Entry));
			} catch (...) {
				(void)Regions.Remove(Id);
				throw;
			}
			if (!Added) {
				(void)Regions.Remove(Id);
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
				(void)Regions.Remove(Id);
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
			for (auto &[Connection, Peer] : Peers) {
				(void)Connection;
				Peer.Dirty = true;
			}
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
					const auto Status = Regions.Remove(Object);
					if (Status != SpatialRegionStatus::Success && Status != SpatialRegionStatus::MissingObject)
						FailSpatial("Spatial root removal", Status);
					SpatialObjects.erase(Root);
					SaturatingIncrement(Metrics.SpatialRemovals);
				}
			} else {
				GlobalObjects.erase(Object);
			}
			ObjectLocations.erase(Location);
			Metrics.SpatialEntries = SpatialObjects.size();
			for (auto &[Connection, Peer] : Peers) {
				(void)Connection;
				Peer.RelevantSpatialRoots.erase(Object);
				Peer.Dirty = true;
			}
		}

		void UpdateSpatialPosition(ObjectId Object) {
			auto Found = SpatialObjects.find(Object);
			if (Found == SpatialObjects.end()) return;
			auto InstanceValue = Found->second.Object.lock();
			auto Position = PositionOf(InstanceValue);
			auto Bounds = BoundsOf(InstanceValue);
			if (!InstanceValue || !Position || !Finite(*Position) || !Bounds) {
				Fail("Spatial root lost valid authoritative bounds");
				return;
			}
			Found->second.Position = *Position;
			const auto Status = Regions.Update(Object, *Bounds);
			if (Status != SpatialRegionStatus::Success) FailSpatial("Spatial root membership update", Status);
		}

		void RefreshDirtySpatialPositions() {
			for (const auto Object : DirtySpatialObjects) {
				auto Found = SpatialObjects.find(Object);
				if (Found != SpatialObjects.end()) Found->second.Dirty = false;
				UpdateSpatialPosition(Object);
				if (!Healthy) return;
			}
			DirtySpatialObjects.clear();
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
			std::array<SpatialRegionQueryVolume, MaximumReplicationFocusPoints> Volumes;
			for (std::size_t Index = 0; Index < Focus.size(); ++Index)
				Volumes[Index] = {
					.Space = DefaultSpatialSpaceId,
					.Center = glm::dvec3(Focus[Index]),
					.Radius = Configuration.LeaveRadius,
				};
			const auto Status = Regions.Query(std::span(Volumes).first(Focus.size()), QueryScratch);
			if (Status == SpatialRegionStatus::Success) return true;
			FailSpatial("Replication region candidate query", Status);
			return false;
		}

		std::vector<glm::vec3> ResolveFocus(const PeerState &Peer) const {
			if (!Peer.TrustedFocus.empty()) return Peer.TrustedFocus;
			auto Owner = SpatialObjects.find(Peer.OwnerCharacter);
			return Owner == SpatialObjects.end() ? std::vector<glm::vec3>{}
												 : std::vector<glm::vec3>{Owner->second.Position};
		}

		bool BuildSelection(PeerState &Peer) {
			std::set<ObjectId> Required;
			std::set<ObjectId> Desired = GlobalObjects;
			if (Peer.LocalPlayer.IsValid()) Required.insert(Peer.LocalPlayer);
			for (const auto RootId : Peer.RelevantSpatialRoots) {
				auto Root = SpatialObjects.find(RootId);
				if (Root == SpatialObjects.end()) continue;
				Desired.insert(Root->second.Members.begin(), Root->second.Members.end());
				if (RootId == Peer.OwnerCharacter)
					Required.insert(Root->second.Members.begin(), Root->second.Members.end());
			}
			Desired.insert(Required.begin(), Required.end());
			if (Desired.size() > Configuration.MaximumDesiredObjectsPerPeer ||
				Required.size() > Configuration.MaximumDesiredObjectsPerPeer) {
				Fail("Peer relevance selection exceeds its object limit");
				return false;
			}
			Peer.Selection.RequiredObjects.assign(Required.begin(), Required.end());
			Peer.Selection.DesiredObjects.assign(Desired.begin(), Desired.end());
			return true;
		}

		void RefreshDesiredObjectGauge() {
			Metrics.DesiredObjects = 0;
			for (const auto &[Connection, Peer] : Peers) {
				(void)Connection;
				SaturatingIncrement(
					Metrics.DesiredObjects, static_cast<std::uint64_t>(Peer.Selection.DesiredObjects.size())
				);
			}
		}

		bool UpdatePeer(PeerState &Peer, std::uint64_t SimulationTick, bool Force) {
			if (!Force && !Peer.Dirty && Peer.LastUpdateTick != 0 && SimulationTick >= Peer.LastUpdateTick &&
				SimulationTick - Peer.LastUpdateTick < Configuration.UpdateIntervalTicks)
				return true;
			Peer.SelectionEvaluated = true;
			auto Focus = ResolveFocus(Peer);
			Peer.ResolvedFocus = Focus;
			if (Focus.empty())
				QueryScratch.Clear();
			else if (!Query(Focus))
				return false;
			std::set<ObjectId> Relevant;
			for (const auto Existing : Peer.RelevantSpatialRoots) {
				auto Found = SpatialObjects.find(Existing);
				if (Found != SpatialObjects.end() &&
					(Existing == Peer.OwnerCharacter ||
					 WithinAnyFocus(Found->second.Position, Focus, Configuration.LeaveRadius)))
					Relevant.insert(Existing);
			}
			for (const auto Candidate : QueryScratch.Candidates) {
				auto Found = SpatialObjects.find(Candidate);
				if (Found != SpatialObjects.end() &&
					WithinAnyFocus(Found->second.Position, Focus, Configuration.EnterRadius))
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
			Peer.RelevantSpatialRoots = std::move(Relevant);
			Peer.LastUpdateTick = SimulationTick;
			Peer.Dirty = false;
			return BuildSelection(Peer);
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
		if (!Added || !State->UpdatePeer(Iterator->second, 0, true)) {
			State->Peers.erase(Connection);
			return false;
		}
		State->RefreshDesiredObjectGauge();
		return true;
	}

	bool ReplicationRelevance::RemovePeer(ConnectionId Connection) {
		const bool Removed = State->Peers.erase(Connection) != 0;
		if (Removed) State->RefreshDesiredObjectGauge();
		return Removed;
	}

	bool ReplicationRelevance::SetOwnerCharacter(ConnectionId Connection, ObjectId Character) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end()) return false;
		if (Character.IsValid() && !State->SpatialObjects.contains(Character)) return false;
		if (Peer->second.OwnerCharacter == Character) return true;
		Peer->second.OwnerCharacter = Character;
		Peer->second.Dirty = true;
		return true;
	}

	bool ReplicationRelevance::SetTrustedFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end() || FocusPoints.size() > MaximumReplicationFocusPoints ||
			std::ranges::any_of(FocusPoints, [](glm::vec3 Point) { return !Finite(Point); }))
			return false;
		Peer->second.TrustedFocus.assign(FocusPoints.begin(), FocusPoints.end());
		Peer->second.Dirty = true;
		return true;
	}

	bool ReplicationRelevance::Update(std::uint64_t SimulationTick) {
		if (!State->Healthy || SimulationTick == 0) return false;
		const auto Started = std::chrono::steady_clock::now();
		State->RefreshDirtySpatialPositions();
		if (!State->Healthy) return false;
		for (auto &[Connection, Peer] : State->Peers) {
			(void)Connection;
			Peer.SelectionEvaluated = false;
		}
		for (auto &[Connection, Peer] : State->Peers) {
			(void)Connection;
			if (!State->UpdatePeer(Peer, SimulationTick, false)) return false;
		}
		State->RefreshDesiredObjectGauge();
		SaturatingIncrement(
			State->Metrics.UpdateCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			)
		);
		return State->Healthy;
	}

	const PeerRelevanceSelection *ReplicationRelevance::GetSelection(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? nullptr : &Peer->second.Selection;
	}

	std::span<const glm::vec3> ReplicationRelevance::GetResolvedFocus(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer == State->Peers.end() ? std::span<const glm::vec3>{}
										  : std::span<const glm::vec3>{Peer->second.ResolvedFocus};
	}

	bool ReplicationRelevance::IsRuntimeRelevant(ConnectionId Connection, ObjectId Object) const {
		auto Peer = State->Peers.find(Connection);
		return Peer != State->Peers.end() && Peer->second.RelevantSpatialRoots.contains(Object);
	}

	bool ReplicationRelevance::WasSelectionEvaluated(ConnectionId Connection) const {
		auto Peer = State->Peers.find(Connection);
		return Peer != State->Peers.end() && Peer->second.SelectionEvaluated;
	}

	bool ReplicationRelevance::IsHealthy() const {
		return State->Healthy;
	}

	const std::string &ReplicationRelevance::GetFailure() const {
		return State->Failure;
	}

	ReplicationRelevanceMetrics ReplicationRelevance::GetMetrics() const {
		auto Result = State->Metrics;
		const auto Spatial = State->Regions.GetMetrics();
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

	std::optional<SpatialAddress> ReplicationRelevance::GetSpatialAddress(ObjectId Object) const {
		return State->Regions.GetPrimaryAddress(Object);
	}

	bool ReplicationRelevance::IsLargeSpatialObject(ObjectId Object) const {
		return State->Regions.IsLargeObject(Object);
	}

	bool ReplicationRelevance::VerifySpatialIndex() const {
		return State->Regions.VerifyConsistency();
	}
}

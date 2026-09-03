#include "gargantuan/network/ReplicationRelevance.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

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

		struct CellAddress {
			std::int32_t X = 0;
			std::int32_t Y = 0;
			std::int32_t Z = 0;
			auto operator<=>(const CellAddress &) const = default;
		};

		std::optional<std::int32_t> CellCoordinate(float Value, float CellSize) {
			const auto Coordinate = std::floor(static_cast<double>(Value) / static_cast<double>(CellSize));
			if (!std::isfinite(Coordinate) || Coordinate < std::numeric_limits<std::int32_t>::min() ||
				Coordinate > std::numeric_limits<std::int32_t>::max())
				return std::nullopt;
			return static_cast<std::int32_t>(Coordinate);
		}

		std::optional<CellAddress> CellFor(glm::vec3 Position, float CellSize) {
			auto X = CellCoordinate(Position.x, CellSize);
			auto Y = CellCoordinate(Position.y, CellSize);
			auto Z = CellCoordinate(Position.z, CellSize);
			return X && Y && Z ? std::optional(CellAddress{*X, *Y, *Z}) : std::nullopt;
		}
	}

	bool ReplicationRelevanceConfiguration::IsValid() const {
		return std::isfinite(CellSize) && CellSize > 0.0f && std::isfinite(EnterRadius) && EnterRadius > 0.0f &&
			   std::isfinite(LeaveRadius) && LeaveRadius >= EnterRadius && UpdateIntervalTicks != 0 &&
			   MaximumSpatialObjects != 0 && MaximumSpatialObjects <= MaximumReplicationSpatialObjects &&
			   MaximumSpatialCells != 0 && MaximumSpatialCells <= MaximumReplicationSpatialCells &&
			   MaximumDesiredObjectsPerPeer != 0 && MaximumDesiredObjectsPerPeer <= MaximumPeerDesiredObjects &&
			   MaximumQueryCells != 0 && MaximumQueryCells <= MaximumReplicationQueryCells;
	}

	struct ReplicationRelevance::Implementation {
		struct ObjectLocation {
			std::optional<ObjectId> SpatialRoot;
		};

		struct SpatialEntry {
			std::weak_ptr<Instance> Object;
			glm::vec3 Position{0.0f};
			CellAddress Cell;
			std::set<ObjectId> Members;
			SignalConnection::Pointer PositionChanged;
			bool DynamicCharacter = false;
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
		std::set<ObjectId> DynamicSpatialObjects;
		std::map<CellAddress, std::set<ObjectId>> Cells;
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
			  Configuration(ConfigurationValue) {
			if (!SourceRoot || !std::dynamic_pointer_cast<DataModel>(SourceRoot) || !Configuration.IsValid())
				throw std::invalid_argument("[Replication:Relevance] configuration or source root is invalid");
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
		}

		void Fail(std::string Message) {
			Healthy = false;
			Failure = std::move(Message);
			SaturatingIncrement(Metrics.LimitFailures);
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

		bool InsertCell(ObjectId Object, CellAddress Cell) {
			auto Found = Cells.find(Cell);
			if (Found == Cells.end()) {
				if (Cells.size() >= Configuration.MaximumSpatialCells) {
					Fail("Spatial cell limit exceeded");
					return false;
				}
				Found = Cells.emplace(Cell, std::set<ObjectId>{}).first;
			}
			Found->second.insert(Object);
			return true;
		}

		void RemoveCell(ObjectId Object, CellAddress Cell) {
			auto Found = Cells.find(Cell);
			if (Found == Cells.end()) return;
			Found->second.erase(Object);
			if (Found->second.empty()) Cells.erase(Found);
		}

		bool AddSpatialRoot(const std::shared_ptr<Instance> &Object) {
			const auto Id = Object->GetObjectId();
			if (SpatialObjects.contains(Id)) return true;
			if (SpatialObjects.size() >= Configuration.MaximumSpatialObjects) {
				Fail("Spatial object limit exceeded");
				return false;
			}
			auto Position = PositionOf(Object);
			auto Cell = Position ? CellFor(*Position, Configuration.CellSize) : std::nullopt;
			if (!Position || !Finite(*Position) || !Cell) {
				Fail("Spatial object has an invalid position or cell address");
				return false;
			}
			SpatialEntry Entry{
				.Object = Object,
				.Position = *Position,
				.Cell = *Cell,
				.DynamicCharacter = static_cast<bool>(std::dynamic_pointer_cast<Character>(Object)),
			};
			Entry.Members.insert(Id);
			if (!Entry.DynamicCharacter) {
				Entry.PositionChanged = Object->GetPropertyChangedSignal("CFrame")->Connect([this, Id](std::monostate) {
					UpdateSpatialPosition(Id);
				});
			}
			if (!InsertCell(Id, *Cell)) return false;
			SpatialObjects.emplace(Id, std::move(Entry));
			if (std::dynamic_pointer_cast<Character>(Object)) DynamicSpatialObjects.insert(Id);
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
					RemoveCell(Object, Root->second.Cell);
					SpatialObjects.erase(Root);
					DynamicSpatialObjects.erase(Object);
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
			auto Cell = Position ? CellFor(*Position, Configuration.CellSize) : std::nullopt;
			if (!InstanceValue || !Position || !Finite(*Position) || !Cell) {
				UnregisterObject(Object);
				return;
			}
			Found->second.Position = *Position;
			if (*Cell == Found->second.Cell) return;
			const auto Previous = Found->second.Cell;
			if (!InsertCell(Object, *Cell)) return;
			Found = SpatialObjects.find(Object);
			if (Found == SpatialObjects.end()) return;
			Found->second.Cell = *Cell;
			RemoveCell(Object, Previous);
			SaturatingIncrement(Metrics.SpatialMoves);
		}

		void RefreshDynamicPositions() {
			const std::vector<ObjectId> Dynamic(DynamicSpatialObjects.begin(), DynamicSpatialObjects.end());
			for (const auto Object : Dynamic)
				UpdateSpatialPosition(Object);
		}

		bool WithinAnyFocus(glm::vec3 Position, std::span<const glm::vec3> Focus, float Radius) const {
			const auto RadiusSquared = static_cast<double>(Radius) * Radius;
			for (const auto Point : Focus) {
				const auto Difference = glm::dvec3(Position) - glm::dvec3(Point);
				if (glm::dot(Difference, Difference) <= RadiusSquared) return true;
			}
			return false;
		}

		bool Query(std::span<const glm::vec3> Focus, std::set<ObjectId> &Candidates) {
			for (const auto Point : Focus) {
				auto Minimum = CellFor(Point - glm::vec3(Configuration.LeaveRadius), Configuration.CellSize);
				auto Maximum = CellFor(Point + glm::vec3(Configuration.LeaveRadius), Configuration.CellSize);
				if (!Minimum || !Maximum) {
					Fail("Replication focus has an invalid cell address");
					return false;
				}
				const auto XCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(Maximum->X) - Minimum->X + 1);
				const auto YCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(Maximum->Y) - Minimum->Y + 1);
				const auto ZCount = static_cast<std::uint64_t>(static_cast<std::int64_t>(Maximum->Z) - Minimum->Z + 1);
				if (XCount > Configuration.MaximumQueryCells || YCount > Configuration.MaximumQueryCells ||
					ZCount > Configuration.MaximumQueryCells || XCount * YCount > Configuration.MaximumQueryCells ||
					XCount * YCount * ZCount > Configuration.MaximumQueryCells) {
					Fail("Replication spatial query cell limit exceeded");
					return false;
				}
				SaturatingIncrement(Metrics.SpatialQueries);
				SaturatingIncrement(Metrics.QueryCells, XCount * YCount * ZCount);
				for (std::int64_t X = Minimum->X; X <= Maximum->X; ++X)
					for (std::int64_t Y = Minimum->Y; Y <= Maximum->Y; ++Y)
						for (std::int64_t Z = Minimum->Z; Z <= Maximum->Z; ++Z) {
							auto Cell = Cells.find({
								static_cast<std::int32_t>(X),
								static_cast<std::int32_t>(Y),
								static_cast<std::int32_t>(Z),
							});
							if (Cell != Cells.end()) Candidates.insert(Cell->second.begin(), Cell->second.end());
						}
			}
			SaturatingIncrement(Metrics.CandidateObjects, static_cast<std::uint64_t>(Candidates.size()));
			return true;
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
			std::set<ObjectId> Candidates;
			if (!Focus.empty() && !Query(Focus, Candidates)) return false;
			std::set<ObjectId> Relevant;
			for (const auto Existing : Peer.RelevantSpatialRoots) {
				auto Found = SpatialObjects.find(Existing);
				if (Found != SpatialObjects.end() &&
					(Existing == Peer.OwnerCharacter ||
					 WithinAnyFocus(Found->second.Position, Focus, Configuration.LeaveRadius)))
					Relevant.insert(Existing);
			}
			for (const auto Candidate : Candidates) {
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
			SaturatingIncrement(Metrics.RelevanceEvaluations, static_cast<std::uint64_t>(Candidates.size()));
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
		State->RefreshDynamicPositions();
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
		State->RefreshDynamicPositions();
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
		return State->Metrics;
	}
}

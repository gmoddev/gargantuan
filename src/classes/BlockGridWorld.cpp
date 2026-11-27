#include "BlockGridWorldState.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <limits>
#include <type_traits>

namespace gargantuan {
	namespace {
		constexpr unsigned Side = blockgrid::LeafSide;
		constexpr std::size_t MaximumGridBoxes = MaximumRaycastFilterBodies;
		constexpr std::size_t MaximumWorldBoxes = MaximumPhysicsCollisionConsumers;

		std::vector<BlockGridBox> LeafBoxes(blockgrid::Cell Origin, const blockgrid::Leaf &Leaf) {
				std::vector<BlockGridBox> Boxes;
				if (Leaf.GetOccupiedCount() == blockgrid::LeafVolume) {
					const int Extent = static_cast<int>(Side);
					Boxes.push_back({Origin.X, Origin.Y, Origin.Z, Origin.X+Extent, Origin.Y+Extent, Origin.Z+Extent});
					return Boxes;
				}
				std::bitset<Side * Side * Side> Filled;
				const auto Index = [](unsigned X, unsigned Y, unsigned Z) { return (Z * Side + Y) * Side + X; };
				Leaf.ForEachOccupied([&](unsigned X, unsigned Y, unsigned Z, blockgrid::Block) {
					Filled.set(Index(X, Y, Z));
				});
				for (unsigned Z = 0; Z != Side; ++Z) {
					for (unsigned Y = 0; Y != Side; ++Y) {
						for (unsigned X = 0; X != Side; ++X) {
							if (!Filled.test(Index(X, Y, Z))) continue;
							unsigned EndX = X + 1, EndY = Y + 1, EndZ = Z + 1;
							while (EndX < Side && Filled.test(Index(EndX, Y, Z))) ++EndX;
							const auto FullRow = [&](unsigned Row, unsigned Plane) {
								for (unsigned I = X; I < EndX; ++I)
									if (!Filled.test(Index(I, Row, Plane))) return false;
								return true;
							};
							while (EndY < Side && FullRow(EndY, Z)) ++EndY;
							while (EndZ < Side) {
								unsigned Row = Y;
								while (Row < EndY && FullRow(Row, EndZ)) ++Row;
								if (Row < EndY) break;
								++EndZ;
							}
							for (unsigned K = Z; K < EndZ; ++K)
								for (unsigned J = Y; J < EndY; ++J)
									for (unsigned I = X; I < EndX; ++I) Filled.reset(Index(I, J, K));
							Boxes.push_back({Origin.X + static_cast<int>(X), Origin.Y + static_cast<int>(Y),
								Origin.Z + static_cast<int>(Z), Origin.X + static_cast<int>(EndX),
								Origin.Y + static_cast<int>(EndY), Origin.Z + static_cast<int>(EndZ)});
						}
					}
				}
				return Boxes;
		}

		template <typename Snapshot>
		bool PrepareTopology(BlockGridWorldState &State, BlockGridParticipant &Member, const Snapshot &Cells) {
			const bool Active = Member.Grid->GetCanCollide() || Member.Grid->GetCanQuery();
			const bool Patch = Active && Member.TopologyCurrent && Member.TopologyActive &&
				std::is_same_v<Snapshot, blockgrid::Field::Candidate>;
			BlockGridLeafBoxes Changed;
			std::vector<blockgrid::Cell> Replaced;
			std::size_t OldCount = 0, NewCount = 0;
			const auto Visit = [&](blockgrid::Cell Origin, const blockgrid::Leaf *Page) {
				if (NewCount > MaximumGridBoxes) return;
				if (Patch) {
					Replaced.push_back(Origin);
					if (auto It = Member.Desired.find(Origin); It != Member.Desired.end()) OldCount += It->second.size();
				}
				if (!Page) return;
				auto Boxes = LeafBoxes(Origin, *Page);
				NewCount += Boxes.size();
				Changed.emplace(Origin, std::move(Boxes));
			};
			if (Active) {
				if constexpr (std::is_same_v<Snapshot, blockgrid::Field::Candidate>) {
					if (Patch) Cells.ForEachChangedLeaf(Visit);
					else Cells.ForEachLeaf([&](auto Origin, const auto &Page) { Visit(Origin, &Page); });
				} else Cells.ForEachLeaf([&](auto Origin, const auto &Page) { Visit(Origin, &Page); });
			}
			const auto Count = (Patch ? Member.DesiredCount - OldCount : 0) + NewCount;
			if (Count > MaximumGridBoxes || Count > MaximumWorldBoxes - (State.ReservedBoxes - Member.DesiredCount))
				return false;
			if (Patch) for (auto Origin : Replaced) Member.Desired.erase(Origin);
			else Member.Desired.clear();
			Member.Desired.merge(Changed);
			State.ReservedBoxes = State.ReservedBoxes - Member.DesiredCount + Count;
			Member.DesiredCount = Count;
			Member.TopologyCurrent = true;
			Member.TopologyActive = Active;
			return true;
		}

		PhysicsBodyDesc DescribeBox(const BlockGrid &Grid, const BlockGridBox &Box) {
			const glm::vec3 Min(Box[0], Box[1], Box[2]), Max(Box[3], Box[4], Box[5]);
			const auto Pose = Grid.GetCFrame();
			const float Size = Grid.GetCellSize();
			PhysicsBodyDesc Result;
			Result.Transform = CFrame(Pose.PointToWorldSpace((Min + Max) * (Size * 0.5f)), Pose.Rotation);
			Result.Shape = {PhysicsShapeKind::Box, (Max - Min) * Size};
			Result.Anchored = true;
			Result.CanCollide = Grid.GetCanCollide();
			Result.CanTouch = false;
			Result.CanQuery = Grid.GetCanQuery();
			Result.RequiresBlockGridQuery = true;
			return Result;
		}
	}

	void WorldRoot::AddBlockGrid(const std::shared_ptr<Instance> &Value) {
		auto Grid = std::dynamic_pointer_cast<BlockGrid>(Value);
		if (!Grid || ShuttingDownPhysics) return;
		if (!BlockGridState) BlockGridState = std::make_unique<BlockGridWorldState>();
		const auto Id = Grid->GetObjectId();
		const auto [It, Added] = BlockGridState->Members.try_emplace(Id, BlockGridParticipant{Grid});
		if (!Added) return;
		std::weak_ptr<WorldRoot> Owner = std::static_pointer_cast<WorldRoot>(shared_from_this());
		Grid->SetAdmission([Owner, Id](const blockgrid::Field::Candidate &Next, std::span<const blockgrid::Write>) {
			const auto World = Owner.lock();
			if (!World || !World->BlockGridState) return true;
			auto &State = *World->BlockGridState;
			const auto It = State.Members.find(Id);
			return It == State.Members.end() || PrepareTopology(State, It->second, Next);
		});
	}

	void WorldRoot::RemoveBlockGrid(const std::shared_ptr<Instance> &Value) {
		if (!BlockGridState || !Value) return;
		const auto It = BlockGridState->Members.find(Value->GetObjectId());
		if (It == BlockGridState->Members.end()) return;
		if (!Value->GetDestroyed() && !Value->IsDestroying()) It->second.Grid->SetAdmission({});
		BlockGridState->ReservedBoxes -= It->second.DesiredCount;
		for (const auto &[Box, Body] : It->second.Bodies) {
			if (Physics.IsBodyValid(Body) && !Physics.DestroyBody(Body).Succeeded())
				BlockGridState->Retiring.insert(Body);
			BlockGridState->Owners.erase(Body);
		}
		BlockGridState->Members.erase(It);
		if (BlockGridState->Members.empty() && BlockGridState->Retiring.empty()) BlockGridState.reset();
	}

	void WorldRoot::ForEachBlockGrid(const std::function<void(const BlockGrid &)> &Visit) const {
		if (BlockGridState)
			for (const auto &[Id, Member] : BlockGridState->Members) Visit(*Member.Grid);
	}

	bool WorldRoot::SyncBlockGridCollision() {
		if (!BlockGridState) return true;
		auto &State = *BlockGridState;
		std::erase_if(State.Retiring, [&](PhysicsBodyId Body) {
			return !Physics.IsBodyValid(Body) || Physics.DestroyBody(Body).Succeeded();
		});
		bool Ready = State.Retiring.empty();
		for (auto &[Id, Member] : State.Members) {
			const auto &Grid = *Member.Grid;
			const auto &Field = Grid.GetField();
			const auto Pose = Grid.GetCFrame();
			const bool ConfigChanged = Member.Pose.Position != Pose.Position || Member.Pose.Rotation != Pose.Rotation ||
				Member.CellSize != Grid.GetCellSize() || Member.Collidable != Grid.GetCanCollide() ||
				Member.Queryable != Grid.GetCanQuery();
			if (!ConfigChanged && Member.Revision == Field.GetRevision() && Member.Ready) continue;
			const bool Active = Grid.GetCanCollide() || Grid.GetCanQuery();
			if ((!Member.TopologyCurrent || Member.TopologyActive != Active) && !PrepareTopology(State, Member, Field)) {
				Member.Ready = false;
				Ready = false;
				continue;
			}
			std::set<BlockGridBox> Boxes;
			for (const auto &[Origin, Leaf] : Member.Desired) Boxes.insert(Leaf.begin(), Leaf.end());
			Member.Ready = true;
			for (auto It = Member.Bodies.begin(); It != Member.Bodies.end();) {
				if (Boxes.contains(It->first)) { ++It; continue; }
				const auto Body = It->second;
				if (Physics.IsBodyValid(Body) && !Physics.DestroyBody(Body).Succeeded()) {
					Member.Ready = false; ++It; continue;
				}
				State.Owners.erase(Body);
				It = Member.Bodies.erase(It);
			}
			for (const auto &Box : Boxes) {
				const auto Existing = Member.Bodies.find(Box);
				if (Existing != Member.Bodies.end()) {
					if (ConfigChanged && !Physics.UpdateBody(Existing->second, DescribeBox(Grid, Box)).Succeeded())
						Member.Ready = false;
					continue;
				}
				const auto Body = Physics.CreateBody(DescribeBox(Grid, Box));
				if (!Body.IsValid()) { Member.Ready = false; break; }
				Member.Bodies.emplace(Box, Body);
				State.Owners.emplace(Body, BlockGridCollider{Id, Box});
			}
			if (!Member.Ready) { Ready = false; continue; }
			Member.Revision = Field.GetRevision();
			Member.Pose = Pose;
			Member.CellSize = Grid.GetCellSize();
			Member.Collidable = Grid.GetCanCollide();
			Member.Queryable = Grid.GetCanQuery();
		}
		return Ready;
	}

	bool WorldRoot::AppendBlockGridFilter(ObjectId Object, std::vector<PhysicsBodyId> &Bodies) const {
		if (!BlockGridState) return true;
		const auto Member = BlockGridState->Members.find(Object);
		if (Member == BlockGridState->Members.end()) return true;
		if (Member->second.Bodies.size() > MaximumRaycastFilterBodies - Bodies.size()) return false;
		for (const auto &[Box, Body] : Member->second.Bodies) Bodies.push_back(Body);
		return true;
	}

	std::shared_ptr<BlockGrid> WorldRoot::ResolveBlockGrid(PhysicsBodyId Body) const {
		if (!BlockGridState) return {};
		const auto Owner = BlockGridState->Owners.find(Body);
		if (Owner == BlockGridState->Owners.end()) return {};
		return BlockGridState->Members.at(Owner->second.Grid).Grid;
	}

	std::optional<glm::ivec3> WorldRoot::ResolveBlockCell(PhysicsBodyId Body, glm::vec3 Position) const {
		const auto Grid = ResolveBlockGrid(Body);
		if (!Grid) return std::nullopt;
		const auto &Box = BlockGridState->Owners.at(Body).Bounds;
		const auto Local = Grid->GetCFrame().PointToObjectSpace(Position) / Grid->GetCellSize();
		if (!std::isfinite(Local.x) || !std::isfinite(Local.y) || !std::isfinite(Local.z)) return std::nullopt;
		const glm::vec3 Min(Box[0], Box[1], Box[2]), MaxCell(Box[3]-1, Box[4]-1, Box[5]-1);
		return glm::ivec3(glm::clamp(glm::floor(Local), Min, MaxCell));
	}
}

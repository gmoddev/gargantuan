#include "BlockGridWorldState.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "../physics/Box3DShapeQuery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace gargantuan {
	namespace {
		constexpr unsigned Side = blockgrid::LeafSide;
		constexpr std::size_t MaximumGridBoxes = MaximumRaycastFilterBodies;
		constexpr std::size_t MaximumWorldBoxes = MaximumPhysicsCollisionConsumers;

		std::uint8_t Participation(bool Collide,bool Query) {
			return static_cast<std::uint8_t>(unsigned(Collide)|(unsigned(Query)<<1));
		}
		template<class Snapshot>
		std::vector<BlockGridBox> LeafBoxes(blockgrid::Cell Origin,const blockgrid::Leaf& Leaf,const Snapshot& Cells,std::uint8_t GridMask) {
				std::vector<BlockGridBox> Boxes;
				const auto Uniform=Leaf.GetUniform();
				if (Leaf.GetOccupiedCount()==blockgrid::LeafVolume && (!Leaf.HasCustomCells() ||
					(Uniform && Cells.GetDefinition(Uniform->Definition).Geometry==blockgrid::Shape::Cube))) {
					const int Extent = static_cast<int>(Side);
					const auto& Spec=Cells.GetDefinition(Uniform?Uniform->Definition:blockgrid::Stone);
					const auto Flags=static_cast<std::uint8_t>(GridMask&Participation(Spec.CanCollide,Spec.Queryable));
					if (Flags) Boxes.push_back({{Origin.X,Origin.Y,Origin.Z,Origin.X+Extent,Origin.Y+Extent,Origin.Z+Extent},Flags});
					return Boxes;
				}
				std::array<std::uint8_t,blockgrid::LeafVolume> Filled{};
				const auto Index = [](unsigned X, unsigned Y, unsigned Z) { return (Z * Side + Y) * Side + X; };
				Leaf.ForEachOccupied([&](unsigned X, unsigned Y, unsigned Z, blockgrid::Block B) {
					const auto& Spec=Cells.GetDefinition(B.Definition);
					const auto Flags=static_cast<std::uint8_t>(GridMask&Participation(Spec.CanCollide,Spec.Queryable));
					if (!Flags) return;
					if (Spec.Geometry==blockgrid::Shape::Cube) { Filled[Index(X,Y,Z)]=Flags; return; }
					const auto& Data=blockgrid::GetGeometry(Spec.Geometry,B.Orientation);
					const int I=Origin.X+static_cast<int>(X), J=Origin.Y+static_cast<int>(Y), K=Origin.Z+static_cast<int>(Z);
					for (std::uint8_t Piece=0;Piece<Data.Collision.size();++Piece)
						Boxes.push_back({{I,J,K,I+1,J+1,K+1},Flags,Spec.Geometry,B.Orientation,Piece});
				});
				for (unsigned Z = 0; Z != Side; ++Z) {
					for (unsigned Y = 0; Y != Side; ++Y) {
						for (unsigned X = 0; X != Side; ++X) {
							const auto Flags=Filled[Index(X,Y,Z)];
							if (!Flags) continue;
							unsigned EndX = X + 1, EndY = Y + 1, EndZ = Z + 1;
							while (EndX < Side && Filled[Index(EndX,Y,Z)]==Flags) ++EndX;
							const auto FullRow = [&](unsigned Row, unsigned Plane) {
								for (unsigned I = X; I < EndX; ++I)
									if (Filled[Index(I,Row,Plane)]!=Flags) return false;
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
									for (unsigned I = X; I < EndX; ++I) Filled[Index(I,J,K)]=0;
							Boxes.push_back({{Origin.X + static_cast<int>(X), Origin.Y + static_cast<int>(Y),
								Origin.Z + static_cast<int>(Z), Origin.X + static_cast<int>(EndX),
								Origin.Y + static_cast<int>(EndY), Origin.Z + static_cast<int>(EndZ)},Flags});
						}
					}
				}
				return Boxes;
		}

		template <typename Snapshot>
		bool PrepareTopology(BlockGridWorldState &State, BlockGridParticipant &Member, const Snapshot &Cells) {
			const auto Mask=Participation(Member.Grid->GetCanCollide(),Member.Grid->GetCanQuery());
			const bool Patch = Mask && Member.TopologyCurrent && Member.TopologyMask==Mask &&
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
				auto Boxes = LeafBoxes(Origin,*Page,Cells,Mask);
				NewCount += Boxes.size();
				if (!Boxes.empty()) Changed.emplace(Origin,std::move(Boxes));
			};
			if (Mask) {
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
			Member.TopologyMask = Mask;
			return true;
		}

		PhysicsBodyDesc DescribeBox(const BlockGrid &Grid, const BlockGridBox &Box) {
			const auto& Bounds=Box.Cells;
			const glm::vec3 Min(Bounds[0], Bounds[1], Bounds[2]), Max(Bounds[3], Bounds[4], Bounds[5]);
			const auto Pose = Grid.GetCFrame();
			const float Size = Grid.GetCellSize();
			PhysicsBodyDesc Result;
			Result.Transform = CFrame(Pose.PointToWorldSpace((Min + Max) * (Size * 0.5f)), Pose.Rotation);
			Result.Shape = {PhysicsShapeKind::Box, (Max - Min) * Size};
			if (Box.Geometry!=blockgrid::Shape::Cube) {
				const auto& Piece=blockgrid::GetGeometry(Box.Geometry,Box.Orientation).Collision[Box.Piece];
				const auto V=[](const blockgrid::Point& P){return glm::vec3(P[0],P[1],P[2]);};
				const auto& B=blockgrid::Basis(Piece.Orientation);
				Result.Transform=CFrame(Pose.PointToWorldSpace((Min+V(Piece.Center))*Size),
					Pose.Rotation*glm::mat3(V(B[0]),V(B[1]),V(B[2])));
				Result.Shape={Piece.Kind==blockgrid::PieceKind::Wedge?PhysicsShapeKind::Wedge:PhysicsShapeKind::Box,V(Piece.Size)*Size};
			}
			Result.Anchored = true;
			Result.CanCollide = (Box.Participation&1)!=0;
			Result.CanTouch = false;
			Result.CanQuery = (Box.Participation&2)!=0;
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
			if (!ConfigChanged && Member.Revision == Field.GetGeometryRevision() && Member.Ready) continue;
			const auto Mask=Participation(Grid.GetCanCollide(),Grid.GetCanQuery());
			if ((!Member.TopologyCurrent || Member.TopologyMask != Mask) && !PrepareTopology(State, Member, Field)) {
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
			Member.Revision = Field.GetGeometryRevision();
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
		const auto &Box = BlockGridState->Owners.at(Body).Bounds.Cells;
		if (Box[3]==Box[0]+1 && Box[4]==Box[1]+1 && Box[5]==Box[2]+1)
			return glm::ivec3(Box[0],Box[1],Box[2]);
		const auto Local = Grid->GetCFrame().PointToObjectSpace(Position) / Grid->GetCellSize();
		if (!std::isfinite(Local.x) || !std::isfinite(Local.y) || !std::isfinite(Local.z)) return std::nullopt;
		const glm::vec3 Min(Box[0], Box[1], Box[2]), MaxCell(Box[3]-1, Box[4]-1, Box[5]-1);
		auto Cell=glm::floor(Local);
		for (unsigned Axis=0;Axis<3;++Axis) if (Local[Axis]==Cell[Axis] && Cell[Axis]>Min[Axis]) --Cell[Axis];
		return glm::ivec3(glm::clamp(Cell,Min,MaxCell));
	}
	PhysicsOperationStatus WorldRoot::VisitBlockOverlap(PhysicsBodyId Body,const PhysicsQueryShape& Query,std::size_t& Work,
		const std::function<bool(glm::ivec3)>& Emit) const {
		const auto Grid=ResolveBlockGrid(Body);
		if (!Grid) return PhysicsOperationStatus::BackendFailure;
		const auto& Box=BlockGridState->Owners.at(Body).Bounds.Cells;
		const glm::ivec3 Min(Box[0],Box[1],Box[2]), Max(Box[3],Box[4],Box[5]);
		// A detailed piece has one semantic owner. The backend already intersected its actual convex shape.
		if (Max-Min==glm::ivec3(1)) { (void)Emit(Min); return PhysicsOperationStatus::Success; }
		Box3DShapeQuery Proxy;
		if (!Proxy.Set(Query)) return PhysicsOperationStatus::InvalidDescription;
		const auto Frame=Grid->GetCFrame();
		const float Unit=Grid->GetCellSize();
		const auto Q=Box3DConversions::ToBox3(glm::quat_cast(Frame.Rotation));
		auto Status=PhysicsOperationStatus::Success;
		const auto Visit=[&](auto&& Self,glm::ivec3 Lo,glm::ivec3 Hi,bool Known)->bool {
			if (!Known) {
				if (!Work) { Status=PhysicsOperationStatus::ResourceLimit; return false; }
				--Work;
				const glm::vec3 Half=glm::vec3(Hi-Lo)*(.5f*Unit);
				const auto Center=Frame.PointToWorldSpace(glm::vec3(Hi+Lo)*(.5f*Unit));
				const auto Relative=Box3DConversions::ToBox3(Center-Query.Transform.Position);
				const auto Hull=b3MakeBoxHull(Half.x,Half.y,Half.z);
				if (!b3OverlapHull(&Hull.base,{Relative,Q},&Proxy.Proxy)) return true;
			}
			const auto Range=Hi-Lo;
			if (Range==glm::ivec3(1)) return Emit(Lo);
			unsigned Axis=0;
			if (Range.y>Range.x) Axis=1;
			if (Range.z>Range[Axis]) Axis=2;
			auto Left=Hi, Right=Lo;
			Left[Axis]=Right[Axis]=Lo[Axis]+Range[Axis]/2;
			return Self(Self,Lo,Left,false) && Self(Self,Right,Hi,false);
		};
		(void)Visit(Visit,Min,Max,true);
		return Status;
	}
}

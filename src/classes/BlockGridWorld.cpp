#include "BlockGridWorldState.hpp"
#include "gargantuan/blockgrid/Assets.hpp"
#include "gargantuan/blockgrid/Momentum.hpp"
#include "gargantuan/blockgrid/Transfer.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "../physics/Box3DShapeQuery.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <type_traits>

namespace gargantuan {
	namespace {
		constexpr unsigned Side = blockgrid::LeafSide;
		constexpr std::size_t MaximumGridBoxes = MaximumRaycastFilterBodies;
		constexpr std::size_t MaximumWorldBoxes = MaximumPhysicsCollisionConsumers;
		std::atomic_size_t LiveAssemblies{}; // Optional hierarchy fast path across independently owned Main runtimes.

		std::uint8_t Participation(bool Collide,bool Query) {
			return static_cast<std::uint8_t>(unsigned(Collide)|(unsigned(Query)<<1));
		}
		glm::mat3 Rotation(std::uint8_t Orientation) {
			const auto& R=blockgrid::Basis(Orientation);
			const auto V=[](const blockgrid::Point& P){return glm::vec3(P[0],P[1],P[2]);};
			return glm::mat3(V(R[0]),V(R[1]),V(R[2]));
		}
		bool MeshInRange(const BlockGrid& Grid,glm::vec3 Min,std::uint8_t Orientation,const PhysicsTriangleMesh& Mesh) {
			const auto& Pose=Grid.GetCFrame();
			const float Unit=Grid.GetCellSize();
			const auto Center=Pose.PointToWorldSpace((Min+.5f)*Unit);
			const float Length=glm::length(Center);
			if (Length+.867f*Unit<100'000.f) return true;
			if (!(Length<=100'000.f)) return false;
			const auto R=Pose.Rotation*Rotation(Orientation);
			return std::ranges::all_of(*Mesh.Positions,[&](auto P){return glm::length(Center+R*(P*Unit))<=100'000.f;});
		}
		std::uint8_t QueryProxyMask(const BlockGrid& Grid) {
			return Participation(Grid.GetCanCollide() && !Grid.IsDynamicAssembly(),Grid.GetCanQuery());
		}
		template<class Snapshot>
		std::shared_ptr<const std::vector<PhysicsConvexChild>> AssemblyHulls(const BlockGrid& Grid,const Snapshot& Source) {
			if (!Grid.GetCanCollide() || !Source.GetOccupiedCount() || Source.GetOccupiedCount()>MaximumPhysicsAssemblyHulls)
				throw std::length_error("[BlockGrid:Motion] A dynamic assembly requires 1 to 128 occupied cells with collision enabled");
			auto Result=std::make_shared<std::vector<PhysicsConvexChild>>();
			Result->reserve(Source.GetOccupiedCount());
			const auto V=[](const blockgrid::Point& P){return glm::vec3(P[0],P[1],P[2]);};
			const float Size=Grid.GetCellSize();
			Source.ForEachLeaf([&](blockgrid::Cell Base,const blockgrid::Leaf& Page) {
				Page.ForEachOccupied([&](unsigned X,unsigned Y,unsigned Z,blockgrid::Block B) {
					const auto& D=Source.GetDefinition(B.Definition);
					const glm::vec3 Min(Base.X+int(X),Base.Y+int(Y),Base.Z+int(Z));
					if (D.Geometry==blockgrid::Shape::Mesh || glm::any(glm::greaterThan(glm::abs(Min),glm::vec3(64.f))))
						throw std::invalid_argument("[BlockGrid:Motion] Dynamic cells require built-in convex geometry within 64 cells of the grid origin");
					if (!D.CanCollide) return;
					for (const auto& P:blockgrid::GetGeometry(D.Geometry,B.Orientation).Collision) {
						if (Result->size()>=MaximumPhysicsAssemblyHulls) throw std::length_error("[BlockGrid:Motion] Assembly hull capacity exhausted");
						Result->push_back({P.Kind==blockgrid::PieceKind::Box?PhysicsShapeKind::Box:PhysicsShapeKind::Wedge,
							CFrame((Min+V(P.Center))*Size,Rotation(P.Orientation)),V(P.Size)*Size});
					}
				});
			});
			if (Result->empty()) throw std::invalid_argument("[BlockGrid:Motion] Assembly has no physical hulls");
			return Result;
		}
		PhysicsBodyDesc AssemblyBody(const BlockGrid& Grid,const std::shared_ptr<const std::vector<PhysicsConvexChild>>& Hulls) {
			PhysicsBodyDesc D;
			D.Transform=Grid.GetCFrame(); D.Shape={PhysicsShapeKind::ConvexAssembly,glm::vec3(1),{},Hulls};
			D.Density=1.f; D.CanQuery=false; D.CanTouch=false; D.RequiresBlockGridQuery=true;
			return D;
		}
		template<class Snapshot>
		std::vector<BlockGridBox> LeafBoxes(blockgrid::Cell Origin,const blockgrid::Leaf& Leaf,const Snapshot& Cells,
			std::uint8_t GridMask,const BlockGrid& Grid,bool& Available) {
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
					const int I=Origin.X+static_cast<int>(X), J=Origin.Y+static_cast<int>(Y), K=Origin.Z+static_cast<int>(Z);
					if (Spec.Geometry==blockgrid::Shape::Mesh) {
						const auto M=Spec.Assets && Spec.Assets->Resolved?Spec.Assets->Resolved->QueryMesh:nullptr;
						if (!M || !MeshInRange(Grid,{I,J,K},B.Orientation,*M)) { Available=false; return; }
						Boxes.push_back({{I,J,K,I+1,J+1,K+1},Flags,Spec.Geometry,B.Orientation,0,M});
						return;
					}
					const auto& Data=blockgrid::GetGeometry(Spec.Geometry,B.Orientation);
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
		bool PrepareTopology(BlockGridWorldState &State, BlockGridParticipant &Member, const Snapshot &Cells,
			const std::function<bool()>& AdmitMotion={},std::optional<std::uint8_t> OverrideMask={}) {
			const auto Mask=OverrideMask.value_or(QueryProxyMask(*Member.Grid));
			const bool Patch = Mask && Member.TopologyCurrent && Member.TopologyMask==Mask &&
				std::is_same_v<Snapshot, blockgrid::Field::Candidate>;
			BlockGridLeafBoxes Changed;
			std::vector<blockgrid::Cell> Replaced;
			std::size_t OldCount = 0, NewCount = 0;
			bool Available=true;
			const auto Visit = [&](blockgrid::Cell Origin, const blockgrid::Leaf *Page) {
				if (!Available || NewCount > MaximumGridBoxes) return;
				if (Patch) {
					Replaced.push_back(Origin);
					if (auto It = Member.Desired.find(Origin); It != Member.Desired.end()) OldCount += It->second.size();
				}
				if (!Page) return;
				auto Boxes = LeafBoxes(Origin,*Page,Cells,Mask,*Member.Grid,Available);
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
			if (!Available || Count > MaximumGridBoxes || Count > MaximumWorldBoxes - (State.ReservedBoxes - Member.DesiredCount))
				return false;
			if (AdmitMotion && !AdmitMotion()) return false;
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
			if (Box.Geometry==blockgrid::Shape::Mesh) {
				Result.Transform.Rotation=Pose.Rotation*Rotation(Box.Orientation);
				Result.Shape={PhysicsShapeKind::StaticTriangleMesh,glm::vec3(Size),Box.Mesh};
			} else if (Box.Geometry!=blockgrid::Shape::Cube) {
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
	void ValidateBlockGridReparent(const Instance& Moving,const std::shared_ptr<Instance>& NewParent,
		std::span<const std::shared_ptr<Instance>> Subtree) {
		if (NewParent && BlockGrid::HasParkedTransfers()) {
			const auto* Parked=dynamic_cast<const BlockGrid*>(&Moving);
			const auto* ParentGrid=dynamic_cast<const BlockGrid*>(NewParent.get());
			if ((Parked && Parked->IsTransferParked()) || (ParentGrid && ParentGrid->IsTransferParked()))
				throw std::domain_error("[BlockGrid:History] Extracted shell is parked by Undo");
		}
		if (!LiveAssemblies.load(std::memory_order_relaxed) || !NewParent || Moving.IsDestroying()) return;
		const auto Old=WorldRoot::GetEffectiveWorld(Moving);
		if (!Old || Old==WorldRoot::GetEffectiveWorld(*NewParent)) return;
		for (const auto& Node:Subtree) {
			const auto* Grid=dynamic_cast<const BlockGrid*>(Node.get());
			if (!Grid || !Grid->IsDynamicAssembly() || WorldRoot::GetEffectiveWorld(*Grid)!=Old) continue;
			const Instance* Current=Grid;
			while (Current) {
				if (const auto* Scope=dynamic_cast<const SpatialDomainMember*>(Current); Scope && Scope->HasAssignedRegion()) break;
				if (dynamic_cast<const WorldRoot*>(Current) || dynamic_cast<const ViewportFrame*>(Current)) break;
				if (Current==&Moving) throw std::domain_error("[BlockGrid:Motion] Freeze the assembly before reparenting it to another world");
				const auto Parent=Current->GetParent();
				Current=Parent && *Parent?Parent->get():nullptr;
			}
		}
	}
	void ValidateBlockGridDomainChange(const Instance& Moving,const WorldRoot* Destination) {
		if (!LiveAssemblies.load(std::memory_order_relaxed)) return;
		const auto Old=WorldRoot::GetEffectiveWorld(Moving);
		if (!Old || Old.get()==Destination) return;
		Old->ForEachBlockGrid([&](const BlockGrid& G) {
			if (!G.IsDynamicAssembly()) return;
			for (const Instance* Current=&G;Current;) {
				if (Current==&Moving) throw std::domain_error("[BlockGrid:Motion] Freeze the assembly before changing its world");
				if (const auto* Scope=dynamic_cast<const SpatialDomainMember*>(Current);Scope && Scope->HasAssignedRegion()) break;
				if (dynamic_cast<const WorldRoot*>(Current) || dynamic_cast<const ViewportFrame*>(Current)) break;
				const auto Parent=Current->GetParent(); Current=Parent && *Parent?Parent->get():nullptr;
			}
		});
	}

	void WorldRoot::AddBlockGrid(const std::shared_ptr<Instance> &Value) {
		auto Grid = std::dynamic_pointer_cast<BlockGrid>(Value);
		if (!Grid || ShuttingDownPhysics || !IsSpatiallyAvailable()) return;
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
			if (It==State.Members.end()) return true;
			auto& Member=It->second;
			if (!Member.Assembly) return PrepareTopology(State,Member,Next);
			bool Dirty=false;
			Next.ForEachChangedLeaf([&](auto,const auto*){Dirty=true;});
			if (!Dirty) return PrepareTopology(State,Member,Next);
			const auto Updated=AssemblyHulls(*Member.Grid,Next);
			return PrepareTopology(State,Member,Next,[&] {
				if (!World->Physics.UpdateBody(Member.Assembly->Body,AssemblyBody(*Member.Grid,Updated)).Succeeded()) return false;
				Member.Assembly->Hulls=Updated; Member.Assembly->Pose=Member.Grid->GetCFrame(); return true;
			});
		});
		Grid->WorldOwnedAdmission=true;
	}

	std::function<void()> WorldRoot::PrepareBlockGridTransfer(BlockGrid& Source,const blockgrid::Field& SourceAfter,
		BlockGrid& Destination,const blockgrid::Field& DestinationAfter,blockgrid::TransferMotion Motion) {
		if (SteppingPhysics || ShuttingDownPhysics || !BlockGridState || !Source.WorldOwnedAdmission || !Destination.WorldOwnedAdmission)
			throw std::runtime_error("[BlockGrid:Admission] Both grids require a reservable world provider");
		auto& Live=*BlockGridState;
		const auto SourceMember=Live.Members.find(Source.GetObjectId()),DestinationMember=Live.Members.find(Destination.GetObjectId());
		if (SourceMember==Live.Members.end() || DestinationMember==Live.Members.end())
			throw std::domain_error("[BlockGrid:Admission] A transfer participant has left the collision domain");
		auto& A=SourceMember->second;
		auto& B=DestinationMember->second;
		const bool Moving=Motion!=blockgrid::TransferMotion::Static,Split=Motion==blockgrid::TransferMotion::Split;
		if (&A==&B || bool(A.Assembly)!=Moving || bool(B.Assembly)!=(Motion==blockgrid::TransferMotion::Merge))
			throw std::domain_error("[BlockGrid:Motion] Transfer requires an admitted body for each moving participant");
		struct Coverage { BlockGridParticipant A,B; std::size_t WorldCount{}; };
		auto Reserved=std::make_shared<Coverage>();
		BlockGridWorldState Scratch;
		Scratch.ReservedBoxes=Live.ReservedBoxes-A.DesiredCount-B.DesiredCount;
		const auto Reserve=[&](BlockGridParticipant& Original,BlockGridParticipant& Staged,const blockgrid::Field& After,bool Dynamic) {
			Staged.Grid=Original.Grid;
			Staged.Desired=Original.Desired; Staged.DesiredCount=Original.DesiredCount;
			Staged.TopologyMask=Original.TopologyMask; Staged.TopologyCurrent=Original.TopologyCurrent;
			Scratch.ReservedBoxes+=Original.DesiredCount;
			const auto Mask=Participation(Original.Grid->GetCanCollide() && !Dynamic,Original.Grid->GetCanQuery());
			if (!After.AdmitTransferFrom(Original.Grid->GetField(),[&](const auto& Next,auto){return PrepareTopology(Scratch,Staged,Next,{},Mask);}))
				throw std::length_error("[BlockGrid:Admission] Transfer exceeds available collision coverage");
		};
		Reserve(A,Reserved->A,SourceAfter,Split); Reserve(B,Reserved->B,DestinationAfter,Moving);
		Reserved->WorldCount=Scratch.ReservedBoxes;
		auto Momentum=Moving?PrepareGridMomentum(A,SourceAfter,B,DestinationAfter,Motion):std::function<void()>{};
		return [&Live,&A,&B,Reserved=std::move(Reserved),Momentum=std::move(Momentum)]() noexcept {
			const auto Install=[](BlockGridParticipant& To,BlockGridParticipant& From) noexcept {
				To.Desired.swap(From.Desired); To.DesiredCount=From.DesiredCount;
				To.TopologyMask=From.TopologyMask; To.TopologyCurrent=true;
			};
			Install(A,Reserved->A); Install(B,Reserved->B); Live.ReservedBoxes=Reserved->WorldCount;
			if (Momentum) Momentum();
		};
	}

	std::function<void()> WorldRoot::PrepareGridMomentum(BlockGridParticipant& A,const blockgrid::Field& SourceAfter,
		BlockGridParticipant& B,const blockgrid::Field& DestinationAfter,blockgrid::TransferMotion Motion) {
		using namespace blockgrid;
		const bool Split=Motion==TransferMotion::Split;
		auto& Live=*BlockGridState;
		if ((Split && Live.Assemblies.size()>=64) || A.Grid->AssemblyRevision==std::numeric_limits<std::uint64_t>::max() ||
			B.Grid->AssemblyRevision==std::numeric_limits<std::uint64_t>::max())
			throw std::length_error("[BlockGrid:Motion] Moving transfer capacity is exhausted");
		const std::array<PhysicsBodyId,2> Old{A.Assembly->Body,B.Assembly?B.Assembly->Body:PhysicsBodyId{}};
		if (!Physics.PrepareAssemblyReplacement(std::span(Old).first(Split?1:2),Split?2:1).Succeeded())
			throw std::domain_error("[BlockGrid:Motion] Native constraints or stale bodies prevent replacement");
		struct Candidate {
			PhysicsWorld& Physics;
			std::array<std::unique_ptr<BlockGridAssembly>,2> Bodies;
			std::array<std::unique_ptr<AssemblyProjection>,2> Projections;
			decltype(BlockGridWorldState::Assemblies) Owners;
			bool Committed{};
			explicit Candidate(PhysicsWorld& Value):Physics(Value) {}
			~Candidate() {
				if (!Committed) for (const auto& B:Bodies)
					if (B && B->Body.IsValid()) (void)Physics.DestroyBody(B->Body);
			}
		};
		auto Plan=std::make_shared<Candidate>(Physics);
		Plan->Owners=Live.Assemblies;
		for (const auto Id:Old) if (Id.IsValid()) Plan->Owners.erase(Id);
		struct Sample { RigidMass Mass; RigidVelocity Velocity; };
		const auto SampleBody=[&](PhysicsBodyId Id) {
			const auto Body=Physics.GetBodyState(Id); const auto Mass=Physics.GetMassProperties(Id);
			if (!Body || !Mass) throw std::runtime_error("[BlockGrid:Motion] Native mass and velocity must be available");
			const auto R=glm::dmat3(Body->Transform.Rotation);
			return Sample{{Mass->Mass,glm::dvec3(Body->Transform.Position)+R*glm::dvec3(Mass->LocalCenter),
				R*glm::dmat3(Mass->LocalInertia)*glm::transpose(R)},
				{glm::dvec3(Body->LinearVelocity),glm::dvec3(Body->AngularVelocity)}};
		};
		const auto Build=[&](unsigned Index,const BlockGrid& Grid,const Field& Cells) {
			auto& C=Plan->Bodies[Index]; C=std::make_unique<BlockGridAssembly>();
			C->Pose=Grid.GetCFrame(); C->Hulls=AssemblyHulls(Grid,Cells);
			C->Body=Physics.CreateBody(AssemblyBody(Grid,C->Hulls));
			if (!C->Body.IsValid()) throw std::runtime_error("[BlockGrid:Motion] Could not stage replacement convex assembly");
			Plan->Owners.emplace(C->Body,Grid.GetObjectId());
			return SampleBody(C->Body).Mass;
		};
		const auto SetVelocity=[&](unsigned Index,const BlockGrid& Grid,RigidVelocity V) {
			const auto Id=Plan->Bodies[Index]->Body;
			if (!Physics.SetLinearVelocity(Id,glm::vec3(V.Linear)).Succeeded() || !Physics.SetAngularVelocity(Id,glm::vec3(V.Angular)).Succeeded())
				throw std::runtime_error("[BlockGrid:Motion] Native replacement velocity was unavailable");
			const auto State=Physics.GetBodyState(Id); const auto M=Physics.GetMassProperties(Id);
			if (!State || !M) throw std::runtime_error("[BlockGrid:Motion] Native replacement receipt was unavailable");
			auto& P=Plan->Projections[Index]; P=std::make_unique<AssemblyProjection>();
			P->Revision=Grid.AssemblyRevision+1; P->Dynamic=true; P->Pose=State->Transform; P->Mass=M->Mass;
			P->LinearVelocity=State->LinearVelocity; P->AngularVelocity=State->AngularVelocity;
			P->LocalCenterOfMass=M->LocalCenter; P->LocalInertia=M->LocalInertia;
			if (!ValidAssemblyProjection(*P)) throw std::domain_error("[BlockGrid:Motion] Replacement motion is outside supported bounds");
		};
		const auto First=SampleBody(Old[0]);
		if (Split) {
			const auto Left=Build(0,*A.Grid,SourceAfter),Right=Build(1,*B.Grid,DestinationAfter);
			const auto Velocities=SplitMomentum(First.Mass,First.Velocity,Left,Right);
			SetVelocity(0,*A.Grid,Velocities[0]); SetVelocity(1,*B.Grid,Velocities[1]);
		} else {
			if (SourceAfter.GetOccupiedCount()) throw std::domain_error("[BlockGrid:Motion] Moving merge must empty its donor");
			const auto Second=SampleBody(Old[1]); const auto Merged=Build(1,*B.Grid,DestinationAfter);
			SetVelocity(1,*B.Grid,MergeMomentum(First.Mass,First.Velocity,Second.Mass,Second.Velocity,Merged));
		}
		return [this,&Live,&A,&B,Old,Split,Plan=std::move(Plan)]() noexcept {
			// The freeze on Main spans admission and commit. All native IDs were checked and are joint-free.
			for (auto Id:Old) if (Id.IsValid()) (void)Physics.DestroyBody(Id);
			Live.Assemblies.swap(Plan->Owners);
			const auto Install=[&](BlockGridParticipant& Member,unsigned I) noexcept {
				Member.Assembly=std::move(Plan->Bodies[I]); Member.Grid->DynamicAssembly=bool(Member.Assembly);
				Member.Grid->AssemblyState=std::move(Plan->Projections[I]); ++Member.Grid->AssemblyRevision;
			};
			Install(A,0); Install(B,1);
			if (Split) ++LiveAssemblies; else --LiveAssemblies;
			Plan->Committed=true;
		};
	}

	void WorldRoot::RemoveBlockGrid(const std::shared_ptr<Instance> &Value) {
		if (!BlockGridState || !Value) return;
		const auto It = BlockGridState->Members.find(Value->GetObjectId());
		if (It == BlockGridState->Members.end()) return;
		if (!Value->GetDestroyed() && !Value->IsDestroying()) It->second.Grid->SetAdmission({});
		BlockGridState->ReservedBoxes -= It->second.DesiredCount;
		const auto Leaving=It->second.Grid;
		const bool WasDynamic=bool(It->second.Assembly);
		if (const auto& Assembly=It->second.Assembly) {
			--LiveAssemblies;
			BlockGridState->Assemblies.erase(Assembly->Body);
			if (Physics.IsBodyValid(Assembly->Body) && !Physics.DestroyBody(Assembly->Body).Succeeded())
				BlockGridState->Retiring.insert(Assembly->Body);
			Leaving->DynamicAssembly=false;
			Leaving->RetireAssemblyState();
		}
		for (const auto &[Box, Body] : It->second.Bodies) {
			if (Physics.IsBodyValid(Body) && !Physics.DestroyBody(Body).Succeeded())
				BlockGridState->Retiring.insert(Body);
			BlockGridState->Owners.erase(Body);
		}
		BlockGridState->Members.erase(It);
		if (BlockGridState->Members.empty() && BlockGridState->Retiring.empty()) BlockGridState.reset();
		if (WasDynamic && !ShuttingDownPhysics && !Leaving->GetDestroyed() && !Leaving->IsDestroying()) Leaving->NotifyConfiguration("MotionMode");
	}

	void WorldRoot::ForEachBlockGrid(const std::function<void(const BlockGrid &)> &Visit) const {
		if (BlockGridState)
			for (const auto &[Id, Member] : BlockGridState->Members) Visit(*Member.Grid);
	}
	bool WorldRoot::SetGridMotion(BlockGrid& Grid,bool Dynamic) {
		if (SteppingPhysics || ShuttingDownPhysics || !BlockGridState) throw std::runtime_error("[BlockGrid:Motion] Simulation world is unavailable");
		const auto Id=Grid.GetObjectId();
		const auto It=BlockGridState->Members.find(Id);
		if (It==BlockGridState->Members.end() || !SyncBlockGridCollision()) throw std::runtime_error("[BlockGrid:Motion] Grid collision is unavailable");
		auto& Member=It->second;
		if (Dynamic==Grid.DynamicAssembly) return false;
		if (Dynamic) {
			if (BlockGridState->Assemblies.size()>=64) throw std::length_error("[BlockGrid:Motion] World assembly limit exhausted");
			auto Next=std::make_unique<BlockGridAssembly>();
			Next->Hulls=AssemblyHulls(Grid,Grid.GetField()); Next->Pose=Grid.GetCFrame();
			Next->Body=Physics.CreateBody(AssemblyBody(Grid,Next->Hulls));
			if (!Next->Body.IsValid()) throw std::runtime_error("[BlockGrid:Motion] Dynamic body admission failed");
			try { BlockGridState->Assemblies.emplace(Next->Body,Id); }
			catch (...) { (void)Physics.DestroyBody(Next->Body); throw; }
			Member.Assembly=std::move(Next);
		}
		const auto Retire=[&] {
			const auto Body=Member.Assembly->Body;
			if (!Physics.DestroyBody(Body).Succeeded()) return false;
			BlockGridState->Assemblies.erase(Body); Member.Assembly.reset(); return true;
		};
		Grid.DynamicAssembly=Dynamic;
		Member.TopologyCurrent=false;
		try {
			if (!SyncBlockGridCollision() || (!Dynamic && !Retire())) throw std::runtime_error("[BlockGrid:Motion] Collision transition failed");
		} catch (...) {
			Grid.DynamicAssembly=!Dynamic;
			if (Dynamic && !Retire()) {
				const auto Body=Member.Assembly->Body;
				BlockGridState->Assemblies.erase(Body); BlockGridState->Retiring.insert(Body); Member.Assembly.reset();
			}
			Member.TopologyCurrent=false;
			(void)SyncBlockGridCollision();
			throw;
		}
		if (Dynamic) { ++LiveAssemblies; RefreshGridAssembly(Grid); }
		else { --LiveAssemblies; Grid.RetireAssemblyState(); }
		return true;
	}
	std::pair<PhysicsBodyState,PhysicsMassProperties> WorldRoot::GetGridAssemblyState(const BlockGrid& Grid) {
		if (!BlockGridState || !Grid.IsDynamicAssembly() || !SyncBlockGridCollision())
			throw std::runtime_error("[BlockGrid:Motion] Dynamic body unavailable");
		const auto& Assembly=BlockGridState->Members.at(Grid.GetObjectId()).Assembly;
		const auto Body=Physics.GetBodyState(Assembly->Body);
		const auto Mass=Physics.GetMassProperties(Assembly->Body);
		if (!Body || !Mass) throw std::runtime_error("[BlockGrid:Motion] Native mass properties unavailable");
		return {*Body,*Mass};
	}
	bool WorldRoot::SetGridVelocity(BlockGrid& Grid,glm::vec3 Linear,glm::vec3 Angular) {
		if (SteppingPhysics || !(glm::length(Linear)<=512.f) || !(glm::length(Angular)<=64.f))
			throw std::invalid_argument("[BlockGrid:Motion] Velocity must be finite and within the construction limits");
		const auto [State,Mass]=GetGridAssemblyState(Grid);
		if (State.LinearVelocity==Linear && State.AngularVelocity==Angular) return false;
		const auto Body=BlockGridState->Members.at(Grid.GetObjectId()).Assembly->Body;
		if (!Physics.SetLinearVelocity(Body,Linear).Succeeded()) throw std::runtime_error("[BlockGrid:Motion] Native velocity update failed");
		if (!Physics.SetAngularVelocity(Body,Angular).Succeeded()) {
			(void)Physics.SetLinearVelocity(Body,State.LinearVelocity);
			throw std::runtime_error("[BlockGrid:Motion] Native angular velocity update failed");
		}
		RefreshGridAssembly(Grid);
		return true;
	}
	void WorldRoot::RefreshGridAssembly(BlockGrid& Grid) {
		const auto& A=BlockGridState->Members.at(Grid.GetObjectId()).Assembly;
		const auto S=Physics.GetBodyState(A->Body);
		const auto M=Physics.GetMassProperties(A->Body);
		if (!S || !M) throw std::runtime_error("[BlockGrid:Motion] Missing native assembly state");
		A->Pose=S->Transform;
		blockgrid::AssemblyProjection P;
		P.Dynamic=true; P.Pose=S->Transform; P.LinearVelocity=S->LinearVelocity; P.AngularVelocity=S->AngularVelocity;
		P.Mass=M->Mass; P.LocalCenterOfMass=M->LocalCenter; P.LocalInertia=M->LocalInertia;
		Grid.PublishAssemblyState(P);
		if (Grid.Transform.Position==P.Pose.Position && Grid.Transform.Rotation==P.Pose.Rotation) return;
		Grid.Transform=P.Pose;
		Grid.NotifyPropertyCommitted("CFrame");
	}
	void WorldRoot::SetGridPose(BlockGrid& Grid,const CFrame& Pose) {
		if (!BlockGridState || SteppingPhysics) throw std::runtime_error("[BlockGrid:Motion] Cannot move the assembly during a physics step");
		const auto& A=BlockGridState->Members.at(Grid.GetObjectId()).Assembly;
		if (!A) throw std::runtime_error("[BlockGrid:Motion] Missing native body");
		auto D=AssemblyBody(Grid,A->Hulls); D.Transform=Pose;
		if (!Physics.UpdateBody(A->Body,D).Succeeded()) throw std::runtime_error("[BlockGrid:Motion] Body pose update failed");
		A->Pose=Pose;
		auto P=*Grid.GetAssemblyProjection(); P.Pose=Pose; Grid.PublishAssemblyState(P);
	}
	void WorldRoot::ReadGridMotion() {
		// A property observer may remove another assembly. Pin identities before notifying any observer.
		std::array<ObjectId,64> Members{};
		std::size_t Count=0;
		for (const auto& [Body,Id]:BlockGridState->Assemblies) Members[Count++]=Id;
		for (std::size_t I=0;I<Count && BlockGridState;++I) {
			const auto It=BlockGridState->Members.find(Members[I]);
			if (It!=BlockGridState->Members.end() && It->second.Assembly) {
				const auto Grid=It->second.Grid;
				RefreshGridAssembly(*Grid);
			}
		}
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
			if (!Grid.EnsureAssetBindings()) { Member.Ready=false; Member.TopologyCurrent=false; Ready=false; continue; }
			if (Member.AssetRevision!=Grid.GetAssetPublicationRevision()) Member.TopologyCurrent=false;
			const auto &Field = Grid.GetField();
			const auto Pose = Grid.GetCFrame();
			const bool ConfigChanged = Member.Pose.Position != Pose.Position || Member.Pose.Rotation != Pose.Rotation ||
				Member.CellSize != Grid.GetCellSize() || Member.Collidable != Grid.GetCanCollide() ||
				Member.Queryable != Grid.GetCanQuery();
			if (Member.Assembly && Grid.IsDynamicAssembly() && (Member.Assembly->Pose.Position!=Pose.Position || Member.Assembly->Pose.Rotation!=Pose.Rotation)) {
				if (!Physics.UpdateBody(Member.Assembly->Body,AssemblyBody(Grid,Member.Assembly->Hulls)).Succeeded()) {
					Member.Ready=false; Ready=false; continue;
				}
				Member.Assembly->Pose=Pose;
			}
			const auto Mask=QueryProxyMask(Grid);
			if (!ConfigChanged && Member.TopologyCurrent && Member.TopologyMask==Mask && Member.Revision == Field.GetGeometryRevision() && Member.Ready) continue;
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
			Member.AssetRevision=Grid.GetAssetPublicationRevision();
			Member.Revision = Field.GetGeometryRevision();
			Member.Pose = Pose;
			Member.CellSize = Grid.GetCellSize();
			Member.Collidable = Grid.GetCanCollide();
			Member.Queryable = Grid.GetCanQuery();
		}
		return Ready;
	}

	bool WorldRoot::VerifyReplicaGridInstallation(const BlockGrid &Grid) {
		if (!Grid.IsReplicaMaterialization() || !Grid.HasReplicaField() || !BlockGridState || !Physics.IsValid() ||
			ShuttingDownPhysics || SteppingPhysics || GetEffectiveWorld(Grid).get() != this) return false;
		(void)SyncBlockGridCollision();
		const auto It = BlockGridState->Members.find(Grid.GetObjectId());
		if (It == BlockGridState->Members.end() || !BlockGridState->Retiring.empty()) return false;
		const auto &Member = It->second;
		if (!Member.Ready || !Member.TopologyCurrent || Member.Assembly || Member.Grid.get() != &Grid ||
			Member.Revision != Grid.GetField().GetGeometryRevision() || Member.AssetRevision != Grid.GetAssetPublicationRevision() ||
			Member.DesiredCount != Member.Bodies.size()) return false;
		for (const auto &[Piece, Body] : Member.Bodies) {
			const auto Found = BlockGridState->Owners.find(Body);
			if (!Physics.IsBodyValid(Body) || Found == BlockGridState->Owners.end() ||
				Found->second.Grid != Grid.GetObjectId() || Found->second.Bounds != Piece) return false;
		}
		return true;
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

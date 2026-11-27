#include "gargantuan/classes/WorldRoot.hpp"

#include "BlockGridWorldState.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/terrain/Field.hpp"
#include "terrain/PhysicsStage.hpp"

#include <algorithm>
#include <cmath>

namespace gargantuan {
	namespace {
		constexpr std::size_t MaximumInspectedColliders = 16'384;
		constexpr std::size_t MaximumDestinationProviders = 1'024;

		bool ValidBounds(const SpatialDestinationBounds &Bounds) {
			for (int I = 0; I != 3; ++I)
				if (!std::isfinite(Bounds.Minimum[I]) || !std::isfinite(Bounds.Maximum[I]) ||
					Bounds.Minimum[I] > Bounds.Maximum[I]) return false;
			return true;
		}

		bool Intersects(const SpatialDestinationBounds &Bounds, glm::dvec3 Minimum, glm::dvec3 Maximum) {
			return glm::all(glm::lessThanEqual(Minimum, glm::dvec3(Bounds.Maximum))) &&
				glm::all(glm::greaterThanEqual(Maximum, glm::dvec3(Bounds.Minimum)));
		}

		bool OverlapsBody(const SpatialDestinationBounds &Bounds, const CFrame &Pose, glm::vec3 Size) {
			const auto Half = glm::abs(glm::dvec3(Size)) * 0.5;
			const auto Extents = glm::abs(glm::dvec3(Pose.Rotation[0])) * Half.x +
				glm::abs(glm::dvec3(Pose.Rotation[1])) * Half.y + glm::abs(glm::dvec3(Pose.Rotation[2])) * Half.z;
			return Intersects(Bounds, glm::dvec3(Pose.Position) - Extents, glm::dvec3(Pose.Position) + Extents);
		}
	}

	std::expected<SpatialDestinationReceipt, PhysicsOperationStatus> WorldRoot::PrepareSpatialDestination(
		const SpatialDestinationBounds &Bounds, std::span<const ObjectId> RequiredOwners) {
		using enum PhysicsOperationStatus;
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main || !ValidBounds(Bounds)) return std::unexpected(InvalidDescription);
		const auto Realm = SpatialRegion::FindAttachedWorld(*this);
		if (!Realm || !SpatialActivation || ShuttingDownPhysics || SteppingPhysics || !Physics.IsValid())
			return std::unexpected(InvalidId);
		if (RequiredOwners.size() > MaximumDestinationProviders || TrackedParts.size() > MaximumInspectedColliders)
			return std::unexpected(ResourceLimit);
		SpatialDestinationReceipt Result{.Realm = Realm->GetObjectId(), .Domain = GetObjectId(),
			.Activation = SpatialActivation, .Bounds = Bounds, .RequiredOwners = {RequiredOwners.begin(), RequiredOwners.end()}};
		std::ranges::sort(Result.RequiredOwners);
		if (std::ranges::any_of(Result.RequiredOwners, [](ObjectId Id) { return !Id.IsValid(); }) ||
			std::adjacent_find(Result.RequiredOwners.begin(), Result.RequiredOwners.end()) != Result.RequiredOwners.end())
			return std::unexpected(InvalidDescription);
		const auto Required = [&](ObjectId Id) { return std::ranges::binary_search(Result.RequiredOwners, Id); };

		ApplyPendingPhysicsChanges();
		if (!SyncBlockGridCollision()) return std::unexpected(ResourceLimit);
		std::size_t Inspected = TrackedParts.size();
		for (const auto &[Id, Weak] : TrackedParts) {
			const auto Part = Weak.lock();
			if (!Part || Part->GetDestroyed() || Part->IsDestroying() || GetEffectiveWorld(*Part).get() != this)
				return std::unexpected(InvalidId);
			if (!Part->GetCanCollide() && !Required(Id)) continue;
			// A custom mesh may extend beyond Size. Keep its owner conservatively until an exact provider bound is available.
			const bool Custom = bool(std::dynamic_pointer_cast<MeshPart>(Part));
			if (!Required(Id) && !Custom && !OverlapsBody(Bounds, Part->GetCFrame(), Part->GetSize())) continue;
			const auto Handle = PartBodies.find(Id);
			if (Handle == PartBodies.end() || FailedMeshUpdatesLogged.contains(Id)) return std::unexpected(BackendFailure);
			const auto State = Physics.GetBodyState(Handle->second);
			if (!State || State->Description.CanCollide != Part->GetCanCollide()) return std::unexpected(BackendFailure);
			const auto &Desc = State->Description;
			SpatialProviderReceipt Proof{.Owner = Id, .Provider = SpatialCollisionProvider::Rigid,
				.Pose = State->Transform, .Size = Desc.Shape.Size, .Shape = Desc.Shape.Kind,
				.CollisionGroup = Desc.CollisionGroup, .CollisionMask = Desc.CollisionMask,
				.AuthoritativeEmpty = !Desc.CanCollide};
			if (Desc.Shape.Mesh) {
				Proof.Resource = Desc.Shape.Mesh->ResourceId;
				Proof.ContentRevision = Desc.Shape.Mesh->ContentRevision;
			}
			Proof.Installed.push_back({Handle->second, {}, Proof.ContentRevision});
			Result.Providers.push_back(std::move(Proof));
			if (Result.Providers.size() > MaximumDestinationProviders) return std::unexpected(ResourceLimit);
		}

		const auto Owner = GetOwnedTerrain();
		if (!Owner) return std::unexpected(InvalidId);
		const auto &Field = Owner->GetField();
		const auto *Installed = GetTerrainPhysics();
		const bool Empty = Field.GetStats().MaterializedPages == 0;
		if ((!Empty && !Installed) || (Installed && (Installed->IsStaging() || !Installed->IsHealthy())))
			return std::unexpected(BackendFailure);
		const auto Center = glm::vec3((glm::dvec3(Bounds.Minimum) + glm::dvec3(Bounds.Maximum)) * .5);
		const auto Extents = glm::vec3((glm::dvec3(Bounds.Maximum) - glm::dvec3(Bounds.Minimum)) * .5);
		if (!HasTerrainQueryCoverage(Center, Center, Extents)) return std::unexpected(ResourceLimit);
		SpatialProviderReceipt TerrainProof{.Owner = Owner->GetObjectId(), .Provider = SpatialCollisionProvider::Terrain,
			.Lifetime = Field.GetLifetime(), .ContentRevision = Field.GetRevision(),
			.Size = glm::vec3(Field.GetGrid().GetSpacing()), .AuthoritativeEmpty = Empty && !TerrainReplicaCoverageRequired};
		if (Installed) {
			if (Installed->GetBodyCount() > MaximumInspectedColliders - Inspected) return std::unexpected(ResourceLimit);
			Inspected += Installed->GetBodyCount();
			std::vector<PhysicsBodyId> Bodies;
			Installed->AppendBodies(Bodies);
			for (const auto Body : Bodies) {
				const auto Collider = Installed->FindOwner(Body);
				if (!Collider || !Physics.IsBodyValid(Body)) return std::unexpected(BackendFailure);
				const auto &Key = Collider->Key;
				const double Spacing = Field.GetGrid().GetSpacing();
				const glm::dvec3 Min = glm::dvec3(Key.X, Key.Y, Key.Z) * (Spacing * terrain::ChunkSide);
				if (Intersects(Bounds, Min - Spacing, Min + Spacing * (terrain::ChunkSide + 1)))
					TerrainProof.Installed.push_back({Body, {Key.X, Key.Y, Key.Z}, Collider->SurfaceRevision});
			}
			std::ranges::sort(TerrainProof.Installed, {}, &SpatialInstalledCollider::Body);
		}
		Result.Providers.push_back(std::move(TerrainProof));
		if (BlockGridState) for (const auto &[Id, Member] : BlockGridState->Members) {
			const auto &Grid = *Member.Grid;
			if (!Grid.GetCanCollide() && !Required(Id)) continue;
			if (!Member.Ready || Member.Revision != Grid.GetField().GetGeometryRevision()) return std::unexpected(BackendFailure);
			if (Member.Bodies.size() > MaximumInspectedColliders - Inspected) return std::unexpected(ResourceLimit);
			Inspected += Member.Bodies.size();
			SpatialProviderReceipt Proof{.Owner = Id, .Provider = SpatialCollisionProvider::BlockGrid,
				.ContentRevision = *Member.Revision, .Pose = Member.Pose, .Size = glm::vec3(Member.CellSize),
				.AuthoritativeEmpty = !Member.Collidable || Grid.GetField().GetOccupiedCount() == 0};
			bool HasPhysicalPiece = false;
			if (Member.Assembly) {
				const auto &Assembly = *Member.Assembly;
				const auto Native = Physics.GetBodyState(Assembly.Body);
				if (!Native || !Native->Description.CanCollide || Native->Description.Shape.Kind != PhysicsShapeKind::ConvexAssembly ||
					!Assembly.Hulls || Assembly.Hulls->empty() || Native->Description.Shape.Hulls != Assembly.Hulls)
					return std::unexpected(BackendFailure);
				if (Assembly.Hulls->size() > MaximumInspectedColliders - Inspected) return std::unexpected(ResourceLimit);
				Inspected += Assembly.Hulls->size();
				HasPhysicalPiece = true;
				Proof.Shape = PhysicsShapeKind::ConvexAssembly;
				Proof.Pose = Native->Transform;
				for (std::size_t I = 0; I < Assembly.Hulls->size(); ++I) {
					const auto &Hull = (*Assembly.Hulls)[I];
					if (OverlapsBody(Bounds, Native->Transform * Hull.Local, Hull.Size))
						Proof.Installed.push_back({Assembly.Body, {}, *Member.Revision,
							static_cast<std::uint8_t>(Hull.Kind), 0, static_cast<std::uint8_t>(I)});
				}
			}
			for (const auto &[Piece, Body] : Member.Bodies) {
				if ((Piece.Participation & 1u) == 0) continue;
				HasPhysicalPiece = true;
				const auto Native = Physics.GetBodyState(Body);
				if (!Native || !Native->Description.CanCollide) return std::unexpected(BackendFailure);
				if (!OverlapsBody(Bounds, Native->Transform, Native->Description.Shape.Size)) continue;
				const auto &Cells = Piece.Cells;
				Proof.Installed.push_back({Body, {Cells[0], Cells[1], Cells[2]}, *Member.Revision,
					static_cast<std::uint8_t>(Piece.Geometry), Piece.Orientation, Piece.Piece});
			}
			Proof.AuthoritativeEmpty = !HasPhysicalPiece;
			if (!Proof.Installed.empty() || Required(Id)) {
				Proof.Resource = Grid.GetBlockRevision();
				Result.Providers.push_back(std::move(Proof));
			}
			if (Result.Providers.size() > MaximumDestinationProviders) return std::unexpected(ResourceLimit);
		}
		if (Result.Providers.size() > MaximumDestinationProviders) return std::unexpected(ResourceLimit);
		std::ranges::sort(Result.Providers, {}, &SpatialProviderReceipt::Owner);
		for (const auto Id : Result.RequiredOwners)
			if (!std::ranges::binary_search(Result.Providers, Id, {}, &SpatialProviderReceipt::Owner))
				return std::unexpected(InvalidId);
		return Result;
	}

	PhysicsOperationResult WorldRoot::CommitSpatialTransfer(const std::shared_ptr<BasePart> &Member,
		const SpatialMovementRequest &Movement, const SpatialDestinationReceipt &Receipt, const SpatialMovementLease &Admission) {
		using enum PhysicsOperationStatus;
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main || !Member || Member->GetDestroyed() ||
			Member->IsDestroying() || Member->GetObjectId() != Movement.Member || !dynamic_cast<Part *>(Member.get()))
			return {InvalidId, "[Spatial:Transfer] Expected a live ordinary rigid Part"};
		const auto Source = GetEffectiveWorld(*Member);
		const auto SourceRealm = SpatialRegion::FindAttachedWorld(*Member);
		if (!Source || !SourceRealm || SourceRealm->GetObjectId() != Receipt.Realm || Movement.SourceDomain != Source->GetObjectId() ||
			Movement.SourceActivation != Source->SpatialActivation || Source->SteppingPhysics || SteppingPhysics)
			return {InvalidId, "[Spatial:Transfer] The source domain or safe point changed"};
		if (const auto Protection=SourceRealm->GetSpatialTransferProtection(); Protection && Protection->RequiresGrant(*Member,*this,Receipt.Bounds) &&
			!Protection->Reservations.Admits(Admission,Movement,Receipt))
			return {InvalidDescription,"[Spatial:Transfer] Session movement requires the issued bilateral reservation"};
		const auto Original = Member->GetCFrame();
		if (Original.Position != Movement.SourcePose.Position || Original.Rotation != Movement.SourcePose.Rotation)
			return {InvalidId, "[Spatial:Transfer] The source pose changed"};
		const auto &Pose = Movement.DestinationPose;
		for (int I = 0; I != 3; ++I) {
			if (!std::isfinite(Pose.Position[I])) return {InvalidDescription, "[Spatial:Transfer] Invalid destination position"};
			for (int J = 0; J != 3; ++J) {
				const auto Dot = glm::dot(Pose.Rotation[I], Pose.Rotation[J]);
				if (!std::isfinite(Dot) || std::abs(Dot - (I == J ? 1.f : 0.f)) > 1.e-4f)
					return {InvalidDescription, "[Spatial:Transfer] Expected a rigid destination rotation"};
			}
		}
		if (glm::determinant(Pose.Rotation) <= 0.f) return {InvalidDescription, "[Spatial:Transfer] Reflections are unavailable"};
		const auto Half = glm::abs(glm::dvec3(Member->GetSize())) * .5;
		const auto Extents = glm::abs(glm::dvec3(Pose.Rotation[0])) * Half.x + glm::abs(glm::dvec3(Pose.Rotation[1])) * Half.y +
			glm::abs(glm::dvec3(Pose.Rotation[2])) * Half.z;
		if (glm::any(glm::lessThan(glm::dvec3(Pose.Position) - Extents, glm::dvec3(Receipt.Bounds.Minimum))) ||
			glm::any(glm::greaterThan(glm::dvec3(Pose.Position) + Extents, glm::dvec3(Receipt.Bounds.Maximum))))
			return {InvalidDescription, "[Spatial:Transfer] The admitted footprint does not cover the destination body"};
		std::vector<std::shared_ptr<Instance>> Descendants{Member};
		for (std::size_t I = 0; I < Descendants.size(); ++I) {
			const auto Current = Descendants[I];
			for (const auto &Child : Current->Children) {
				if (dynamic_cast<WorldRoot *>(Child.get()) || dynamic_cast<ViewportFrame *>(Child.get())) continue;
				if (auto *Assigned = dynamic_cast<SpatialDomainMember *>(Child.get()); Assigned && Assigned->HasAssignedRegion()) continue;
				if (dynamic_cast<BasePart *>(Child.get()) || dynamic_cast<BlockGrid *>(Child.get()) ||
					dynamic_cast<DeformableBody *>(Child.get()) || dynamic_cast<Constraint *>(Child.get()))
					return {InvalidDescription, "[Spatial:Transfer] This movement requires an assembly footprint"};
				if (Descendants.size() >= MaximumInspectedColliders) return {ResourceLimit, "[Spatial:Transfer] Member traversal limit"};
				Descendants.push_back(Child);
			}
		}
		for (const auto &[Id, Weak] : Source->TrackedConstraints) {
			const auto Joint = Weak.lock();
			if (!Joint || !Joint->GetEnabled()) continue;
			const auto [First, Second] = Joint->GetActiveParts();
			if ((First == Member || Second == Member) && First && Second)
				return {InvalidDescription, "[Spatial:Transfer] The body belongs to a constrained assembly"};
		}
		if (!ValidateSpatialDestination(Receipt)) return {ResourceLimit, "[Spatial:Transfer] Destination coverage is stale"};
		Source->ApplyPendingPhysicsChanges();
		const auto Native = Source->PartBodies.find(Movement.Member);
		if (Native == Source->PartBodies.end()) return {InvalidId, "[Spatial:Transfer] Source body is unavailable"};
		const auto SourceState = Source->Physics.GetBodyState(Native->second);
		if (!SourceState) return {BackendFailure, "[Spatial:Transfer] Source body state is unavailable"};
		PhysicsOverlapRequest Occupancy;
		Occupancy.Shape = {.Kind = PhysicsQueryShapeKind::Box, .Transform = Pose, .Size = Member->GetSize()};
		Occupancy.Filter.CollisionGroup = SourceState->Description.CollisionGroup;
		Occupancy.Filter.CollisionMask = SourceState->Description.CollisionMask;
		Occupancy.Filter.IncludeBlockGrids = true; Occupancy.Filter.RequireCollidable = true;
		if (Source.get() == this) Occupancy.Filter.Bodies.push_back(Native->second);
		const auto Overlap = Physics.Overlap(Occupancy);
		if (!Overlap.Succeeded()) return {Overlap.Status, Overlap.Message};
		if (Overlap.Truncated || !Overlap.Bodies.empty()) return {InvalidDescription, "[Spatial:Transfer] Destination is occupied"};
		try {
			Member->AssertCanMutate(); Member->ValidateAdmittedSpatialProperty("CFrame",Pose);
			SourceRealm->EnsureAuthoritativeRevisionAvailable();
			if (Source.get() == this) {
				auto Description = SourceState->Description; Description.Transform = Pose;
				const auto Applied = Physics.UpdateBody(Native->second, Description);
				if (!Applied.Succeeded()) return Applied;
				Member->CFrame=Pose; Member->NotifyPropertyCommitted("CFrame");
			} else {
				const auto Region = std::dynamic_pointer_cast<SpatialRegion>(shared_from_this());
				const auto Parent = Member->GetParent().value_or(nullptr);
				if (!Region && (!Parent || GetEffectiveWorld(*Parent).get() != this))
					return {InvalidDescription, "[Spatial:Transfer] The authored parent cannot represent default membership"};
				Member->ChangeAssignedRegion(*Member, Region ? std::optional(Region) : std::nullopt, &Pose);
			}
		} catch (const std::invalid_argument &Error) { return {InvalidDescription, Error.what()}; }
		catch (const std::exception &Error) { return {BackendFailure, Error.what()}; }
		return {};
	}

	bool WorldRoot::CanHoldSpatialSubject(ObjectId Subject) {
		if (GetCurrentExecutionDomain()!=ExecutionDomain::Main || SteppingPhysics) return false;
		ApplyPendingPhysicsChanges();
		const auto It=PartBodies.find(Subject);
		if (It==PartBodies.end()) return false;
		const auto State=Physics.GetBodyState(It->second);
		return State && (State->Description.Anchored || (State->Description.Kinematic &&
			State->LinearVelocity==glm::vec3(0) && State->AngularVelocity==glm::vec3(0)));
	}
	bool WorldRoot::CanHoldSpatialCollision(ObjectId Subject) const {
		if (GetCurrentExecutionDomain()!=ExecutionDomain::Main || TrackedParts.size()>MaximumInspectedColliders || !IdDeformables.empty()) return false;
		if (BlockGridState && !BlockGridState->Assemblies.empty()) return false;
		if (BlockGridState) for (const auto &[Id, Member] : BlockGridState->Members)
			if (Member.Grid->IsReplicaMaterialization() && Member.Grid->GetMotionMode()=="Dynamic") return false;
		for (const auto &[Id,Weak]:TrackedParts) {
			if (Id==Subject) continue;
			const auto Part=Weak.lock();
			if (!Part) return false;
			if (!Part->GetCanCollide()) continue;
			const auto Body=PartBodies.find(Id);
			if (Body==PartBodies.end()) return false;
			const auto State=Physics.GetBodyState(Body->second);
			if (!State || !State->Description.Anchored) return false;
		}
		return true;
	}

	bool WorldRoot::ValidateSpatialDestination(const SpatialDestinationReceipt &Receipt) {
		if (Receipt.Domain != GetObjectId() || Receipt.Activation != SpatialActivation) return false;
		const auto Current = PrepareSpatialDestination(Receipt.Bounds, Receipt.RequiredOwners);
		return Current && *Current == Receipt;
	}
}

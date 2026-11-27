#include "gargantuan/classes/WorldRoot.hpp"

#include "BlockGridWorldState.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
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
			if (!Member.Ready || Member.Revision != Grid.GetField().GetRevision()) return std::unexpected(BackendFailure);
			if (Member.Bodies.size() > MaximumInspectedColliders - Inspected) return std::unexpected(ResourceLimit);
			Inspected += Member.Bodies.size();
			SpatialProviderReceipt Proof{.Owner = Id, .Provider = SpatialCollisionProvider::BlockGrid,
				.ContentRevision = *Member.Revision, .Pose = Member.Pose, .Size = glm::vec3(Member.CellSize),
				.AuthoritativeEmpty = !Member.Collidable || Grid.GetField().GetOccupiedCount() == 0};
			for (const auto &[Box, Body] : Member.Bodies) {
				const glm::vec3 Min(Box[0], Box[1], Box[2]), Max(Box[3], Box[4], Box[5]);
				const CFrame BoxPose(Member.Pose.PointToWorldSpace((Min + Max) * (.5f * Member.CellSize)), Member.Pose.Rotation);
				if (!OverlapsBody(Bounds, BoxPose, (Max - Min) * Member.CellSize)) continue;
				if (!Physics.IsBodyValid(Body)) return std::unexpected(BackendFailure);
				Proof.Installed.push_back({Body, {Box[0], Box[1], Box[2]}, *Member.Revision});
			}
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

	bool WorldRoot::ValidateSpatialDestination(const SpatialDestinationReceipt &Receipt) {
		if (Receipt.Domain != GetObjectId() || Receipt.Activation != SpatialActivation) return false;
		const auto Current = PrepareSpatialDestination(Receipt.Bounds, Receipt.RequiredOwners);
		return Current && *Current == Receipt;
	}
}

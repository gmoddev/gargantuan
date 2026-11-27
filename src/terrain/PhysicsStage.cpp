// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "PhysicsStage.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace gargantuan::terrain {
	namespace {
		constexpr std::size_t MaximumCohortChunks = 2 * MaximumReplicaContourChunks;
		constexpr std::size_t MaximumMeshVertices = 262144;
		constexpr std::size_t MaximumMeshIndices = 1048575;

		[[nodiscard]] std::uint64_t AllocateNamespace() noexcept {
			static std::atomic_uint64_t Next{1};
			auto Candidate = Next.load(std::memory_order_relaxed);
			while (Candidate != std::numeric_limits<std::uint64_t>::max()) {
				if (Next.compare_exchange_weak(Candidate, Candidate + 1, std::memory_order_relaxed))
					return Candidate;
			}
			return 0;
		}

		[[nodiscard]] bool ValidStageLimits(PhysicsStageLimits Limits) noexcept {
			return Limits.MaxLiveChunks <= MaximumPhysicsCollisionResources &&
				Limits.MaxCohortChunks <= MaximumCohortChunks &&
				Limits.MaxCandidateBytes <= MaximumPhysicsCollisionCanonicalBytes;
		}

		[[nodiscard]] bool ValidMesh(const CollisionSurfaceMesh &Mesh, GridSpec Grid) {
			if (Mesh.Indices.empty())
				return Mesh.Positions.empty() && Mesh.TriangleMaterials.empty();
			if (Mesh.Positions.size() < 3 || Mesh.Positions.size() > MaximumMeshVertices ||
				Mesh.Indices.size() > MaximumMeshIndices || Mesh.Indices.size() % 3 != 0 ||
				Mesh.TriangleMaterials.size() != Mesh.Indices.size() / 3)
				return false;
			const auto Extent = static_cast<float>(ChunkSide * Grid.GetSpacing());
			return std::ranges::all_of(Mesh.Positions, [Extent](SurfacePoint Point) {
					const auto InChunk = [Extent](float Value) { return Value >= 0 && Value <= Extent; };
					return InChunk(Point.X) && InChunk(Point.Y) && InChunk(Point.Z);
				}) && std::ranges::all_of(Mesh.Indices, [&Mesh](std::uint32_t Index) {
					return Index < Mesh.Positions.size();
				}) && std::ranges::all_of(Mesh.TriangleMaterials, [](MaterialId Material) {
					return Material != 0;
				});
		}

		[[nodiscard]] PhysicsBodyDesc Describe(ChunkKey Key, GridSpec Grid,
			const CollisionSurfaceMesh &Source, std::uint64_t Namespace, std::uint64_t ResourceRevision) {
			auto Mesh = std::make_shared<PhysicsTriangleMesh>();
			Mesh->ResourceId = "terrain/" + std::to_string(Namespace) + "/" + std::to_string(Key.X) + "/" +
				std::to_string(Key.Y) + "/" + std::to_string(Key.Z);
			Mesh->ContentRevision = ResourceRevision;
			auto Positions = std::make_shared<std::vector<glm::vec3>>();
			Positions->reserve(Source.Positions.size());
			for (const auto Point : Source.Positions)
				Positions->emplace_back(Point.X, Point.Y, Point.Z);
			Mesh->Positions = std::move(Positions);
			Mesh->Indices = std::make_shared<const std::vector<std::uint32_t>>(Source.Indices);

			const auto Extent = ChunkSide * Grid.GetSpacing();
			PhysicsBodyDesc Result;
			Result.Transform.Position = {static_cast<float>(Key.X * Extent),
				static_cast<float>(Key.Y * Extent), static_cast<float>(Key.Z * Extent)};
			Result.Shape = {PhysicsShapeKind::StaticTriangleMesh, glm::vec3{1.0f}, std::move(Mesh)};
			Result.Anchored = true;
			Result.CanTouch = false;
			return Result;
		}
	}

	struct PhysicsStage::Impl {
		struct Resident {
			PhysicsBodyId Body;
			Revision SurfaceRevision{};
		};
		struct PendingCohort {
			GridSpec Grid;
			std::vector<PhysicsBodyId> Created;
			std::vector<ChunkKey> Superseded;
			std::vector<ChunkKey> Painted;
			std::map<ChunkKey, Resident> Additions;
			std::map<PhysicsBodyId, ChunkKey> Owners;
		};

		struct Transaction final : StagedPublication {
			explicit Transaction(std::shared_ptr<Impl> Owner) : Owner(std::move(Owner)) {}
			~Transaction() override {
				if (Active)
					Owner->Discard();
			}
			void Install(Revision Value) noexcept override {
				if (!Active)
					return;
				Active = false;
				if (Value == 0)
					Owner->Discard();
				else if (Owner->Pending) {
					Owner->Install(Value);
					Installed = Owner->Healthy;
				}
			}
			[[nodiscard]] bool IsInstalled() const noexcept override { return Installed; }
			std::shared_ptr<Impl> Owner;
			bool Active{true};
			bool Installed{};
		};

		Impl(PhysicsWorld &World, PhysicsStageLimits Limits) : World(&World), Limits(Limits) {
			Healthy = Namespace != 0 && ValidStageLimits(Limits);
			if (Healthy)
				Unretired.reserve(Limits.MaxLiveChunks + Limits.MaxCohortChunks);
		}

		PhysicsWorld *World;
		PhysicsStageLimits Limits;
		const std::uint64_t Namespace{AllocateNamespace()};
		std::uint64_t NextResourceRevision{1};
		bool Healthy{};
		std::optional<GridSpec> InstalledGrid;
		std::optional<PendingCohort> Pending;
		std::map<ChunkKey, Resident> Chunks;
		std::map<PhysicsBodyId, ChunkKey> Owners;
		// Only used if a provider violates the owned-body retirement contract.
		// Reserved at construction so a failed retire can retain the native ID.
		std::vector<PhysicsBodyId> Unretired;

		[[nodiscard]] bool Retired(PhysicsBodyId Body) noexcept {
			try {
				if (World->DestroyBody(Body).Succeeded())
					return true;
			} catch (...) {
				// A provider can release the body before failing to recycle its slot.
			}
			try {
				return !World->IsBodyValid(Body);
			} catch (...) {
				return false;
			}
		}

		void Retire(PhysicsBodyId Body) noexcept {
			if (Retired(Body))
				return;
			Healthy = false;
			Unretired.push_back(Body);
		}

		void Discard() noexcept {
			if (!Pending)
				return;
			for (const auto Body : Pending->Created)
				Retire(Body);
			Pending.reset();
		}

		void Install(Revision SurfaceRevision) noexcept {
			if (!Pending)
				return;
			for (const auto Key : Pending->Superseded) {
				const auto Old = Chunks.find(Key);
				if (Old != Chunks.end()) {
					Retire(Old->second.Body);
					Owners.erase(Old->second.Body);
					Chunks.erase(Old);
				}
			}
			for (const auto Key : Pending->Painted) {
				const auto Old = Chunks.find(Key);
				if (Old != Chunks.end())
					Old->second.SurfaceRevision = SurfaceRevision;
			}
			for (auto &[Key, Addition] : Pending->Additions)
				Addition.SurfaceRevision = SurfaceRevision;
			Chunks.merge(Pending->Additions);
			Owners.merge(Pending->Owners);
			InstalledGrid = Pending->Grid;
			Pending.reset();
		}

		void Shutdown() noexcept {
			Discard();
			// Retry owners whose provider did not retire immediately. The physics
			// world owns any remaining native allocation through its destruction.
			for (const auto Body : Unretired)
				static_cast<void>(Retired(Body));
			for (const auto &[Key, Live] : Chunks)
				static_cast<void>(Retired(Live.Body));
			Chunks.clear();
			Owners.clear();
			Unretired.clear();
			World = nullptr;
			Healthy = false;
		}
	};

	PhysicsStage::PhysicsStage(PhysicsWorld &World, PhysicsStageLimits Limits)
		: State(std::make_shared<Impl>(World, Limits)) {}

	PhysicsStage::~PhysicsStage() {
		State->Shutdown();
	}

	std::expected<std::unique_ptr<StagedPublication>, EditStatus>
	PhysicsStage::Stage(GridSpec Grid, std::span<const ChunkPublicationCandidate> Chunks) {
		if (!State->Healthy || !State->World->IsValid() || State->Pending)
			return std::unexpected(EditStatus::Unavailable);
		if (!Grid.IsValid() || (State->InstalledGrid && *State->InstalledGrid != Grid))
			return std::unexpected(EditStatus::InvalidInput);
		if (Chunks.size() > State->Limits.MaxCohortChunks)
			return std::unexpected(EditStatus::LimitExceeded);

		try {
			std::set<ChunkKey> Seen;
			std::size_t AdditionCount = 0, RemovalCount = 0, Bytes = 0;
			for (const auto &Chunk : Chunks) {
				if (!IsValid(Chunk.Key) || !Seen.insert(Chunk.Key).second || !ValidMesh(Chunk.Collision, Grid))
					return std::unexpected(EditStatus::InvalidInput);
				const bool HasSurface = !Chunk.Collision.Indices.empty();
				const bool PreviouslyOwned = State->Chunks.contains(Chunk.Key);
				if (!Chunk.GeometryChanged && PreviouslyOwned) {
					if (!HasSurface)
						return std::unexpected(EditStatus::InvalidInput);
					continue;
				}
				RemovalCount += PreviouslyOwned;
				if (!HasSurface)
					continue;
				++AdditionCount;
				const auto MeshBytes = Chunk.Collision.Positions.size() * sizeof(glm::vec3) +
					Chunk.Collision.Indices.size() * sizeof(std::uint32_t);
				if (MeshBytes > State->Limits.MaxCandidateBytes - Bytes)
					return std::unexpected(EditStatus::LimitExceeded);
				Bytes += MeshBytes;
			}
			if (State->Chunks.size() - RemovalCount + AdditionCount > State->Limits.MaxLiveChunks ||
				AdditionCount > std::numeric_limits<std::uint64_t>::max() - State->NextResourceRevision)
				return std::unexpected(EditStatus::LimitExceeded);

			std::unique_ptr<StagedPublication> Handle = std::make_unique<Impl::Transaction>(State);
			auto &Prepared = State->Pending.emplace();
			Prepared.Grid = Grid;
			Prepared.Created.reserve(AdditionCount);
			Prepared.Superseded.reserve(RemovalCount);
			Prepared.Painted.reserve(Chunks.size());
			for (const auto &Chunk : Chunks) {
				const bool PreviouslyOwned = State->Chunks.contains(Chunk.Key);
				if (!Chunk.GeometryChanged && PreviouslyOwned) {
					Prepared.Painted.push_back(Chunk.Key);
					continue;
				}
				if (PreviouslyOwned)
					Prepared.Superseded.push_back(Chunk.Key);
				if (Chunk.Collision.Indices.empty())
					continue;
				const auto Description = Describe(Chunk.Key, Grid, Chunk.Collision, State->Namespace,
					State->NextResourceRevision++);
				PhysicsBodyId Body;
				try {
					Body = State->World->CreateBody(Description);
				} catch (...) {
					// A throwing provider gives no handle for verifying its partial operation.
					State->Healthy = false;
					return std::unexpected(EditStatus::PublicationDenied);
				}
				if (!Body.IsValid())
					return std::unexpected(EditStatus::PublicationDenied);
				if (State->Owners.contains(Body) || Prepared.Owners.contains(Body)) {
					State->Healthy = false;
					return std::unexpected(EditStatus::PublicationDenied);
				}
				Prepared.Created.push_back(Body);
				Prepared.Additions.emplace(Chunk.Key, Impl::Resident{Body});
				Prepared.Owners.emplace(Body, Chunk.Key);
			}
			return Handle;
		} catch (const std::bad_alloc &) {
			return std::unexpected(EditStatus::LimitExceeded);
		}
	}

	std::optional<ColliderOwner> PhysicsStage::FindOwner(PhysicsBodyId Body) const noexcept {
		const auto Owner = State->Owners.find(Body);
		if (Owner == State->Owners.end())
			return std::nullopt;
		const auto Resident = State->Chunks.find(Owner->second);
		return ColliderOwner{Owner->second, Resident->second.SurfaceRevision};
	}

	std::size_t PhysicsStage::GetBodyCount() const noexcept {
		return State->Owners.size();
	}

	bool PhysicsStage::IsStaging() const noexcept {
		return State->Pending.has_value();
	}

	bool PhysicsStage::IsHealthy() const noexcept {
		return State->Healthy;
	}

	void PhysicsStage::AppendBodies(std::vector<PhysicsBodyId> &Output) const {
		Output.reserve(Output.size() + GetBodyCount());
		for (const auto &[Body, Key] : State->Owners)
			Output.push_back(Body);
	}
}

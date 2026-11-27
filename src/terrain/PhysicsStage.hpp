// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "EditCoordinator.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"

namespace gargantuan::terrain {
	struct PhysicsStageLimits {
		std::size_t MaxLiveChunks{512};
		std::size_t MaxCohortChunks{8};
		std::size_t MaxCandidateBytes{16 * 1024 * 1024};
	};

	struct ColliderOwner {
		ChunkKey Key;
		Revision SurfaceRevision{};
	};

	// Main-domain adapter. PhysicsWorld must outlive PhysicsStage. Stage through
	// Install or discard is one safe-point block with no step or query interleave.
	// Existing neutral providers create a live body immediately.
	class PhysicsStage final : public CohortPublisher {
	public:
		explicit PhysicsStage(PhysicsWorld &World, PhysicsStageLimits Limits = {});
		~PhysicsStage() override;
		PhysicsStage(const PhysicsStage &) = delete;
		PhysicsStage &operator=(const PhysicsStage &) = delete;

		[[nodiscard]] std::expected<std::unique_ptr<StagedPublication>, EditStatus>
		Stage(GridSpec Grid, std::span<const ChunkPublicationCandidate> Chunks) override;

		[[nodiscard]] std::optional<ColliderOwner> FindOwner(PhysicsBodyId Body) const noexcept;
		[[nodiscard]] std::size_t GetBodyCount() const noexcept;
		[[nodiscard]] bool IsStaging() const noexcept;
		[[nodiscard]] bool IsHealthy() const noexcept;
		void AppendBodies(std::vector<PhysicsBodyId> &Output) const;

	private:
		struct Impl;
		std::shared_ptr<Impl> State;
	};
}

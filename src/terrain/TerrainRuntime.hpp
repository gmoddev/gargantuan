// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/runtime/JobSystem.hpp"
#include "terrain/RenderStage.hpp"

namespace gargantuan {
    class PhysicsWorld;
    struct RenderCameraInput;
}

namespace gargantuan::terrain {
    class PhysicsStage;
    class VisualDensityView;

    class TerrainRuntime final {
    public:
        // Main-only, before a runtime or query path is exposed. A denial
        // destroys every native body prepared for this candidate.
        [[nodiscard]] static std::expected<std::unique_ptr<TerrainRuntime>, EditStatus>
        Start(Field &World, ObjectId TerrainOwner, PhysicsWorld &Physics, bool AllowEdits,
            bool AllowReplicaReplacement = false, bool ExperimentalVisualDensity = false,
            std::shared_ptr<const TerrainMaterialPalette> Palette = {});
        ~TerrainRuntime();

        [[nodiscard]] std::expected<EditTicket, EditStatus> Submit(EditRequest Request,
            std::optional<Revision> ExpectedRevision = std::nullopt);
        [[nodiscard]] bool Cancel(EditTicket Ticket) noexcept;
        [[nodiscard]] std::vector<EditReceipt> Poll();
        [[nodiscard]] std::shared_ptr<const RenderState> CaptureRender() const noexcept;
        [[nodiscard]] const PhysicsStage &GetPhysicsStage() const noexcept { return *Physics; }
        [[nodiscard]] std::expected<void, EditStatus> InstallReplica(
            GridSpec Grid, std::vector<ChunkPublicationCandidate> Prepared, Revision Publication);
        void UpdateVisualDensity(const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height);

    private:
        TerrainRuntime(Field &World, ObjectId TerrainOwner, PhysicsWorld &Physics, bool AllowEdits,
            bool AllowReplicaReplacement, bool ExperimentalVisualDensity,
            std::shared_ptr<const TerrainMaterialPalette> Palette);
        const GridSpec Grid;
        const bool AllowEdits;
        const bool AllowReplicaReplacement;
        std::unique_ptr<PhysicsStage> Physics;
        RenderStage Render;
        JobSystem Jobs{2, JobSystem::StartPolicy::OnFirstSubmission};
        std::optional<EditCoordinator> Edits;
        std::unique_ptr<VisualDensityView> Detail;
        Revision VisualPublication{};
        bool VisualAdmissionLogged{};
        Revision ReplicaPublication{};
        std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> ReplicaSurfaces;
    };
}

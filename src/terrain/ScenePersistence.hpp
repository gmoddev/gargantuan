// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/terrain/Persistence.hpp"

#include <algorithm>
#include <stdexcept>

namespace gargantuan { class SpatialRegion; }

namespace gargantuan::terrain {
    // The document-local owner ID identifies a GTMF inside one place capture.
    [[nodiscard]] TerrainDatasetId GetDomainTerrainDatasetId(ProjectId Identity, std::uint32_t DocumentNodeId);
    [[nodiscard]] std::optional<TerrainCapture> CaptureDomainTerrain(SpatialRegion &Region, ProjectId Identity,
        std::uint32_t DocumentNodeId, const TerrainCaptureIndex *PriorDurable = nullptr);
    [[nodiscard]] std::unique_ptr<Field> LoadDomainTerrain(ProjectId Identity, std::uint32_t DocumentNodeId,
        const AssetContentId &Digest, const TerrainArtifactReader &Reader);
    [[nodiscard]] std::optional<TerrainCapture> CaptureSceneTerrain(DataModel &World, ProjectId Identity,
        const TerrainCaptureIndex *PriorDurable = nullptr);

    [[nodiscard]] InstanceSerialization::TerrainSceneAdmission GetSceneTerrainAdmission(ProjectId Identity,
        TerrainArtifactReader Reader);

    [[nodiscard]] std::unique_ptr<Field> LoadSceneTerrain(ProjectId Identity, const AssetContentId &Digest,
        const TerrainArtifactReader &Reader);

    [[nodiscard]] inline const TerrainArtifact *FindTerrainArtifact(
        std::span<const TerrainArtifact> Artifacts, const TerrainArtifactId &Id) {
        const auto Found = std::ranges::lower_bound(Artifacts, Id, {}, &TerrainArtifact::Id);
        return Found != Artifacts.end() && Found->Id == Id ? &*Found : nullptr;
    }

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, TerrainPersistenceError> ReadCapturedTerrainArtifact(
        std::span<const TerrainArtifact> Artifacts, const TerrainArtifactId &Id, std::size_t MaximumBytes);

    [[nodiscard]] inline std::runtime_error TerrainPersistenceFailure(TerrainPersistenceError Error) {
        return std::runtime_error("[Terrain:Persistence] " + std::string(GetTerrainPersistenceErrorText(Error)));
    }
}

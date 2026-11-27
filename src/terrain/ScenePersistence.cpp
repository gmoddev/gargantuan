// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/ScenePersistence.hpp"
#include "terrain/EditCoordinator.hpp"
#include "terrain/TerrainProfile.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/services/Workspace.hpp"

namespace gargantuan::terrain {
    namespace {
        void RequireReopenProfile(const Field &Storage) {
            const auto Profile = ValidateTerrainReopenProfile(Storage);
            if (Profile) return;
            if (Profile.error() == EditStatus::ReopenCapacityExceeded)
                throw std::runtime_error(
                    "[Terrain:Persistence] Offline terrain exceeds 512 stored pages or 1024 contour candidates");
            throw TerrainPersistenceFailure(TerrainPersistenceError::LimitExceeded);
        }
    }

    std::optional<TerrainCapture> CaptureSceneTerrain(DataModel &World, ProjectId Identity,
        const TerrainCaptureIndex *PriorDurable) {
        const auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(
            World.FindFirstChildOfClass("Workspace", false));
        const auto TerrainValue = WorkspaceValue ? WorkspaceValue->GetTerrain() : nullptr;
        if (!TerrainValue) return std::nullopt;
        const auto &Field = TerrainValue->GetField();
        if (Field.GetRevision() == 0 && Field.GetGrid() == GridSpec{}) return std::nullopt;
        RequireReopenProfile(Field);
        auto Capture = CaptureTerrain(Field, Identity, GetTerrainDatasetId(Identity), PriorDurable,
            TerrainPersistenceLimits{.MaximumChunks = MaximumStartupSourceChunks});
        if (!Capture) throw TerrainPersistenceFailure(Capture.error());
        return std::move(*Capture);
    }

    std::unique_ptr<Field> LoadSceneTerrain(ProjectId Identity, const AssetContentId &Digest,
        const TerrainArtifactReader &Reader) {
        auto Field = LoadTerrain(Identity, GetTerrainDatasetId(Identity), Digest, Reader,
            TerrainPersistenceLimits{.MaximumChunks = MaximumStartupSourceChunks});
        if (!Field) throw TerrainPersistenceFailure(Field.error());
        RequireReopenProfile(**Field);
        return std::move(*Field);
    }

    InstanceSerialization::TerrainSceneAdmission GetSceneTerrainAdmission(ProjectId Identity,
        TerrainArtifactReader Reader) {
        return [Identity, Reader = std::move(Reader)](DataModel &World, const AssetContentId &Digest) {
            auto Restored = LoadSceneTerrain(Identity, Digest, Reader);
            const auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World.GetService("Workspace"));
            if (!WorkspaceValue) throw std::runtime_error("[Terrain:Persistence] Expected a canonical Workspace");
            WorkspaceValue->RestoreTerrainField(std::move(Restored));
        };
    }

    std::expected<std::vector<std::uint8_t>, TerrainPersistenceError> ReadCapturedTerrainArtifact(
        std::span<const TerrainArtifact> Artifacts, const TerrainArtifactId &Id, std::size_t MaximumBytes) {
        const auto *Artifact = FindTerrainArtifact(Artifacts, Id);
        if (!Artifact || !Artifact->Bytes) return std::unexpected(TerrainPersistenceError::MissingArtifact);
        if (Artifact->Bytes->size() > MaximumBytes) return std::unexpected(TerrainPersistenceError::LimitExceeded);
        return *Artifact->Bytes;
    }
}

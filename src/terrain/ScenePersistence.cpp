// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/ScenePersistence.hpp"
#include "terrain/EditCoordinator.hpp"
#include "terrain/TerrainProfile.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
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

        std::optional<TerrainCapture> CaptureOwnerTerrain(const std::shared_ptr<Terrain> &Owner, ProjectId Identity,
            TerrainDatasetId Dataset, const TerrainCaptureIndex *PriorDurable) {
            if (!Owner) return std::nullopt;
            const auto &Field = Owner->GetField();
            if (Field.GetRevision() == 0 && Field.GetGrid() == GridSpec{}) return std::nullopt;
            RequireReopenProfile(Field);
            auto Capture = CaptureTerrain(Field, Identity, Dataset, PriorDurable,
                TerrainPersistenceLimits{.MaximumChunks = MaximumStartupSourceChunks});
            if (!Capture) throw TerrainPersistenceFailure(Capture.error());
            return std::move(*Capture);
        }

        std::unique_ptr<Field> LoadOwnerTerrain(ProjectId Identity, TerrainDatasetId Dataset,
            const AssetContentId &Digest, const TerrainArtifactReader &Reader) {
            auto Field = LoadTerrain(Identity, Dataset, Digest, Reader,
                TerrainPersistenceLimits{.MaximumChunks = MaximumStartupSourceChunks});
            if (!Field) throw TerrainPersistenceFailure(Field.error());
            RequireReopenProfile(**Field);
            return std::move(*Field);
        }
    }

    TerrainDatasetId GetDomainTerrainDatasetId(ProjectId Identity, std::uint32_t DocumentNodeId) {
        if (DocumentNodeId == 0)
            throw std::invalid_argument("[Terrain:Persistence] A domain terrain owner needs a document node ID");
        auto Dataset = GetTerrainDatasetId(Identity);
        Dataset.High ^= 0x5350415449414c00ULL; // Domain namespace, distinct from default Workspace.
        Dataset.Low ^= DocumentNodeId;
        // Only when this project's low half fits uint32_t can the reserved zero ID occur.
        if (!Dataset.IsValid()) Dataset.Low = 1ULL << 32;
        return Dataset;
    }

    std::optional<TerrainCapture> CaptureDomainTerrain(SpatialRegion &Region, ProjectId Identity,
        std::uint32_t DocumentNodeId, const TerrainCaptureIndex *PriorDurable) {
        return CaptureOwnerTerrain(Region.GetTerrain(), Identity, GetDomainTerrainDatasetId(Identity, DocumentNodeId),
            PriorDurable);
    }

    std::unique_ptr<Field> LoadDomainTerrain(ProjectId Identity, std::uint32_t DocumentNodeId,
        const AssetContentId &Digest, const TerrainArtifactReader &Reader) {
        return LoadOwnerTerrain(Identity, GetDomainTerrainDatasetId(Identity, DocumentNodeId), Digest, Reader);
    }

    DomainTerrainCaptures CaptureDocumentTerrain(const std::shared_ptr<DataModel> &World, ProjectId Identity,
        std::size_t WorkspaceClosureBytes) {
        DomainTerrainCaptures Captures;
        InstanceSerialization::VisitDocumentRegions(World, [&](SpatialRegion &Region, std::uint32_t Node) {
            auto Capture = CaptureDomainTerrain(Region, Identity, Node);
            if (!Capture) return;
            if (Captures.size() >= MaximumDomainTerrainBindings || WorkspaceClosureBytes > MaximumTerrainClosureBytes ||
                Capture->ClosureBytes > MaximumTerrainClosureBytes - WorkspaceClosureBytes)
                throw TerrainPersistenceFailure(TerrainPersistenceError::LimitExceeded);
            WorkspaceClosureBytes += Capture->ClosureBytes;
            Captures.emplace(Node, std::move(*Capture));
        });
        return Captures;
    }

    DomainTerrainBindings GetDomainTerrainBindings(const DomainTerrainCaptures &Captures) {
        DomainTerrainBindings Bindings;
        for (const auto &[Node, Capture] : Captures) Bindings.emplace(Node, Capture.ManifestDigest);
        return Bindings;
    }

    TerrainArtifactReader BudgetDocumentTerrainReads(TerrainArtifactReader Reader) {
        auto Remaining = std::make_shared<std::size_t>(MaximumTerrainClosureBytes);
        return [Reader = std::move(Reader), Remaining](const TerrainArtifactId &Id, std::size_t Maximum)
            -> std::expected<std::vector<std::uint8_t>, TerrainPersistenceError> {
            auto Bytes = Reader(Id, std::min(Maximum, *Remaining));
            if (Bytes) {
                if (Bytes->size() > *Remaining) return std::unexpected(TerrainPersistenceError::LimitExceeded);
                *Remaining -= Bytes->size();
            }
            return Bytes;
        };
    }

    InstanceSerialization::DomainTerrainAdmission GetDomainTerrainAdmission(ProjectId Identity,
        TerrainArtifactReader Reader) {
        return [Identity, Reader = std::move(Reader)](SpatialRegion &Region, std::uint32_t Node, const AssetContentId &Digest) {
            Region.RestoreTerrainField(LoadDomainTerrain(Identity, Node, Digest, Reader));
        };
    }

    std::optional<TerrainCapture> CaptureSceneTerrain(DataModel &World, ProjectId Identity,
        const TerrainCaptureIndex *PriorDurable) {
        const auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(
            World.FindFirstChildOfClass("Workspace", false));
        return CaptureOwnerTerrain(WorkspaceValue ? WorkspaceValue->GetTerrain() : nullptr, Identity,
            GetTerrainDatasetId(Identity), PriorDurable);
    }

    std::unique_ptr<Field> LoadSceneTerrain(ProjectId Identity, const AssetContentId &Digest,
        const TerrainArtifactReader &Reader) {
        return LoadOwnerTerrain(Identity, GetTerrainDatasetId(Identity), Digest, Reader);
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

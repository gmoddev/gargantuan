// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/terrain/Field.hpp"
#include "gargantuan/terrain/Manifest.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::terrain {
    inline constexpr std::size_t MaximumTerrainClosureBytes = 256 * 1024 * 1024;

    enum class TerrainArtifactPurpose : std::uint8_t { Chunk, Manifest };

    struct TerrainArtifactId {
        TerrainArtifactPurpose Purpose{};
        AssetContentId Digest;
        auto operator<=>(const TerrainArtifactId &) const = default;
    };

    struct TerrainArtifact {
        TerrainArtifactId Id;
        std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
    };

    struct TerrainPersistenceLimits {
        std::size_t MaximumChunks{MaximumTerrainManifestEntries};
        std::size_t MaximumClosureBytes{MaximumTerrainClosureBytes};
        FieldLimits RestoredField{};
    };

    enum class TerrainPersistenceError {
        InvalidBinding,
        MissingArtifact,
        ReadFailed,
        CorruptArtifact,
        UnsupportedVersion,
        LimitExceeded,
        OutOfMemory,
        Canceled,
    };

    struct TerrainCapturedChunk {
        TerrainChunkRef Reference;
        ChunkVersion Version;
    };

    // Trusted in-process reuse metadata for a closure verified durable at the
    // same destination. Never reconstructed from a file or client request.
    struct TerrainCaptureIndex {
        ProjectId Project;
        TerrainDatasetId Dataset;
        GridSpec Grid;
        std::uint64_t FieldLifetime{};
        std::vector<TerrainCapturedChunk> Chunks;
    };

    struct TerrainCapture {
        AssetContentId ManifestDigest;
        Revision FieldRevision{};
        TerrainCaptureIndex Index;
        // Sorted by purpose then digest, with complete manifest bytes. If
        // PriorDurable was provided, reused leaves remain in the manifest.
        std::vector<TerrainArtifact> Artifacts;
        std::size_t ClosureBytes{};
        std::size_t ReusedChunks{};
    };

    // The reader receives only a validated typed digest, purpose and read cap.
    // Project/package owners provide their confined and authorized implementation.
    using TerrainArtifactReader = std::function<std::expected<std::vector<std::uint8_t>, TerrainPersistenceError>(
        const TerrainArtifactId &Id, std::size_t MaximumBytes)>;

    [[nodiscard]] TerrainDatasetId GetTerrainDatasetId(ProjectId Project) noexcept;
    [[nodiscard]] std::optional<std::string> GetTerrainArtifactPath(const TerrainArtifactId &Id);
    [[nodiscard]] std::string_view GetTerrainPersistenceErrorText(TerrainPersistenceError Error) noexcept;

    // Capture live state on Main, or encode an already acquired complete
    // snapshot on another domain without accessing the live Field.
    [[nodiscard]] std::expected<TerrainCapture, TerrainPersistenceError> CaptureTerrain(
        const Field &Source, ProjectId Project, TerrainDatasetId Dataset,
        const TerrainCaptureIndex *PriorDurable = nullptr, TerrainPersistenceLimits Limits = {},
        std::stop_token Stop = {});
    [[nodiscard]] std::expected<TerrainCapture, TerrainPersistenceError> CaptureTerrain(
        const CommittedFieldCapture &Source, ProjectId Project, TerrainDatasetId Dataset,
        const TerrainCaptureIndex *PriorDurable = nullptr, TerrainPersistenceLimits Limits = {},
        std::stop_token Stop = {});

    // Reconstruct a detached Field. Its lifetime is new and former runtime
    // revision tickets cannot authorize new state or publication.
    [[nodiscard]] std::expected<std::unique_ptr<Field>, TerrainPersistenceError> LoadTerrain(
        ProjectId Project, TerrainDatasetId Dataset, AssetContentId ManifestDigest,
        const TerrainArtifactReader &Reader, TerrainPersistenceLimits Limits = {}, std::stop_token Stop = {});
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/terrain/Types.hpp"

#include <array>
#include <expected>
#include <span>
#include <stop_token>
#include <vector>

namespace gargantuan::terrain {
    inline constexpr std::uint16_t TerrainManifestVersion = 1;
    inline constexpr std::size_t TerrainManifestHeaderBytes = 96;
    inline constexpr std::size_t TerrainManifestEntryBytes = 60;
    inline constexpr std::size_t MaximumTerrainManifestEntries = 65'536;
    inline constexpr std::size_t MaximumTerrainManifestBytes =
        TerrainManifestHeaderBytes + MaximumTerrainManifestEntries * TerrainManifestEntryBytes;

    struct TerrainDatasetId {
        std::uint64_t High{};
        std::uint64_t Low{};
        [[nodiscard]] bool IsValid() const noexcept { return High != 0 || Low != 0; }
        auto operator<=>(const TerrainDatasetId &) const = default;
    };

    struct TerrainArtifactDigest {
        // Exact SHA-256 digest bytes in the standard digest order.
        std::array<std::uint8_t, 32> Bytes{};
        auto operator<=>(const TerrainArtifactDigest &) const = default;
    };

    struct TerrainChunkRef {
        ChunkKey Key;
        TerrainArtifactDigest Digest;
        std::uint32_t ByteCount{};
        auto operator<=>(const TerrainChunkRef &) const = default;
    };

    // A complete dataset directory at one authored spacing. Missing keys have
    // the default-air sample after the caller has admitted the entire closure.
    struct TerrainManifest {
        ProjectId Project;
        TerrainDatasetId Dataset;
        GridSpec Grid;
        std::vector<TerrainChunkRef> Chunks;
        bool operator==(const TerrainManifest &) const = default;
    };

    enum class TerrainManifestError {
        InvalidBinding,
        InvalidEntry,
        IncompatibleSpecification,
        UnsupportedVersion,
        Malformed,
        NonCanonical,
        LimitExceeded,
        OutOfMemory,
        Canceled,
    };

    // Inert values only. Before using the directory a caller verifies the
    // manifest and artifact contents, resolves material identity, and commits
    // through the canonical project and authoritative field owners.
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, TerrainManifestError> EncodeTerrainManifest(
        const TerrainManifest &Manifest, std::stop_token Stop = {});
    [[nodiscard]] std::expected<TerrainManifest, TerrainManifestError> DecodeTerrainManifest(
        std::span<const std::uint8_t> Bytes, ProjectId ExpectedProject, TerrainDatasetId ExpectedDataset,
        std::stop_token Stop = {});
}

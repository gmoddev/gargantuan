// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Field.hpp"

#include <stop_token>

namespace gargantuan::terrain {
    inline constexpr std::uint16_t ChunkArtifactVersion = 1;
    inline constexpr std::size_t ChunkArtifactHeaderBytes = 36;
    inline constexpr std::size_t MaximumChunkArtifactBytes = ChunkArtifactHeaderBytes +
        (ChunkSide / BrickSide) * (ChunkSide / BrickSide) * (ChunkSide / BrickSide) *
        (1 + BrickSide * BrickSide * BrickSide * (sizeof(std::int16_t) + sizeof(MaterialId)));

    enum class ChunkCodecError {
        InvalidBinding,
        UncapturedChunk,
        UnsupportedVersion,
        Malformed,
        NonCanonical,
        LimitExceeded,
        OutOfMemory,
        Canceled,
    };

    struct DecodedChunk {
        ChunkKey Key;
        GridSpec Grid;
        // Complete replacement of the valid owned sample range, including air.
        // A caller must still prepare and commit through the authoritative Field.
        std::vector<SampleWrite> Writes;
    };

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, ChunkCodecError> EncodeChunk(
        const Snapshot &Source, ChunkKey Key, std::stop_token Stop = {});
    [[nodiscard]] std::expected<DecodedChunk, ChunkCodecError> DecodeChunk(
        std::span<const std::uint8_t> Bytes, ChunkKey ExpectedKey, GridSpec ExpectedGrid, std::stop_token Stop = {});
}

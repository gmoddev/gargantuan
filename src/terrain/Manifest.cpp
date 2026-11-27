// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Manifest.hpp"

#include "ArtifactBytes.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/terrain/ChunkCodec.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <optional>

namespace gargantuan::terrain {
    namespace {
        using artifact_detail::Reader;
        using artifact_detail::Writer;

        constexpr std::uint32_t Magic = 0x464d5447; // GTMF
        constexpr std::uint8_t SignedScalarEncoding = 1;
        constexpr std::uint8_t AbsentAndExteriorAreAir = 1;
        constexpr std::uint8_t NoRequiredFlags = 0;
        constexpr std::uint16_t OutsideDensity = 32767;
        constexpr std::uint32_t OutsideMaterial = 0;
        constexpr std::int64_t MaximumAxis = (1LL << 20) - 1;
        constexpr std::size_t MinimumNonDefaultArtifactBytes = ChunkArtifactHeaderBytes + 6;

        static_assert(ChunkArtifactVersion == 1 && ChunkSide == 32 && BrickSide == 8);
        static_assert(AirSample.Density == OutsideDensity && AirSample.Material == OutsideMaterial);
        static_assert(MaximumSampleCoordinate == MaximumAxis);

        bool IsV1Grid(GridSpec Grid) noexcept {
            return Grid.IsValid() && Grid.SpacingShift >= -1 && Grid.SpacingShift <= 1;
        }

        std::expected<void, TerrainManifestError> CheckEntry(
            const TerrainChunkRef &Entry, const std::optional<ChunkKey> &Previous) {
            if (!terrain::IsValid(Entry.Key) || Entry.ByteCount < MinimumNonDefaultArtifactBytes ||
                Entry.ByteCount > MaximumChunkArtifactBytes ||
                std::ranges::all_of(Entry.Digest.Bytes, [](std::uint8_t Byte) { return Byte == 0; }))
                return std::unexpected(TerrainManifestError::InvalidEntry);
            if (Previous && !(Entry.Key > *Previous))
                return std::unexpected(TerrainManifestError::NonCanonical);
            return {};
        }

        void WriteSigned(Writer &Output, std::int64_t Value) {
            Output.Add(std::bit_cast<std::uint64_t>(Value));
        }

        bool ReadSigned(Reader &Input, std::int64_t &Value) {
            std::uint64_t Raw = 0;
            if (!Input.Read(Raw))
                return false;
            Value = std::bit_cast<std::int64_t>(Raw);
            return true;
        }

        void WriteLogicalSpecification(Writer &Output, GridSpec Grid) {
            Output.Add(ChunkArtifactVersion);
            Output.Add(static_cast<std::uint8_t>(Grid.SpacingShift + 1));
            Output.Add(SignedScalarEncoding);
            Output.Add(static_cast<std::uint8_t>(ChunkSide));
            Output.Add(static_cast<std::uint8_t>(BrickSide));
            Output.Add(AbsentAndExteriorAreAir);
            Output.Add(NoRequiredFlags);
            Output.Add(OutsideDensity);
            Output.Add(OutsideMaterial);
            WriteSigned(Output, -MaximumAxis);
            WriteSigned(Output, MaximumAxis);
            for (int Axis = 0; Axis < 3; ++Axis)
                WriteSigned(Output, 0);
        }

        std::expected<GridSpec, TerrainManifestError> ReadLogicalSpecification(Reader &Input) {
            std::uint16_t ChunkVersion = 0, Density = 0;
            std::uint8_t Spacing = 0, Field = 0, ChunkDimension = 0, BrickDimension = 0, Boundary = 0, Flags = 0;
            std::uint32_t Material = 0;
            std::int64_t Min = 0, Max = 0, OriginX = 0, OriginY = 0, OriginZ = 0;
            if (!Input.Read(ChunkVersion) || !Input.Read(Spacing) || !Input.Read(Field) || !Input.Read(ChunkDimension) ||
                !Input.Read(BrickDimension) || !Input.Read(Boundary) || !Input.Read(Flags) || !Input.Read(Density) ||
                !Input.Read(Material) || !ReadSigned(Input, Min) || !ReadSigned(Input, Max) ||
                !ReadSigned(Input, OriginX) || !ReadSigned(Input, OriginY) || !ReadSigned(Input, OriginZ))
                return std::unexpected(TerrainManifestError::Malformed);

            if (Spacing > 2 || ChunkVersion != ChunkArtifactVersion || Field != SignedScalarEncoding ||
                ChunkDimension != ChunkSide || BrickDimension != BrickSide || Boundary != AbsentAndExteriorAreAir ||
                Flags != NoRequiredFlags || Density != OutsideDensity || Material != OutsideMaterial ||
                Min != -MaximumAxis || Max != MaximumAxis || OriginX != 0 || OriginY != 0 || OriginZ != 0)
                return std::unexpected(TerrainManifestError::IncompatibleSpecification);

            GridSpec Grid{static_cast<std::int8_t>(static_cast<int>(Spacing) - 1)};
            if (!IsV1Grid(Grid))
                return std::unexpected(TerrainManifestError::IncompatibleSpecification);
            return Grid;
        }
    }

    std::expected<std::vector<std::uint8_t>, TerrainManifestError> EncodeTerrainManifest(
        const TerrainManifest &Manifest, std::stop_token Stop) {
        G_PROFILE("Terrain::EncodeManifest");
        if (Stop.stop_requested())
            return std::unexpected(TerrainManifestError::Canceled);
        if (!Manifest.Project.IsValid() || !Manifest.Dataset.IsValid())
            return std::unexpected(TerrainManifestError::InvalidBinding);
        if (!IsV1Grid(Manifest.Grid))
            return std::unexpected(TerrainManifestError::IncompatibleSpecification);
        if (Manifest.Chunks.size() > MaximumTerrainManifestEntries)
            return std::unexpected(TerrainManifestError::LimitExceeded);

        std::optional<ChunkKey> Previous;
        for (const auto &Entry : Manifest.Chunks) {
            if (Stop.stop_requested())
                return std::unexpected(TerrainManifestError::Canceled);
            if (const auto Checked = CheckEntry(Entry, Previous); !Checked)
                return std::unexpected(Checked.error());
            Previous = Entry.Key;
        }

        try {
            Writer Output;
            Output.Bytes.reserve(TerrainManifestHeaderBytes + Manifest.Chunks.size() * TerrainManifestEntryBytes);
            Output.Add(Magic);
            Output.Add(TerrainManifestVersion);
            WriteLogicalSpecification(Output, Manifest.Grid);
            Output.Add(Manifest.Project.Bytes);
            Output.Add(Manifest.Dataset.High);
            Output.Add(Manifest.Dataset.Low);
            Output.Add(static_cast<std::uint32_t>(Manifest.Chunks.size()));
            for (const auto &Entry : Manifest.Chunks) {
                if (Stop.stop_requested())
                    return std::unexpected(TerrainManifestError::Canceled);
                WriteSigned(Output, Entry.Key.X);
                WriteSigned(Output, Entry.Key.Y);
                WriteSigned(Output, Entry.Key.Z);
                Output.Add(Entry.Digest.Bytes);
                Output.Add(Entry.ByteCount);
            }
            if (Stop.stop_requested())
                return std::unexpected(TerrainManifestError::Canceled);
            return std::move(Output.Bytes);
        } catch (const std::bad_alloc &) {
            return std::unexpected(TerrainManifestError::OutOfMemory);
        }
    }

    std::expected<TerrainManifest, TerrainManifestError> DecodeTerrainManifest(
        std::span<const std::uint8_t> Bytes, ProjectId ExpectedProject, TerrainDatasetId ExpectedDataset,
        std::stop_token Stop) {
        G_PROFILE("Terrain::DecodeManifest");
        if (Stop.stop_requested())
            return std::unexpected(TerrainManifestError::Canceled);
        if (!ExpectedProject.IsValid() || !ExpectedDataset.IsValid())
            return std::unexpected(TerrainManifestError::InvalidBinding);
        if (Bytes.size() > MaximumTerrainManifestBytes)
            return std::unexpected(TerrainManifestError::LimitExceeded);

        Reader Input(Bytes);
        std::uint32_t ReadMagic = 0;
        std::uint16_t Version = 0;
        if (!Input.Read(ReadMagic) || ReadMagic != Magic || !Input.Read(Version))
            return std::unexpected(TerrainManifestError::Malformed);
        if (Version != TerrainManifestVersion)
            return std::unexpected(TerrainManifestError::UnsupportedVersion);
        const auto Grid = ReadLogicalSpecification(Input);
        if (!Grid)
            return std::unexpected(Grid.error());

        TerrainManifest Manifest;
        Manifest.Grid = *Grid;
        std::uint32_t Count = 0;
        if (!Input.Read(Manifest.Project.Bytes) || !Input.Read(Manifest.Dataset.High) ||
            !Input.Read(Manifest.Dataset.Low) || !Input.Read(Count))
            return std::unexpected(TerrainManifestError::Malformed);
        if (Manifest.Project != ExpectedProject || Manifest.Dataset != ExpectedDataset)
            return std::unexpected(TerrainManifestError::InvalidBinding);
        if (Count > MaximumTerrainManifestEntries)
            return std::unexpected(TerrainManifestError::LimitExceeded);
        if (Input.GetRemaining() != Count * TerrainManifestEntryBytes)
            return std::unexpected(TerrainManifestError::Malformed);

        try {
            Manifest.Chunks.reserve(Count);
            std::optional<ChunkKey> Previous;
            for (std::uint32_t I = 0; I < Count; ++I) {
                if (Stop.stop_requested())
                    return std::unexpected(TerrainManifestError::Canceled);
                TerrainChunkRef Entry;
                if (!ReadSigned(Input, Entry.Key.X) || !ReadSigned(Input, Entry.Key.Y) ||
                    !ReadSigned(Input, Entry.Key.Z) || !Input.Read(Entry.Digest.Bytes) || !Input.Read(Entry.ByteCount))
                    return std::unexpected(TerrainManifestError::Malformed);
                if (const auto Checked = CheckEntry(Entry, Previous); !Checked)
                    return std::unexpected(Checked.error());
                Manifest.Chunks.push_back(Entry);
                Previous = Entry.Key;
            }
            if (Stop.stop_requested())
                return std::unexpected(TerrainManifestError::Canceled);
            return Manifest;
        } catch (const std::bad_alloc &) {
            return std::unexpected(TerrainManifestError::OutOfMemory);
        }
    }
}

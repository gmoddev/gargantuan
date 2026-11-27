// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/ChunkCodec.hpp"
#include "gargantuan/terrain/Manifest.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
    using namespace gargantuan;
    using namespace gargantuan::terrain;
    using Bytes = std::vector<std::uint8_t>;

    constexpr ProjectId Project{{0xAB, 0x01, 0xAC}};
    constexpr TerrainDatasetId Dataset{0x0123456789ABCDEF, 0x1234567887654321};
    constexpr std::size_t CountOffset = 92;
    constexpr std::size_t FlagsOffset = 13;
    constexpr std::size_t FirstRef = TerrainManifestHeaderBytes;
    constexpr std::size_t OffsetDigest = 24;
    constexpr std::size_t OffsetLength = 56;

    void Verify(bool Condition, std::string_view Description) {
        if (!Condition) {
            std::cerr << "[Terrain:Manifest] " << Description << '\n';
            std::exit(2);
        }
    }

    template <typename Result>
    void Denies(const std::expected<Result, TerrainManifestError> &Value, TerrainManifestError Error,
        std::string_view Description) {
        Verify(!Value && Value.error() == Error, Description);
    }

    void WriteLittle32(Bytes &Value, std::size_t Offset, std::uint32_t Payload) {
        for (std::size_t I = 0; I < 4; ++I)
            Value[Offset + I] = static_cast<std::uint8_t>(Payload >> (I * 8));
    }

    TerrainManifest Example() {
        TerrainManifest Source{Project, Dataset, GridSpec{-1}, {}};
        Source.Chunks = {
            {{-31, 0, 15}, {{1, 2, 3, 0, 255}}, 42},
            {{-31, 4, -150}, {{2, 0, 0, 0, 3}}, 4096},
            {{15, -28, -17}, {{0, 0, 0, 0, 4}}, static_cast<std::uint32_t>(MaximumChunkArtifactBytes)},
        };
        return Source;
    }

    void CheckRoundTrip() {
        const auto Source = Example();
        const auto Encoded = EncodeTerrainManifest(Source);
        Verify(Encoded && Encoded->size() == TerrainManifestHeaderBytes + 3 * TerrainManifestEntryBytes, "fixed wire size");
        Verify((*Encoded)[0] == 'G' && (*Encoded)[1] == 'T' && (*Encoded)[2] == 'M' && (*Encoded)[3] == 'F',
            "manifest magic");
        Verify((*Encoded)[4] == 1 && (*Encoded)[5] == 0 && (*Encoded)[6] == 1 && (*Encoded)[7] == 0 &&
            (*Encoded)[8] == 0, "version, artifact version and half-unit code");
        Verify((*Encoded)[60] == 0xAB && (*Encoded)[76] == 0xEF && (*Encoded)[83] == 0x01 &&
            (*Encoded)[CountOffset] == 3, "project and dataset byte order");
        Verify((*Encoded)[FirstRef] == 225 && (*Encoded)[FirstRef + 1] == 255 &&
            (*Encoded)[FirstRef + OffsetDigest] == 1 && (*Encoded)[FirstRef + OffsetLength] == 42,
            "signed key, digest and byte count on wire");

        const auto Decoded = DecodeTerrainManifest(*Encoded, Project, Dataset);
        Verify(Decoded && *Decoded == Source, "manifest exact decoded value equality");
        const auto Again = EncodeTerrainManifest(*Decoded);
        Verify(Again && *Again == *Encoded, "canonical byte equality");
        for (std::size_t End = 0; End < Encoded->size(); ++End)
            Verify(!DecodeTerrainManifest(std::span(*Encoded).first(End), Project, Dataset), "strict prefix denied");

        TerrainManifest Empty{Project, Dataset, GridSpec{1}, {}};
        const auto EmptyEncoded = EncodeTerrainManifest(Empty);
        Verify(EmptyEncoded && EmptyEncoded->size() == TerrainManifestHeaderBytes, "empty admitted dataset");
        const auto EmptyDecoded = DecodeTerrainManifest(*EmptyEncoded, Project, Dataset);
        Verify(EmptyDecoded && *EmptyDecoded == Empty, "empty directory round-trip");
    }

    void CheckDenials() {
        const auto Good = EncodeTerrainManifest(Example());
        Verify(Good.has_value(), "canonical denied-case fixture");
        Denies(DecodeTerrainManifest(*Good, ProjectId{}, Dataset), TerrainManifestError::InvalidBinding, "zero expected project");
        Denies(DecodeTerrainManifest(*Good, Project, {1, 2}), TerrainManifestError::InvalidBinding, "foreign dataset");
        Denies(DecodeTerrainManifest(*Good, ProjectId{{1}}, Dataset), TerrainManifestError::InvalidBinding, "foreign project");

        auto Bad = *Good;
        Bad[0] = 'B';
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::Malformed, "foreign magic");
        Bad = *Good;
        Bad[4] = 2;
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::UnsupportedVersion, "future manifest version");
        constexpr std::array<std::size_t, 14> UnsupportedOffsets{6, 8, 9, 10, 11, 12, FlagsOffset,
            14, 16, 20, 28, 36, 44, 52};
        for (const auto Offset : UnsupportedOffsets) {
            Bad = *Good;
            Bad[Offset] ^= 8u;
            Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::IncompatibleSpecification,
                "unsupported feature, lattice or boundary specification");
        }
        Bad = *Good;
        Bad.push_back(0);
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::Malformed, "trailing bytes");
        Bad = *Good;
        WriteLittle32(Bad, CountOffset, 65'537);
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::LimitExceeded, "wire entry count cap");
        Bytes Oversize(MaximumTerrainManifestBytes + 1, 0);
        Denies(DecodeTerrainManifest(Oversize, Project, Dataset), TerrainManifestError::LimitExceeded, "wire byte cap");

        Bad = *Good;
        std::swap_ranges(Bad.begin() + FirstRef, Bad.begin() + FirstRef + TerrainManifestEntryBytes,
            Bad.begin() + FirstRef + TerrainManifestEntryBytes);
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::NonCanonical, "unsorted signed keys");
        Bad = *Good;
        std::copy_n(Bad.begin() + FirstRef, OffsetDigest, Bad.begin() + FirstRef + TerrainManifestEntryBytes);
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::NonCanonical, "duplicate signed keys");
        Bad = *Good;
        Bad[FirstRef + 7] = 0x7F;
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::InvalidEntry, "oversize coordinate");
        Bad = *Good;
        std::fill_n(Bad.begin() + FirstRef + OffsetDigest, 32, std::uint8_t{0});
        Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::InvalidEntry, "unbound digest");
        for (const auto Size : {0u, static_cast<std::uint32_t>(ChunkArtifactHeaderBytes),
                static_cast<std::uint32_t>(MaximumChunkArtifactBytes) + 1u}) {
            Bad = *Good;
            WriteLittle32(Bad, FirstRef + OffsetLength, Size);
            Denies(DecodeTerrainManifest(Bad, Project, Dataset), TerrainManifestError::InvalidEntry, "invalid stored leaf size");
        }

        auto Invalid = Example();
        std::swap(Invalid.Chunks[0], Invalid.Chunks[1]);
        Denies(EncodeTerrainManifest(Invalid), TerrainManifestError::NonCanonical, "encoder requires canonical order");
        Invalid = Example();
        Invalid.Chunks[1].Key = Invalid.Chunks[0].Key;
        Denies(EncodeTerrainManifest(Invalid), TerrainManifestError::NonCanonical, "encoder denies duplicate keys");
        Invalid = Example();
        Invalid.Grid.SpacingShift = -2;
        Denies(EncodeTerrainManifest(Invalid), TerrainManifestError::IncompatibleSpecification, "encoder rejects foreign grid");
        Invalid = Example();
        Invalid.Dataset = {};
        Denies(EncodeTerrainManifest(Invalid), TerrainManifestError::InvalidBinding, "encoder rejects zero dataset");

        std::stop_source Stop;
        Verify(Stop.request_stop(), "cancel test setup");
        Denies(EncodeTerrainManifest(Example(), Stop.get_token()), TerrainManifestError::Canceled, "encode canceled");
        Denies(DecodeTerrainManifest(*Good, Project, Dataset, Stop.get_token()), TerrainManifestError::Canceled, "decode canceled");
    }

    void CheckMaximum() {
        TerrainManifest Source{Project, Dataset, {}, {}};
        Source.Chunks.reserve(MaximumTerrainManifestEntries);
        constexpr auto Min = FloorChunk(-MaximumSampleCoordinate);
        for (std::size_t I = 0; I < MaximumTerrainManifestEntries; ++I)
            Source.Chunks.push_back({
                {Min + static_cast<std::int64_t>(I), 0, -19},
                {{3, static_cast<std::uint8_t>(I), static_cast<std::uint8_t>(I >> 8)}},
                42,
            });

        const auto Encoded = EncodeTerrainManifest(Source);
        Verify(Encoded && Encoded->size() == MaximumTerrainManifestBytes, "literal maximum test fixture");
        const auto Result = DecodeTerrainManifest(*Encoded, Project, Dataset);
        Verify(Result && *Result == Source, "maximum count and byte parity");
        Source.Chunks.push_back(Source.Chunks.back());
        Denies(EncodeTerrainManifest(Source), TerrainManifestError::LimitExceeded, "encoder count admission precedes allocation");

        std::cout << "[Terrain:Manifest] max " << MaximumTerrainManifestEntries << " entries, "
                  << Encoded->size() << " bytes\n";
    }
}

int main() {
    CheckRoundTrip();
    CheckDenials();
    CheckMaximum();
    std::cout << "[Terrain:Manifest] canonical round-trip, binding and denial checks passed\n";
}

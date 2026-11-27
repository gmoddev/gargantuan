// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/ChunkCodec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
    using namespace gargantuan::terrain;

    using ByteVector = std::vector<std::uint8_t>;
    constexpr ChunkKey Key{-2, 3, -7};
    constexpr GridSpec HalfUnit{-1};
    constexpr GridSpec OneUnit{0};
    constexpr GridSpec TwoUnits{1};
    constexpr std::size_t BrickSamples = 8 * 8 * 8;

    void Verify(bool Condition, std::string_view Description) {
        if (!Condition) {
            std::cerr << "[Terrain:ChunkCodec] " << Description << '\n';
            std::exit(2);
        }
    }

    constexpr SamplePoint PointAt(ChunkKey Chunk, std::size_t Brick, std::size_t Local) {
        return {Chunk.X * ChunkSide + static_cast<std::int64_t>(Brick % 4 * 8 + Local % 8),
            Chunk.Y * ChunkSide + static_cast<std::int64_t>(Brick / 4 % 4 * 8 + Local / 8 % 8),
            Chunk.Z * ChunkSide + static_cast<std::int64_t>(Brick / 16 * 8 + Local / 64)};
    }

    void Apply(Field &Destination, ChunkKey Chunk, std::span<const SampleWrite> Writes) {
        const std::array Keys{Chunk};
        auto SnapshotResult = Destination.Capture(Keys);
        Verify(SnapshotResult.has_value(), "capture destination");
        auto Edit = Field::Prepare(std::move(*SnapshotResult), Writes);
        Verify(Edit.has_value(), "prepare decoded owned writes against field");
        const auto Result = Destination.Commit(std::move(*Edit));
        Verify(Result.has_value(), "commit through field authority");
    }

    ByteVector Encode(const Field &Source, ChunkKey Chunk) {
        const std::array Keys{Chunk};
        const auto Frozen = Source.Capture(Keys);
        Verify(Frozen.has_value(), "capture source");
        auto Encoded = EncodeChunk(*Frozen, Chunk);
        Verify(Encoded.has_value(), "encode captured chunk");
        return std::move(*Encoded);
    }

    ByteVector RoundTrip(const Field &Source, ChunkKey Chunk, std::size_t ValidWrites = 32768) {
        const auto Encoded = Encode(Source, Chunk);
        auto Decoded = DecodeChunk(Encoded, Chunk, Source.GetGrid());
        Verify(Decoded && Decoded->Key == Chunk && Decoded->Grid == Source.GetGrid(), "decoded binding");
        Verify(Decoded->Writes.size() == ValidWrites, "decoded owned sample count");

        Field Destination(Source.GetGrid());
        const auto LastValid = PointAt(Chunk, 63, 511);
        const std::array<SampleWrite, 1> Dirty{{{LastValid, {-1, std::numeric_limits<MaterialId>::max()}}}};
        Apply(Destination, Chunk, Dirty);
        Apply(Destination, Chunk, Decoded->Writes);
        for (const auto &Write : Decoded->Writes) {
            const auto Old = Source.Read(Write.Point);
            const auto New = Destination.Read(Write.Point);
            Verify(Old && New && *Old == *New && *New == Write.Value, "owned sample parity after independent apply");
        }
        Verify(Encode(Destination, Chunk) == Encoded, "canonical bytes independent of authority and revisions");
        return Encoded;
    }

    template <typename T>
    void CheckError(const std::expected<T, ChunkCodecError> &Result, ChunkCodecError Error,
        std::string_view Description) {
        Verify(!Result && Result.error() == Error, Description);
    }

    void SetLittleEndian(ByteVector &Bytes, std::size_t Start, std::uint32_t Value, std::size_t Count) {
        for (std::size_t I = 0; I < Count; ++I)
            Bytes[Start + I] = static_cast<std::uint8_t>(Value >> (I * 8));
    }

    void TestBindingAndAir() {
        Field Source(HalfUnit);
        const auto Encoded = RoundTrip(Source, Key);
        Verify(Encoded.size() == ChunkArtifactHeaderBytes && Encoded[0] == 'G' && Encoded[1] == 'T' &&
            Encoded[2] == 'C' && Encoded[3] == 'H' && Encoded[4] == 1 && Encoded[5] == 0 &&
            Encoded[6] == 0 && Encoded[7] == 0, "air chunk header");
        Verify(Encoded[12] == 254 && Encoded[13] == 255 && Encoded[27] == 0, "signed little-endian coordinates");

        CheckError(DecodeChunk(Encoded, Key, OneUnit), ChunkCodecError::InvalidBinding, "spacing mismatch");
        CheckError(DecodeChunk(Encoded, {-2, 3, -6}, HalfUnit), ChunkCodecError::InvalidBinding, "key mismatch");
        CheckError(DecodeChunk(Encoded, Key, GridSpec{3}), ChunkCodecError::InvalidBinding, "invalid expected grid");
        CheckError(DecodeChunk(Encoded, {1LL << 60, 0, 0}, HalfUnit), ChunkCodecError::InvalidBinding,
            "oversize expected key");
        const std::array OtherKey{ChunkKey{-1, 3, -7}};
        const auto Other = Source.Capture(OtherKey);
        Verify(Other.has_value(), "unrelated immutable capture");
        CheckError(EncodeChunk(*Other, Key), ChunkCodecError::UncapturedChunk, "uncovered source");

        auto Future = Encoded;
        Future[4] = 2;
        CheckError(DecodeChunk(Future, Key, HalfUnit), ChunkCodecError::UnsupportedVersion, "future version");

        auto Trailing = Encoded;
        Trailing.push_back(0);
        CheckError(DecodeChunk(Trailing, Key, HalfUnit), ChunkCodecError::Malformed, "trailing undeclared bytes");

        auto Redundant = Encoded;
        Redundant[6] = 2;
        Redundant.resize(ChunkArtifactHeaderBytes + 64, 0);
        SetLittleEndian(Redundant, 8, 64, 4);
        CheckError(DecodeChunk(Redundant, Key, HalfUnit), ChunkCodecError::NonCanonical, "bricked air alias rejected");

        ByteVector OverLimit(MaximumChunkArtifactBytes + 1, 0);
        CheckError(DecodeChunk(OverLimit, Key, HalfUnit), ChunkCodecError::LimitExceeded, "encoded byte limit");

        std::stop_source Stop;
        Stop.request_stop();
        CheckError(DecodeChunk(Encoded, Key, HalfUnit, Stop.get_token()), ChunkCodecError::Canceled, "decode canceled");
        const std::array Keys{Key};
        const auto Captured = Source.Capture(Keys);
        Verify(Captured.has_value(), "capture air");
        CheckError(EncodeChunk(*Captured, Key, Stop.get_token()), ChunkCodecError::Canceled, "encode canceled");
    }

    void TestUniform() {
        Field Source(TwoUnits);
        std::vector<SampleWrite> Writes;
        for (std::size_t Brick = 0; Brick < 64; ++Brick)
            for (std::size_t Local = 0; Local < BrickSamples; ++Local)
                Writes.push_back({PointAt(Key, Brick, Local), {-32767, 0xFEDCBA98u}});
        Apply(Source, Key, Writes);
        const auto Encoded = RoundTrip(Source, Key);
        Verify(Encoded.size() == ChunkArtifactHeaderBytes + 6 && Encoded[6] == 1 && Encoded[7] == 2,
            "constant chunk packing");
        Verify(Encoded[38] == 0x98 && Encoded[41] == 0xFE, "full stable material id on wire");

        auto InvalidScalar = Encoded;
        InvalidScalar[36] = 0;
        InvalidScalar[37] = 0x80;
        CheckError(DecodeChunk(InvalidScalar, Key, TwoUnits), ChunkCodecError::Malformed, "reserved density");

        auto InvalidMaterial = Encoded;
        SetLittleEndian(InvalidMaterial, 38, 0, 4);
        CheckError(DecodeChunk(InvalidMaterial, Key, TwoUnits), ChunkCodecError::Malformed, "solid without material");
        std::cout << "[Terrain:ChunkCodec] uniform artifact bytes " << Encoded.size() << '\n';
    }

    void TestBrickModes() {
        Field Source;
        std::vector<SampleWrite> Writes;
        constexpr std::array<MaterialId, 3> Palette{7, 12345, 0x80000003u};
        for (std::size_t Local = 0; Local < BrickSamples; ++Local) {
            Writes.push_back({PointAt(Key, 0, Local), {-32767, 0xFEDCBA98u}});
            if (Local != BrickSamples - 1)
                Writes.push_back({PointAt(Key, 2, Local), {static_cast<std::int16_t>(-100 - static_cast<int>(Local % 7)),
                    Palette[Local % Palette.size()]}});
            Writes.push_back({PointAt(Key, 3, Local), {-5, static_cast<MaterialId>(0x90000000u + Local)}});
            Writes.push_back({PointAt(Key, 5, Local), {12, 0}});
        }
        Writes.push_back({PointAt(Key, 1, 8), {-7, 0xF0000059u}});
        Writes.push_back({PointAt(Key, 1, 500), {0, 0}});
        std::ranges::reverse(Writes);
        Apply(Source, Key, Writes);

        const auto Encoded = RoundTrip(Source, Key);
        constexpr std::size_t Constant = ChunkArtifactHeaderBytes;
        constexpr std::size_t Sparse = Constant + 7;
        constexpr std::size_t Packed = Sparse + 1 + 64 + 6 * 2;
        constexpr std::size_t PaletteCount = Packed + 1 + BrickSamples * 2;
        constexpr std::size_t PaletteIds = PaletteCount + 2;
        constexpr std::size_t PackedIndices = PaletteIds + 3 * 4;
        constexpr std::size_t Raw = PackedIndices + 128;
        constexpr std::size_t Air = Raw + 1 + BrickSamples * 6;
        Verify(Encoded[6] == 2 && Encoded[Constant] == 1 && Encoded[Sparse] == 4 && Encoded[Packed] == 2 &&
            Encoded[Raw] == 3 && Encoded[Air] == 0 && Encoded[Air + 1] == 1, "all canonical brick tags");

        for (std::size_t I = 0; I < Encoded.size(); ++I)
            Verify(!DecodeChunk(std::span(Encoded).first(I), Key, OneUnit), "every strict prefix rejected");

        constexpr std::array Truncations{Constant + 5, Sparse + 42, Packed + 205, PaletteCount + 1,
            PaletteIds + 5, PackedIndices + 23, Raw + 1050, Air + 6};
        for (const auto Cutoff : Truncations) {
            auto Truncated = Encoded;
            Truncated.resize(Cutoff);
            SetLittleEndian(Truncated, 8, static_cast<std::uint32_t>(Cutoff - ChunkArtifactHeaderBytes), 4);
            CheckError(DecodeChunk(Truncated, Key, OneUnit), ChunkCodecError::Malformed,
                "truncation with plausible outer length");
        }

        auto InvalidPalette = Encoded;
        SetLittleEndian(InvalidPalette, PaletteIds + 4, Palette[0], 4);
        CheckError(DecodeChunk(InvalidPalette, Key, OneUnit), ChunkCodecError::NonCanonical, "duplicate palette id");
        InvalidPalette = Encoded;
        SetLittleEndian(InvalidPalette, PaletteCount, 513, 2);
        CheckError(DecodeChunk(InvalidPalette, Key, OneUnit), ChunkCodecError::Malformed, "oversize brick palette");
        InvalidPalette = Encoded;
        InvalidPalette[PackedIndices] |= 3u;
        CheckError(DecodeChunk(InvalidPalette, Key, OneUnit), ChunkCodecError::Malformed, "invalid packed index");
        InvalidPalette = Encoded;
        InvalidPalette[Raw - 1] |= 128u;
        CheckError(DecodeChunk(InvalidPalette, Key, OneUnit), ChunkCodecError::NonCanonical, "nonzero unused bits");
        InvalidPalette = Encoded;
        std::fill_n(InvalidPalette.begin() + PackedIndices, 128, std::uint8_t{0});
        CheckError(DecodeChunk(InvalidPalette, Key, OneUnit), ChunkCodecError::NonCanonical, "unused palette members");

        auto InvalidSparse = Encoded;
        InvalidSparse[Sparse + 1 + 64] = 255;
        InvalidSparse[Sparse + 1 + 64 + 1] = 127;
        SetLittleEndian(InvalidSparse, Sparse + 1 + 64 + 2, 0, 4);
        CheckError(DecodeChunk(InvalidSparse, Key, OneUnit), ChunkCodecError::NonCanonical, "sparse explicit default");

        std::cout << "[Terrain:ChunkCodec] mixed artifact bytes " << Encoded.size() << '\n';
    }

    void TestExtentAndMaximum() {
        constexpr auto Coordinate = -MaximumSampleCoordinate;
        constexpr auto Chunk = GetChunkKey({Coordinate, Coordinate, Coordinate});
        Field Edge;
        const std::array<SampleWrite, 1> AtLimit{{{{Coordinate, Coordinate, Coordinate}, {-8, 211}}}};
        Apply(Edge, Chunk, AtLimit);
        const auto Encoded = RoundTrip(Edge, Chunk, 31 * 31 * 31);
        auto InvalidOutside = Encoded;
        InvalidOutside[6] = 1;
        InvalidOutside.resize(ChunkArtifactHeaderBytes + 6);
        SetLittleEndian(InvalidOutside, 8, 6, 4);
        SetLittleEndian(InvalidOutside, ChunkArtifactHeaderBytes, 65528, 2);
        SetLittleEndian(InvalidOutside, ChunkArtifactHeaderBytes + 2, 211, 4);
        CheckError(DecodeChunk(InvalidOutside, Chunk, OneUnit), ChunkCodecError::NonCanonical,
            "constant extends outside owned domain");

        Field Dense;
        std::vector<SampleWrite> Writes;
        for (std::size_t Brick = 0; Brick < 64; ++Brick)
            for (std::size_t Local = 0; Local < BrickSamples; ++Local)
                Writes.push_back({PointAt(Key, Brick, Local), {-2,
                    static_cast<MaterialId>(Brick * BrickSamples + Local + 1)}});
        Apply(Dense, Key, Writes);
        const auto Biggest = RoundTrip(Dense, Key);
        Verify(Biggest.size() == MaximumChunkArtifactBytes, "fully distinct solid materials hit size bound");
        std::cout << "[Terrain:ChunkCodec] largest artifact bytes " << Biggest.size() << '\n';
    }
}

int main() {
    TestBindingAndAir();
    TestUniform();
    TestBrickModes();
    TestExtentAndMaximum();
    std::cout << "[Terrain:ChunkCodec] canonical round-trip, replacement, and denial checks passed\n";
}

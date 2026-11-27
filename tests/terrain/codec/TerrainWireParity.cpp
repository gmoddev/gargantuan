// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/ChunkCodec.hpp"
#include "gargantuan/terrain/Manifest.hpp"

#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
    using namespace gargantuan;
    using namespace gargantuan::terrain;

    constexpr ProjectId Project{{0xFA, 0x07, 0x21}};
    constexpr TerrainDatasetId Dataset{0x15AF94282A27ull, 0x1236AB10FAull};
    constexpr GridSpec Grid{-1};
    constexpr ChunkKey Air{-2, 0, 0};
    constexpr ChunkKey Uniform{-1, 0, 0};
    constexpr ChunkKey Mixed{2, -1, 3};

    void Require(bool Condition, std::string_view Explanation) {
        if (!Condition) {
            std::cerr << "[Terrain:WireParity] " << Explanation << '\n';
            std::exit(2);
        }
    }

    void Write(const char *Filename, std::span<const std::uint8_t> Bytes) {
        std::ofstream File(Filename, std::ios::binary | std::ios::trunc);
        File.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
        File.close();
        Require(!File.fail(), "write artifact into test directory");
    }

    SamplePoint At(ChunkKey Key, int X, int Y, int Z) {
        return {Key.X * ChunkSide + X, Key.Y * ChunkSide + Y, Key.Z * ChunkSide + Z};
    }

    void Apply(Field &Target, ChunkKey Key, std::span<const SampleWrite> Values) {
        const std::array Keys{Key};
        auto Snapshot = Target.Capture(Keys);
        Require(Snapshot.has_value(), "capture fixture range");
        auto Edit = Field::Prepare(std::move(*Snapshot), Values);
        Require(Edit.has_value() && Target.Commit(std::move(*Edit)).has_value(), "commit wire fixture");
    }

    void PrepareLeaves() {
        Field Source(Grid);
        std::vector<SampleWrite> Values;
        for (int Z = 0; Z < ChunkSide; ++Z)
            for (int Y = 0; Y < ChunkSide; ++Y)
                for (int X = 0; X < ChunkSide; ++X)
                    Values.push_back({At(Uniform, X, Y, Z), {-32000, 0xFEDCBA98u}});
        Apply(Source, Uniform, Values);
        Values.clear();
        for (int Z = 0; Z < BrickSide; ++Z) {
            for (int Y = 0; Y < BrickSide; ++Y) {
                for (int X = 0; X < BrickSide; ++X) {
                    const int Local = (Z * BrickSide + Y) * BrickSide + X;
                    const auto PaletteMaterial = static_cast<MaterialId>(7u + static_cast<unsigned>(Local % 3) * 91u);
                    const auto UniqueMaterial = 0xA7000300u + static_cast<MaterialId>(Local);
                    if (Local % 7 != 0)
                        Values.push_back({At(Mixed, X, Y, Z), {static_cast<std::int16_t>(-300 + Local % 13), PaletteMaterial}});
                    Values.push_back({At(Mixed, X + BrickSide, Y, Z), {-1400, UniqueMaterial}});
                }
            }
        }
        Values.push_back({At(Mixed, 28, 2, 19), {-1, 0x80000011u}});
        Values.push_back({At(Mixed, 31, 0, 31), {0, 0}});
        Apply(Source, Mixed, Values);

        const std::array Keys{Air, Uniform, Mixed};
        const auto Snapshot = Source.Capture(Keys);
        Require(Snapshot.has_value(), "capture all parity leaves");
        constexpr std::array Filenames{"Air.gtch", "Uniform.gtch", "Mixed.gtch"};
        for (std::size_t I = 0; I < Keys.size(); ++I) {
            const auto Encoded = EncodeChunk(*Snapshot, Keys[I]);
            Require(Encoded.has_value(), "encode parity leaf");
            Write(Filenames[I], *Encoded);
        }
    }

    TerrainChunkRef ReadArtifactReference(ChunkKey Key, const char *Filename, std::string_view Hash) {
        Require(Hash.size() == 64, "collect 64 hexadecimal digits from SHA-256 independently");
        TerrainChunkRef Result{Key, {}, 0};
        for (std::size_t I = 0; I < Result.Digest.Bytes.size(); ++I) {
            const auto Start = Hash.data() + I * 2;
            unsigned Byte = 0;
            const auto Parsed = std::from_chars(Start, Start + 2, Byte, 16);
            Require(Parsed.ec == std::errc{} && Parsed.ptr == Start + 2 && Byte <= 255, "invalid digest digit");
            Result.Digest.Bytes[I] = static_cast<std::uint8_t>(Byte);
        }
        const auto Size = std::filesystem::file_size(Filename);
        Require(Size <= MaximumChunkArtifactBytes, "oversized encoded parity leaf");
        Result.ByteCount = static_cast<std::uint32_t>(Size);
        return Result;
    }
}

int main(int Count, char **Arguments) {
    if (Count == 1) {
        PrepareLeaves();
    } else {
        Require(Count == 3, "manifest run requires the uniform and mixed SHA-256 digests");
        const TerrainManifest Value{Project, Dataset, Grid, {
            ReadArtifactReference(Uniform, "Uniform.gtch", Arguments[1]),
            ReadArtifactReference(Mixed, "Mixed.gtch", Arguments[2]),
        }};
        const auto Encoded = EncodeTerrainManifest(Value);
        Require(Encoded.has_value(), "encode parity manifest");
        Write("Terrain.gtmf", *Encoded);
    }
}

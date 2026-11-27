// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/ChunkCodec.hpp"

#include "ArtifactBytes.hpp"
#include "gargantuan/Profiler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <new>

namespace gargantuan::terrain {
    namespace {
        constexpr std::uint32_t Magic = 0x48435447; // GTCH
        constexpr std::size_t BrickAxisCount = ChunkSide / BrickSide;
        constexpr std::size_t BrickCount = BrickAxisCount * BrickAxisCount * BrickAxisCount;
        constexpr std::size_t BrickSampleCount = BrickSide * BrickSide * BrickSide;
        constexpr std::size_t MaskSize = BrickSampleCount / 8;
        constexpr std::size_t ChunkSampleCount = BrickCount * BrickSampleCount;
        constexpr std::size_t ChunkKindOffset = 6;
        constexpr std::size_t BodyLengthOffset = 8;
        static_assert(ChunkSide == 32 && BrickSide == 8 && sizeof(MaterialId) == 4);
        static_assert(ChunkSampleCount <= MaximumEditSamples);

        using Tile = std::array<Sample, BrickSampleCount>;
        using Mask = std::array<std::uint8_t, MaskSize>;

        enum class ChunkKind : std::uint8_t { Air, Constant, Bricks };
        enum class BrickKind : std::uint8_t { Air, Constant, Packed, Raw, Sparse };

        [[nodiscard]] bool IsV1Grid(GridSpec Grid) noexcept {
            return Grid.IsValid() && Grid.SpacingShift >= -1 && Grid.SpacingShift <= 1;
        }

        using artifact_detail::Reader;
        using artifact_detail::Writer;

        SamplePoint GetSamplePoint(ChunkKey Key, std::size_t Brick, std::size_t Index) noexcept {
            return {
                Key.X * ChunkSide + static_cast<std::int64_t>(Brick % BrickAxisCount * BrickSide + Index % BrickSide),
                Key.Y * ChunkSide + static_cast<std::int64_t>(Brick / BrickAxisCount % BrickAxisCount * BrickSide +
                    Index / BrickSide % BrickSide),
                Key.Z * ChunkSide + static_cast<std::int64_t>(Brick / BrickAxisCount / BrickAxisCount * BrickSide +
                    Index / BrickSide / BrickSide),
            };
        }

        void WriteSample(Writer &Output, Sample Value) {
            Output.Add(std::bit_cast<std::uint16_t>(Value.Density));
            Output.Add(Value.Material);
        }

        bool ReadDensity(Reader &Input, Sample &Value) {
            std::uint16_t Density = 0;
            if (!Input.Read(Density))
                return false;
            Value.Density = std::bit_cast<std::int16_t>(Density);
            return Value.Density >= -SaturatedDensity;
        }

        bool ReadSample(Reader &Input, Sample &Value) {
            return ReadDensity(Input, Value) && Input.Read(Value.Material) && terrain::IsValid(Value);
        }

        unsigned GetBitWidth(std::size_t Count) {
            return Count < 2 ? 0 : static_cast<unsigned>(std::bit_width(Count - 1));
        }

        struct TileLayout {
            BrickKind Kind{};
            std::array<MaterialId, BrickSampleCount> Palette;
            std::size_t PaletteSize = 0;
        };

        void Classify(const Tile &Samples, TileLayout &Layout) {
            if (std::ranges::all_of(Samples, [&](Sample Value) { return Value == Samples.front(); })) {
                Layout.Kind = Samples.front() == AirSample ? BrickKind::Air : BrickKind::Constant;
                Layout.PaletteSize = 0;
                return;
            }

            std::size_t SolidCount = 0;
            std::size_t SparseCount = 0;
            for (const Sample Value : Samples) {
                SparseCount += Value != AirSample;
                if (Value.Density < 0)
                    Layout.Palette[SolidCount++] = Value.Material;
            }
            const auto End = Layout.Palette.begin() + SolidCount;
            std::sort(Layout.Palette.begin(), End);
            Layout.PaletteSize = static_cast<std::size_t>(std::unique(Layout.Palette.begin(), End) -
                Layout.Palette.begin());

            constexpr std::size_t DensityBytes = BrickSampleCount * sizeof(std::int16_t);
            const auto RawBytes = DensityBytes + SolidCount * sizeof(MaterialId);
            const auto PackedBytes = DensityBytes + sizeof(std::uint16_t) + Layout.PaletteSize * sizeof(MaterialId) +
                (SolidCount * GetBitWidth(Layout.PaletteSize) + 7) / 8;
            const auto SparseBytes = MaskSize + SparseCount * (sizeof(std::int16_t) + sizeof(MaterialId));

            Layout.Kind = PackedBytes <= RawBytes ? BrickKind::Packed : BrickKind::Raw;
            if (SparseBytes < std::min(RawBytes, PackedBytes))
                Layout.Kind = BrickKind::Sparse;
        }

        void EncodePacked(Writer &Output, const Tile &Samples, const TileLayout &Layout) {
            const auto Palette = std::span(Layout.Palette).first(Layout.PaletteSize);
            Output.Add(static_cast<std::uint16_t>(Palette.size()));
            for (MaterialId Material : Palette)
                Output.Add(Material);

            const unsigned Width = GetBitWidth(Palette.size());
            std::uint32_t Pending = 0;
            unsigned PendingBits = 0;
            for (Sample Value : Samples) {
                if (Value.Density >= 0)
                    continue;
                const auto Index = static_cast<std::uint32_t>(std::ranges::lower_bound(Palette, Value.Material) -
                    Palette.begin());
                Pending |= Index << PendingBits;
                PendingBits += Width;
                while (PendingBits >= 8) {
                    Output.Add(static_cast<std::uint8_t>(Pending & 255u));
                    Pending >>= 8u;
                    PendingBits -= 8u;
                }
            }
            if (PendingBits != 0)
                Output.Add(static_cast<std::uint8_t>(Pending));
        }

        [[nodiscard]] bool HasMaskBit(const Mask &Bits, std::size_t Index) noexcept {
            return (Bits[Index / 8] & (1u << (Index % 8))) != 0;
        }

        void EncodeBrick(Writer &Output, const Tile &Samples) {
            TileLayout Layout;
            Classify(Samples, Layout);
            Output.Add(static_cast<std::uint8_t>(Layout.Kind));
            switch (Layout.Kind) {
            case BrickKind::Air:
                break;
            case BrickKind::Constant:
                WriteSample(Output, Samples.front());
                break;
            case BrickKind::Sparse: {
                Mask Bits{};
                for (std::size_t I = 0; I < Samples.size(); ++I)
                    if (Samples[I] != AirSample)
                        Bits[I / 8] |= static_cast<std::uint8_t>(1u << (I % 8));
                for (std::uint8_t Byte : Bits)
                    Output.Add(Byte);
                for (std::size_t I = 0; I < Samples.size(); ++I)
                    if (HasMaskBit(Bits, I))
                        WriteSample(Output, Samples[I]);
                break;
            }
            default:
                for (Sample Value : Samples)
                    Output.Add(std::bit_cast<std::uint16_t>(Value.Density));
                if (Layout.Kind == BrickKind::Packed) {
                    EncodePacked(Output, Samples, Layout);
                } else {
                    for (Sample Value : Samples)
                        if (Value.Density < 0)
                            Output.Add(Value.Material);
                }
                break;
            }
        }

        std::expected<std::uint16_t, ChunkCodecError> DecodePacked(Reader &Input, Tile &Samples) {
            std::uint16_t Count = 0;
            if (!Input.Read(Count) || Count == 0 || Count > Samples.size())
                return std::unexpected(ChunkCodecError::Malformed);

            std::array<MaterialId, BrickSampleCount> Palette;
            MaterialId Last = 0;
            for (std::size_t I = 0; I < Count; ++I) {
                if (!Input.Read(Palette[I]))
                    return std::unexpected(ChunkCodecError::Malformed);
                if (Palette[I] <= Last)
                    return std::unexpected(ChunkCodecError::NonCanonical);
                Last = Palette[I];
            }

            const unsigned Width = GetBitWidth(Count);
            const auto IndexMask = (1u << Width) - 1;
            std::uint32_t Pending = 0;
            unsigned PendingBits = 0;
            for (Sample &Value : Samples) {
                if (Value.Density >= 0)
                    continue;
                while (PendingBits < Width) {
                    std::uint8_t Byte = 0;
                    if (!Input.Read(Byte))
                        return std::unexpected(ChunkCodecError::Malformed);
                    Pending |= static_cast<std::uint32_t>(Byte) << PendingBits;
                    PendingBits += 8;
                }
                const auto Index = Pending & IndexMask;
                if (Index >= Count)
                    return std::unexpected(ChunkCodecError::Malformed);
                Value.Material = Palette[Index];
                Pending >>= Width;
                PendingBits -= Width;
            }
            if (Pending != 0)
                return std::unexpected(ChunkCodecError::NonCanonical);
            return Count;
        }

        std::expected<void, ChunkCodecError> DecodeBrick(Reader &Input, Tile &Samples) {
            std::uint8_t Tag = 0;
            if (!Input.Read(Tag) || Tag > static_cast<std::uint8_t>(BrickKind::Sparse))
                return std::unexpected(ChunkCodecError::Malformed);
            const auto Kind = static_cast<BrickKind>(Tag);
            std::uint16_t PaletteSize = 0;
            if (Kind == BrickKind::Air || Kind == BrickKind::Constant) {
                Sample Value = AirSample;
                if (Kind == BrickKind::Constant && !ReadSample(Input, Value))
                    return std::unexpected(ChunkCodecError::Malformed);
                Samples.fill(Value);
            } else if (Kind == BrickKind::Sparse) {
                Mask Bits;
                for (auto &Byte : Bits)
                    if (!Input.Read(Byte))
                        return std::unexpected(ChunkCodecError::Malformed);
                Samples.fill(AirSample);
                for (std::size_t I = 0; I < Samples.size(); ++I) {
                    if (!HasMaskBit(Bits, I))
                        continue;
                    if (!ReadSample(Input, Samples[I]))
                        return std::unexpected(ChunkCodecError::Malformed);
                    if (Samples[I] == AirSample)
                        return std::unexpected(ChunkCodecError::NonCanonical);
                }
            } else {
                for (Sample &Value : Samples) {
                    Value.Material = 0;
                    if (!ReadDensity(Input, Value))
                        return std::unexpected(ChunkCodecError::Malformed);
                }
                if (Kind == BrickKind::Packed) {
                    const auto ParsedPalette = DecodePacked(Input, Samples);
                    if (!ParsedPalette)
                        return std::unexpected(ParsedPalette.error());
                    PaletteSize = *ParsedPalette;
                } else {
                    for (Sample &Value : Samples)
                        if (Value.Density < 0 && (!Input.Read(Value.Material) || Value.Material == 0))
                            return std::unexpected(ChunkCodecError::Malformed);
                }
            }

            TileLayout Layout;
            Classify(Samples, Layout);
            if (Layout.Kind != Kind || (Kind == BrickKind::Packed && Layout.PaletteSize != PaletteSize))
                return std::unexpected(ChunkCodecError::NonCanonical);
            return {};
        }

        struct ChunkConstant {
            Sample First = AirSample;
            bool Uniform = true;

            void Observe(Sample Value, std::size_t Brick) {
                if (Brick == 0)
                    First = Value;
                Uniform = Uniform && Value == First;
            }

            void Observe(const Tile &Samples, std::size_t Brick) {
                Observe(Samples.front(), Brick);
                if (Uniform)
                    Uniform = std::ranges::all_of(Samples, [&](Sample Value) { return Value == First; });
            }
        };
    }

    std::expected<std::vector<std::uint8_t>, ChunkCodecError> EncodeChunk(
        const Snapshot &Source, ChunkKey Key, std::stop_token Stop) {
        G_PROFILE("Terrain::EncodeChunk");
        if (Stop.stop_requested())
            return std::unexpected(ChunkCodecError::Canceled);
        if (!IsV1Grid(Source.GetGrid()) || !terrain::IsValid(Key))
            return std::unexpected(ChunkCodecError::InvalidBinding);
        if (!Source.GetVersion(Key))
            return std::unexpected(ChunkCodecError::UncapturedChunk);

        try {
            Writer Output;
            Output.Add(Magic);
            Output.Add(ChunkArtifactVersion);
            Output.Add(static_cast<std::uint8_t>(ChunkKind::Bricks));
            Output.Add(static_cast<std::uint8_t>(Source.GetGrid().SpacingShift + 1));
            Output.Add(std::uint32_t{0});
            Output.Add(std::bit_cast<std::uint64_t>(Key.X));
            Output.Add(std::bit_cast<std::uint64_t>(Key.Y));
            Output.Add(std::bit_cast<std::uint64_t>(Key.Z));

            Tile Samples;
            ChunkConstant Constant;
            for (std::size_t Brick = 0; Brick < BrickCount; ++Brick) {
                if (Stop.stop_requested())
                    return std::unexpected(ChunkCodecError::Canceled);
                const auto BrickData = Source.ReadBrick(Key, Brick, Samples);
                if (!BrickData)
                    return std::unexpected(ChunkCodecError::UncapturedChunk);
                if (*BrickData) {
                    const auto Value = **BrickData;
                    Constant.Observe(Value, Brick);
                    const auto Kind = Value == AirSample ? BrickKind::Air : BrickKind::Constant;
                    Output.Add(static_cast<std::uint8_t>(Kind));
                    if (Kind == BrickKind::Constant)
                        WriteSample(Output, Value);
                } else {
                    Constant.Observe(Samples, Brick);
                    EncodeBrick(Output, Samples);
                }
            }
            if (Stop.stop_requested())
                return std::unexpected(ChunkCodecError::Canceled);
            if (Constant.Uniform) {
                Output.Bytes.resize(ChunkArtifactHeaderBytes);
                const auto Kind = Constant.First == AirSample ? ChunkKind::Air : ChunkKind::Constant;
                Output.Put(ChunkKindOffset, static_cast<std::uint8_t>(Kind));
                if (Kind == ChunkKind::Constant)
                    WriteSample(Output, Constant.First);
            }
            Output.Put(BodyLengthOffset, static_cast<std::uint32_t>(Output.Bytes.size() - ChunkArtifactHeaderBytes));
            return std::move(Output.Bytes);
        } catch (const std::bad_alloc &) {
            return std::unexpected(ChunkCodecError::OutOfMemory);
        }
    }

    std::expected<DecodedChunk, ChunkCodecError> DecodeChunk(std::span<const std::uint8_t> Bytes,
        ChunkKey ExpectedKey, GridSpec ExpectedGrid, std::stop_token Stop) {
        G_PROFILE("Terrain::DecodeChunk");
        if (Stop.stop_requested())
            return std::unexpected(ChunkCodecError::Canceled);
        if (!terrain::IsValid(ExpectedKey) || !IsV1Grid(ExpectedGrid))
            return std::unexpected(ChunkCodecError::InvalidBinding);
        if (Bytes.size() > MaximumChunkArtifactBytes)
            return std::unexpected(ChunkCodecError::LimitExceeded);

        Reader Input(Bytes);
        std::uint32_t ReadMagic = 0, BodyBytes = 0;
        std::uint16_t Version = 0;
        std::uint8_t Tag = 0, Spacing = 0;
        std::uint64_t X = 0, Y = 0, Z = 0;
        if (!Input.Read(ReadMagic) || ReadMagic != Magic || !Input.Read(Version))
            return std::unexpected(ChunkCodecError::Malformed);
        if (Version != ChunkArtifactVersion)
            return std::unexpected(ChunkCodecError::UnsupportedVersion);
        if (!Input.Read(Tag) || Tag > static_cast<std::uint8_t>(ChunkKind::Bricks) ||
            !Input.Read(Spacing) || !Input.Read(BodyBytes) || !Input.Read(X) || !Input.Read(Y) || !Input.Read(Z) ||
            Input.GetRemaining() != BodyBytes)
            return std::unexpected(ChunkCodecError::Malformed);

        const ChunkKey Key{std::bit_cast<std::int64_t>(X), std::bit_cast<std::int64_t>(Y),
            std::bit_cast<std::int64_t>(Z)};
        if (Key != ExpectedKey || Spacing != static_cast<unsigned>(ExpectedGrid.SpacingShift + 1))
            return std::unexpected(ChunkCodecError::InvalidBinding);

        try {
            const auto Kind = static_cast<ChunkKind>(Tag);
            Tile Samples;
            if (Kind != ChunkKind::Bricks) {
                Sample Value = AirSample;
                if (Kind == ChunkKind::Constant) {
                    if (!ReadSample(Input, Value))
                        return std::unexpected(ChunkCodecError::Malformed);
                    if (Value == AirSample)
                        return std::unexpected(ChunkCodecError::NonCanonical);
                }
                if (Input.GetRemaining() != 0)
                    return std::unexpected(ChunkCodecError::Malformed);
                Samples.fill(Value);
            } else if (Input.GetRemaining() < BrickCount) {
                return std::unexpected(ChunkCodecError::Malformed);
            }

            DecodedChunk Output{Key, ExpectedGrid, {}};
            Output.Writes.reserve(ChunkSampleCount);
            ChunkConstant Constant;
            for (std::size_t Brick = 0; Brick < BrickCount; ++Brick) {
                if (Stop.stop_requested())
                    return std::unexpected(ChunkCodecError::Canceled);
                if (Kind == ChunkKind::Bricks) {
                    if (const auto Error = DecodeBrick(Input, Samples); !Error)
                        return std::unexpected(Error.error());
                    Constant.Observe(Samples, Brick);
                }
                for (std::size_t I = 0; I < Samples.size(); ++I) {
                    const auto Point = GetSamplePoint(Key, Brick, I);
                    if (terrain::IsValid(Point))
                        Output.Writes.push_back({Point, Samples[I]});
                    else if (Samples[I] != AirSample)
                        return std::unexpected(ChunkCodecError::NonCanonical);
                }
            }
            if (Input.GetRemaining() != 0)
                return std::unexpected(ChunkCodecError::Malformed);
            if (Kind == ChunkKind::Bricks && Constant.Uniform)
                return std::unexpected(ChunkCodecError::NonCanonical);
            if (Stop.stop_requested())
                return std::unexpected(ChunkCodecError::Canceled);
            return Output;
        } catch (const std::bad_alloc &) {
            return std::unexpected(ChunkCodecError::OutOfMemory);
        }
    }
}

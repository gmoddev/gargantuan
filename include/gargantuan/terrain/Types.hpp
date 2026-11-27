// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

namespace gargantuan::terrain {
    inline constexpr int ChunkSide = 32;
    inline constexpr int BrickSide = 8;
    inline constexpr std::int64_t MaximumSampleCoordinate = (1LL << 20) - 1;
    inline constexpr std::int16_t SaturatedDensity = 32767;
    using MaterialId = std::uint32_t;
    using Revision = std::uint64_t;

    struct ChunkKey {
        std::int64_t X{}, Y{}, Z{};
        auto operator<=>(const ChunkKey &) const = default;
    };

    struct SamplePoint {
        std::int64_t X{}, Y{}, Z{};
        auto operator<=>(const SamplePoint &) const = default;
    };

    struct WorldPoint {
        double X{}, Y{}, Z{};
    };

    struct SampleBox {
        SamplePoint Min;
        SamplePoint MaxExclusive;
    };

    struct GridSpec {
        // Power-of-two spacing gives an exact lattice transform. The field
        // validates these values separately from the eventual provider profile.
        std::int8_t SpacingShift{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return SpacingShift >= -1 && SpacingShift <= 1;
        }
        [[nodiscard]] constexpr double GetSpacing() const noexcept {
            return SpacingShift < 0 ? 0.5 : (SpacingShift > 0 ? 2.0 : 1.0);
        }
        auto operator<=>(const GridSpec &) const = default;
    };

    struct Sample {
        std::int16_t Density{SaturatedDensity};
        MaterialId Material{};
        auto operator<=>(const Sample &) const = default;
    };
    inline constexpr Sample AirSample{};

    [[nodiscard]] constexpr bool IsValid(Sample Value) noexcept {
        return Value.Density >= -SaturatedDensity &&
            ((Value.Density < 0) == (Value.Material != 0));
    }

    [[nodiscard]] constexpr bool IsValid(SamplePoint Point) noexcept {
        constexpr auto Bound = MaximumSampleCoordinate;
        return Point.X >= -Bound && Point.X <= Bound &&
            Point.Y >= -Bound && Point.Y <= Bound &&
            Point.Z >= -Bound && Point.Z <= Bound;
    }

    [[nodiscard]] constexpr std::int64_t FloorChunk(std::int64_t Coordinate) noexcept {
        const auto Quotient = Coordinate / ChunkSide;
        return Quotient - (Coordinate % ChunkSide < 0);
    }

    [[nodiscard]] constexpr ChunkKey GetChunkKey(SamplePoint Point) noexcept {
        return {FloorChunk(Point.X), FloorChunk(Point.Y), FloorChunk(Point.Z)};
    }

    [[nodiscard]] constexpr bool IsValid(ChunkKey Key) noexcept {
        constexpr auto Minimum = FloorChunk(-MaximumSampleCoordinate);
        constexpr auto Maximum = FloorChunk(MaximumSampleCoordinate);
        return Key.X >= Minimum && Key.X <= Maximum &&
            Key.Y >= Minimum && Key.Y <= Maximum &&
            Key.Z >= Minimum && Key.Z <= Maximum;
    }
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <vector>

namespace gargantuan::terrain::artifact_detail {
    class Writer {
    public:
        std::vector<std::uint8_t> Bytes;

        template <std::unsigned_integral UInt>
        void Add(UInt Value) {
            const auto Size = Bytes.size();
            Bytes.resize(Size + sizeof(UInt));
            Put(Size, Value);
        }

        void Add(std::span<const std::uint8_t> Value) {
            Bytes.insert(Bytes.end(), Value.begin(), Value.end());
        }

        template <std::unsigned_integral UInt>
        void Put(std::size_t Offset, UInt Value) {
            auto Remaining = static_cast<std::uint64_t>(Value);
            for (std::size_t I = 0; I < sizeof(UInt); ++I) {
                Bytes[Offset + I] = static_cast<std::uint8_t>(Remaining & 255u);
                Remaining >>= 8u;
            }
        }
    };

    class Reader {
    public:
        explicit Reader(std::span<const std::uint8_t> Bytes) : Remaining(Bytes) {}

        template <std::unsigned_integral UInt>
        bool Read(UInt &Value) {
            if (Remaining.size() < sizeof(UInt))
                return false;
            std::uint64_t Accumulator = 0;
            for (std::size_t I = 0; I < sizeof(UInt); ++I)
                Accumulator |= static_cast<std::uint64_t>(Remaining[I]) << (I * 8);
            Value = static_cast<UInt>(Accumulator);
            Remaining = Remaining.subspan(sizeof(UInt));
            return true;
        }

        template <std::size_t Size>
        bool Read(std::array<std::uint8_t, Size> &Value) {
            if (Remaining.size() < Size)
                return false;
            std::copy_n(Remaining.begin(), Size, Value.begin());
            Remaining = Remaining.subspan(Size);
            return true;
        }

        [[nodiscard]] std::size_t GetRemaining() const noexcept { return Remaining.size(); }

    private:
        std::span<const std::uint8_t> Remaining;
    };
}

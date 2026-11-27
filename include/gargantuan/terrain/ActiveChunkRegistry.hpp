// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Field.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace gargantuan::terrain::detail {
    // The scheduler pairs this identity with an immutable field Snapshot and
    // its own work/request sequence. Snapshot dependencies validate separately
    // from a chunk's activity and local geometry/material version.
    struct ChunkWorkTicket {
        ChunkKey Key;
        std::uint64_t WorldEpoch{};
        std::uint64_t RegistryIdentity{};
        std::uint64_t Generation{};
        ChunkVersion Version;

        auto operator<=>(const ChunkWorkTicket &) const = default;
    };

    // Main-owned derived records. Dense rows may relocate on retirement. Only
    // value tickets are stable across Main phases or worker handoffs.
    class ActiveChunkRegistry final {
    public:
        ActiveChunkRegistry(std::size_t Capacity, std::uint64_t WorldEpoch)
            : Capacity(Capacity), WorldEpoch(WorldEpoch), RegistryIdentity(AllocateRegistryIdentity()) {}

        ActiveChunkRegistry(const ActiveChunkRegistry &) = delete;
        ActiveChunkRegistry &operator=(const ActiveChunkRegistry &) = delete;

        [[nodiscard]] std::optional<ChunkWorkTicket> Activate(ChunkKey Key, ChunkVersion Version = {}) noexcept {
            if (!IsValid(Key) || WorldEpoch == 0 || RegistryIdentity == 0)
                return std::nullopt;
            if (const auto Existing = Capture(Key))
                return Existing->Version == Version ? Existing : std::nullopt;
            if (Keys.size() >= Capacity || NextGeneration == 0)
                return std::nullopt;

            const auto Row = Keys.size();
            try {
                ReserveForRow(Row);
                Rows.emplace(Key, Row);
            } catch (const std::bad_alloc &) {
                return std::nullopt;
            } catch (const std::length_error &) {
                return std::nullopt;
            }
            Keys.push_back(Key);
            Generations.push_back(NextGeneration);
            Geometry.push_back(Version.Geometry);
            Material.push_back(Version.Material);
            NextGeneration = NextGeneration == std::numeric_limits<std::uint64_t>::max() ? 0 : NextGeneration + 1;
            return GetAt(Row);
        }

        [[nodiscard]] bool Update(ChunkKey Key, ChunkVersion Version) noexcept {
            const auto Found = Rows.find(Key);
            if (Found == Rows.end())
                return false;
            const auto Row = Found->second;
            if (Version.Geometry < Geometry[Row] || Version.Material < Material[Row])
                return false;
            Geometry[Row] = Version.Geometry;
            Material[Row] = Version.Material;
            return true;
        }

        [[nodiscard]] bool Retire(ChunkKey Key) noexcept {
            const auto Found = Rows.find(Key);
            if (Found == Rows.end())
                return false;

            const auto Row = Found->second;
            const auto Last = Keys.size() - 1;
            if (Row != Last) {
                Keys[Row] = Keys[Last];
                Generations[Row] = Generations[Last];
                Geometry[Row] = Geometry[Last];
                Material[Row] = Material[Last];
                Rows.find(Keys[Row])->second = Row;
            }
            Keys.pop_back();
            Generations.pop_back();
            Geometry.pop_back();
            Material.pop_back();
            Rows.erase(Found);
            return true;
        }

        [[nodiscard]] std::size_t GetCount() const noexcept { return Keys.size(); }

        [[nodiscard]] std::optional<ChunkWorkTicket> GetAt(std::size_t Row) const noexcept {
            if (Row >= Keys.size())
                return std::nullopt;
            return ChunkWorkTicket{Keys[Row], WorldEpoch, RegistryIdentity, Generations[Row], {Geometry[Row], Material[Row]}};
        }

        [[nodiscard]] std::optional<ChunkWorkTicket> Capture(ChunkKey Key) const noexcept {
            const auto Found = Rows.find(Key);
            return Found == Rows.end() ? std::nullopt : GetAt(Found->second);
        }

        [[nodiscard]] bool IsCurrent(const ChunkWorkTicket &Ticket) const noexcept {
            if (Ticket.RegistryIdentity != RegistryIdentity || Ticket.WorldEpoch != WorldEpoch)
                return false;
            const auto Current = Capture(Ticket.Key);
            return Current && Current->Generation == Ticket.Generation && Current->Version == Ticket.Version;
        }

        [[nodiscard]] bool Reset(std::uint64_t Epoch) noexcept {
            if (Epoch <= WorldEpoch || RegistryIdentity == 0)
                return false;
            Rows.clear();
            Keys.clear();
            Generations.clear();
            Geometry.clear();
            Material.clear();
            WorldEpoch = Epoch;
            NextGeneration = 1;
            return true;
        }

    private:
        struct KeyHash {
            std::size_t operator()(const ChunkKey &Key) const noexcept {
                const auto X = static_cast<std::uint64_t>(Key.X) * 0x9e3779b97f4a7c15ull;
                const auto Y = static_cast<std::uint64_t>(Key.Y) * 0xbf58476d1ce4e5b9ull;
                const auto Z = static_cast<std::uint64_t>(Key.Z) * 0x94d049bb133111ebull;
                return static_cast<std::size_t>(X ^ std::rotl(Y, 21) ^ std::rotl(Z, 42));
            }
        };

        static std::uint64_t AllocateRegistryIdentity() noexcept {
            static std::atomic<std::uint64_t> Next{1};
            auto Candidate = Next.load(std::memory_order_relaxed);
            while (Candidate != std::numeric_limits<std::uint64_t>::max()) {
                if (Next.compare_exchange_weak(Candidate, Candidate + 1, std::memory_order_relaxed))
                    return Candidate;
            }
            return 0;
        }

        void ReserveForRow(std::size_t Row) {
            const auto Existing = Keys.capacity();
            const auto Target = Existing > Row ? Existing : Row + std::min(std::max<std::size_t>(Row, 1), Capacity - Row);
            Keys.reserve(Target);
            Generations.reserve(Target);
            Geometry.reserve(Target);
            Material.reserve(Target);
        }

        const std::size_t Capacity;
        std::uint64_t WorldEpoch;
        const std::uint64_t RegistryIdentity;
        std::uint64_t NextGeneration = 1;

        std::vector<ChunkKey> Keys;
        std::vector<std::uint64_t> Generations;
        std::vector<Revision> Geometry;
        std::vector<Revision> Material;
        std::unordered_map<ChunkKey, std::size_t, KeyHash> Rows;
    };
}

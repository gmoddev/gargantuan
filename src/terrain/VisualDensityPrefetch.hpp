// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "terrain/VisualDensity.hpp"

#include <array>
#include <optional>

namespace gargantuan {
    class JobSystem;
}

namespace gargantuan::terrain {
    struct VisualDensityInterest {
        ChunkKey Key;
        std::shared_ptr<const SurfaceMesh> CommittedFine;
    };

    // One experimental, Main-owned render cache. Advance receives current,
    // prioritized visual interests. It submits at most one immutable worker
    // job and leaves current fine coverage available while the job is pending.
    class VisualDensityPrefetch final {
    public:
        static constexpr std::size_t MaximumInterests = 16;
        static constexpr std::size_t MaximumCachedBytes = 4 * 1024 * 1024;

        explicit VisualDensityPrefetch(JobSystem &Jobs) noexcept : Jobs(Jobs) {}
        ~VisualDensityPrefetch();
        VisualDensityPrefetch(const VisualDensityPrefetch &) = delete;
        VisualDensityPrefetch &operator=(const VisualDensityPrefetch &) = delete;

        [[nodiscard]] bool Advance(std::span<const VisualDensityInterest> Interests) noexcept;
        [[nodiscard]] std::shared_ptr<const SurfaceMesh> Select(ChunkKey Key,
            const std::shared_ptr<const SurfaceMesh> &CommittedFine, bool PreferReduced) const noexcept;
        [[nodiscard]] bool IsPreparing() const noexcept { return Active != nullptr; }
        [[nodiscard]] std::size_t GetCachedBytes() const noexcept { return CachedBytes; }

    private:
        struct Work;
        struct CacheEntry {
            ChunkKey Key;
            std::weak_ptr<const SurfaceMesh> Source;
            std::optional<VisualDensityVariant> Value;
        };

        JobSystem &Jobs;
        std::array<std::optional<CacheEntry>, MaximumInterests> Cache;
        std::size_t CachedBytes{};
        std::shared_ptr<Work> Active;
    };
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/render/RenderPublication.hpp"
#include "terrain/EditCoordinator.hpp"
#include "terrain/VisualDensityPrefetch.hpp"

namespace gargantuan::terrain {
    class RenderStage;

    struct VisualDensityCamera {
        WorldPoint Position;
        WorldPoint Forward;
        double FocalPixels{};
    };

    // Bounds ideal perspective displacement of the exact-collar derivative.
    // An infinite result means this view cannot certify the projected error.
    [[nodiscard]] double GetVisualDensityPixelBound(GridSpec Grid, ChunkKey Key,
        const VisualDensityCamera &View) noexcept;

    struct VisualDensityViewReceipt {
        std::size_t Warmed{};
        std::size_t Evicted{};
        std::size_t Promoted{};
        std::size_t Restored{};
        std::size_t FineReplaced{};
        std::size_t ReducedCount{};
        std::size_t FineRollbackBytes{};
        std::size_t CachedVariantBytes{};
        std::size_t PreparedVariantBytes{};
        bool Preparing{};
    };

    // Main-only experimental adapter for a single RenderStage and view.
    // Supply current committed fine sources for visible interests; normal
    // commits and retirements still run through their original owners first.
    class VisualDensityView final {
    public:
        static constexpr std::size_t MaximumCandidates = VisualDensityPrefetch::MaximumInterests;
        static constexpr std::size_t MaximumPromotions = 2;
        static constexpr std::size_t MaximumFineRollbackBytes = 4 * 1024 * 1024;
        static constexpr double EnterPixels = 1.0;
        static constexpr double ExitPixels = 1.5;
        static constexpr double PrefetchPixels = 2.0;

        VisualDensityView(JobSystem &Jobs, RenderStage &Render) noexcept : Render(Render), Prefetch(Jobs) {}
        // Pass a monotonic view publication ID. Call Advance with no camera
        // before disabling a selector while its RenderStage remains live.
        [[nodiscard]] std::expected<VisualDensityViewReceipt, EditStatus> Advance(GridSpec Grid,
            const std::optional<VisualDensityCamera> &Camera, std::span<const VisualDensityInterest> Visible,
            Revision Publication);

    private:
        RenderStage &Render;
        VisualDensityPrefetch Prefetch;
        std::optional<GridSpec> BoundGrid;
        Revision LastPublication{};
    };
}

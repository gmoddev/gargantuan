// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Surface.hpp"
#include <expected>
#include <memory>
#include <utility>

namespace gargantuan::terrain {
    inline constexpr double MaximumVisualDensityMoveInSamples = 1.25;

    struct VisualDensityMetrics {
        std::size_t Collapses{};
        double MaximumDisplacement{};
        std::size_t OverlapPairs{};
    };

    // Optional render derivative from a current authored mesh. Uses the same
    // source spacing, exact boundary collar and vertex material references.
    // Field samples and native colliders still use the original mesh.
    // Denial and cancellation preserve the caller's previous output.
    [[nodiscard]] SurfaceStatus BuildVisualDensity(const SurfaceMesh &Source, unsigned CellsPerAxis,
        unsigned Passes, SurfaceMesh &Output, VisualDensityMetrics &Metrics, std::stop_token Stop = {});

    // The coordinator's immutable fine mesh identifies the precise geometry,
    // paint and halo values used to build a renderer-only variant.
    class VisualDensityVariant final {
    public:
        [[nodiscard]] static std::expected<VisualDensityVariant, SurfaceStatus> Prepare(
            const std::shared_ptr<const SurfaceMesh> &Fine, std::stop_token Stop = {});
        [[nodiscard]] std::shared_ptr<const SurfaceMesh> Select(
            const std::shared_ptr<const SurfaceMesh> &CurrentFine, bool PreferReduced) const noexcept;
        [[nodiscard]] std::size_t GetOwnedBytes() const noexcept;

    private:
        VisualDensityVariant(const std::shared_ptr<const SurfaceMesh> &Fine, std::shared_ptr<const SurfaceMesh> Reduced)
            : Source(Fine), Reduced(std::move(Reduced)) {}

        std::weak_ptr<const SurfaceMesh> Source;
        std::shared_ptr<const SurfaceMesh> Reduced;
    };
}

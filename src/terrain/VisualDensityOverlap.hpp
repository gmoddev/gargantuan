// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Surface.hpp"

namespace gargantuan::terrain {
    inline constexpr std::size_t MaximumVisualDensityVertices = 65'536;
    inline constexpr std::size_t MaximumVisualDensityIndices = 262'143;

    // Requires chunk-local vertices and one source-provenance flag per face.
    // Faces left unchanged by simplification are only tested against changed faces.
    [[nodiscard]] SurfaceStatus CheckVisualDensityOverlap(const SurfaceMesh &Mesh, unsigned Cells,
        std::span<const std::uint8_t> Changed, std::size_t &TestedPairs, std::stop_token Stop);
}

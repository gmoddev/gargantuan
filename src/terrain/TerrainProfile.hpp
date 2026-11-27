// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>

namespace gargantuan::terrain {
    // The initial offline residency path prepares the whole authored field.
    inline constexpr std::size_t MaximumStartupSourceChunks = 512;
    inline constexpr std::size_t MaximumStartupContourChunks = 1024;
}

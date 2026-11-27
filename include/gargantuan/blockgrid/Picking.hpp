// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include <array>

namespace gargantuan::blockgrid {
	struct CellHit { Cell Position; double Fraction; std::array<double,3> Normal; };
	inline constexpr unsigned MaximumRayVisits = 4096;
	inline constexpr unsigned MaximumRayTriangles = 32'768;
	// Local coordinates in cell units. Direction is the entire bounded segment.
	[[nodiscard]] std::optional<CellHit> Cast(const Field& Grid, std::array<double,3> Origin,
		std::array<double,3> Direction, unsigned Budget = MaximumRayVisits);
}

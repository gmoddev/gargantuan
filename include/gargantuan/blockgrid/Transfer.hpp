// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"

namespace gargantuan::blockgrid {
	// Bounds both grids' edits and the cost of importing project-local declarations.
	inline constexpr std::size_t MaximumTransferCells=4'096, MaximumTransferDefinitions=64;
	struct GridLattice {
		Cell Origin;
		std::uint8_t Orientation{};
	};
	struct TransferFields { Field Source, Destination; };
	enum class TransferMotion : std::uint8_t { Static,Split,Merge };
	// Empty selection is rejected. The returned candidates own no live provider or instance state.
	[[nodiscard]] TransferFields PlanGridTransfer(const Field& Source,const Field& Destination,
		std::optional<Region> Bounds,GridLattice Mapping);
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include <string>

namespace gargantuan::blockgrid {
	struct CellDelta { Cell Position; Block Before, After; };
	struct Patch { std::vector<CellDelta> Cells; };
	[[nodiscard]] std::shared_ptr<const Patch> CapturePatch(const Field& Source, std::span<const Write> Writes);
	[[nodiscard]] std::vector<Write> ReplayWrites(const Field& Source, const Patch& Value, bool Forward);
	struct EditRequest {
		std::vector<Write> Writes;
		std::optional<std::string> ExpectedRevision;
		// Only the trusted authoring history replay path may supply an inverse.
		std::shared_ptr<const Patch> Replay;
		bool Forward{};
	};
}

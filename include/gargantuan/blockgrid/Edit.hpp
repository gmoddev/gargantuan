// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include <string>

namespace gargantuan::blockgrid {
	struct CellDelta { Cell Position; Block Before, After; };
	struct BindingDelta { std::optional<Definition> Before; Definition After; };
	inline constexpr std::size_t MaximumNewMaterials=16;
	struct Patch {
		std::vector<CellDelta> Cells;
		std::optional<BindingDelta> Binding;
		std::vector<StateChange> StateCells;
		std::optional<ChannelSpec> Channel;
		std::vector<Definition> Materials;
	};
	struct BindingEdit { Definition Spec; bool Replace{}; };
	struct MotionEdit { bool Dynamic{}; std::optional<std::pair<Point,Point>> Velocities; };
	struct GridTransferEdit {
		std::optional<Region> Extract;
		ObjectId Source;
		std::string SourceRevision;
	};
	[[nodiscard]] std::shared_ptr<const Patch> CapturePatch(const Field& Source, std::span<const Write> Writes);
	[[nodiscard]] std::vector<Write> ReplayWrites(const Field& Source, const Patch& Value, bool Forward);
	struct EditRequest {
		std::vector<Write> Writes;
		std::optional<std::string> ExpectedRevision;
		// Only the trusted authoring history replay path may supply an inverse.
		std::shared_ptr<const Patch> Replay;
		bool Forward{};
		std::optional<BindingEdit> Binding;
		std::optional<ChannelSpec> Channel;
		std::vector<Operation> Commands;
		std::vector<Definition> Materials;
		std::optional<MotionEdit> Motion;
		std::optional<GridTransferEdit> Bilateral;
	};
}

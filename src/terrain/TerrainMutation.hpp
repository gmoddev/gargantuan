// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "terrain/EditCoordinator.hpp"

namespace gargantuan::terrain {
	struct TerrainMutation {
		EditRequest Request;
		std::optional<Revision> ExpectedRevision;
	};

	[[nodiscard]] constexpr std::string_view GetEditStatusName(EditStatus Status) noexcept {
		switch (Status) {
		case EditStatus::Committed:
			return "Committed";
		case EditStatus::Canceled:
			return "Canceled";
		case EditStatus::InvalidInput:
			return "InvalidInput";
		case EditStatus::LimitExceeded:
			return "LimitExceeded";
		case EditStatus::ReopenCapacityExceeded:
			return "ReopenCapacityExceeded";
		case EditStatus::Stale:
			return "Stale";
		case EditStatus::PreparationFailed:
			return "PreparationFailed";
		case EditStatus::PublicationDenied:
			return "PublicationDenied";
		case EditStatus::Unavailable:
			return "Unavailable";
		}
		return "Unavailable";
	}

	[[nodiscard]] constexpr std::string_view GetEditStatusDiagnostic(EditStatus Status) noexcept {
		switch (Status) {
		case EditStatus::InvalidInput:
			return "Check finite brush dimensions, supported coordinate bounds and the packed sample and material format.";
		case EditStatus::LimitExceeded:
			return "The edit exceeded terrain work or memory capacity. Wait for pending edits and use a smaller brush or region.";
		case EditStatus::ReopenCapacityExceeded:
			return "The edit would exceed the offline terrain startup profile. Clear materialized areas or consolidate disconnected terrain.";
		case EditStatus::Stale:
			return "Terrain changed after this edit was prepared. Read the latest revision before retrying.";
		case EditStatus::PreparationFailed:
			return "The terrain surface or collision mesh could not be prepared. Try a smaller region or simplify the edited feature.";
		case EditStatus::PublicationDenied:
			return "Terrain render or collision resources could not be admitted. Reduce active detail or retry after terrain resources retire.";
		case EditStatus::Unavailable:
			return "The terrain runtime or its installed resources are unavailable.";
		default:
			return {};
		}
	}
}

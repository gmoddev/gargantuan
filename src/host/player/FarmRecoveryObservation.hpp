#pragma once

#include "gargantuan/classes/DataModel.hpp"

namespace gargantuan::host::detail {

// Recovery stage belongs to the replicated DataModel (Luau `game`). The
// CharacterControl ScalePhase mirror belongs to the separate content phases.
[[nodiscard]] inline std::size_t FarmRecoveryCaseIndex(const DataModel &World) {
	const auto Stage = World.GetAttributeValue("ScaleOverloadCase");
	const auto *Name = Stage ? std::get_if<std::string>(&*Stage) : nullptr;
	return Name && *Name == "recover_structural" ? 1 : Name && *Name == "recover_mixed" ? 2 : 0;
}

} // namespace gargantuan::host::detail

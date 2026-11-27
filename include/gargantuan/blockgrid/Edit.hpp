// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/blockgrid/Field.hpp"
#include <string>

namespace gargantuan::blockgrid {
	struct EditRequest {
		std::vector<Write> Writes;
		std::optional<std::string> ExpectedRevision;
	};
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <map>
#include <string>

namespace gargantuan::blockgrid {
	class Field;
	using SharedAssetPins = std::map<std::string, std::string, std::less<>>;
	[[nodiscard]] bool AdmitsAssetPins(const Field &Field, const SharedAssetPins *Shared);
}

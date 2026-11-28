// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>

namespace gargantuan::host {
	// Child-to-parent shared session responses use one fixed size so a grant
	// rotation and a drain receipt cannot be mistaken for one another.
	using SharedHostFrame = std::array<unsigned char, 42>;
	inline std::int64_t InvitationTimeMillis() {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}
}

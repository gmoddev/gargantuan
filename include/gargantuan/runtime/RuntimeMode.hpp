#pragma once

#include <cstdint>

namespace gargantuan {
	enum class RuntimeMode : std::uint8_t {
		Offline,
		NetworkClient,
		NetworkServer,
	};
}

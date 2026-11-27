#pragma once

#include "gargantuan/assets/AssetTypes.hpp"

#include <cstdint>
#include <map>

namespace gargantuan::terrain {
	// Document-local node identity is resolved before the candidate world can be published.
	using DomainTerrainBindings = std::map<std::uint32_t, AssetContentId>;
	inline constexpr std::size_t MaximumDomainTerrainBindings = 64;
}

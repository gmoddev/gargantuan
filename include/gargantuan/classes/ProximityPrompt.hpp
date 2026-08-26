#pragma once

#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/generated/ProximityPrompt.hpp"

#include <cstddef>

namespace gargantuan {
	class ProximityPrompt : public Instance {
		I_ProximityPrompt;

		std::string ActionText = "Interact";
		std::string ObjectText;

	  public:
		static constexpr std::size_t MaximumTextBytes = 64;
		static constexpr float MaximumDistance = 64.0f;
		static constexpr float MaximumHoldDuration = 30.0f;
	};
}

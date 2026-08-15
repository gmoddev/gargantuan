#include "gargantuan/network/Statistics.hpp"

#include <cmath>

namespace gargantuan::network {
	bool NetworkStatistics::IsValid() const {
		if (EstimatedRoundTripTime && EstimatedRoundTripTime->count() < 0) return false;
		return !MessageLossRatio ||
			(std::isfinite(*MessageLossRatio) && *MessageLossRatio >= 0.0 && *MessageLossRatio <= 1.0);
	}
}

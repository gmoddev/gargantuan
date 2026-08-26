#include "gargantuan/environment/EnvironmentSemantics.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace gargantuan {
	EnvironmentSunState ComputeEnvironmentSunState(float ClockTime, float Brightness, const Color3 &SunColor) {
		if (!std::isfinite(ClockTime) || ClockTime < 0.0f || ClockTime >= 24.0f || !std::isfinite(Brightness) ||
			Brightness < 0.0f || Brightness > 8.0f || !std::isfinite(SunColor.R) || !std::isfinite(SunColor.G) ||
			!std::isfinite(SunColor.B))
			throw std::invalid_argument("[Environment:Semantics] Cannot derive a sun from invalid authored state");
		const auto Angle = (ClockTime - 6.0f) * (2.0f * std::numbers::pi_v<float> / 24.0f);
		const glm::vec3 Direction{std::cos(Angle), std::sin(Angle), 0.0f};
		return {
			.Direction = glm::normalize(Direction),
			.Color = glm::vec3(SunColor.R, SunColor.G, SunColor.B),
			.Intensity = Brightness * std::max(Direction.y, 0.0f),
		};
	}

	float ComputeEnvironmentExposure(float ExposureCompensation) {
		if (!std::isfinite(ExposureCompensation) || ExposureCompensation < -8.0f || ExposureCompensation > 8.0f)
			throw std::invalid_argument("[Environment:Semantics] ExposureCompensation must be between -8 and 8 stops");
		const auto Result = std::exp2(ExposureCompensation);
		if (!std::isfinite(Result))
			throw std::invalid_argument("[Environment:Semantics] Exposure multiplier is not finite");
		return Result;
	}
}

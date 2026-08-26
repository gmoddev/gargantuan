#pragma once

#include "gargantuan/datatypes/Color3.hpp"

#include <glm/glm.hpp>

namespace gargantuan {
	struct EnvironmentSunState {
		glm::vec3 Direction{0.0f, 1.0f, 0.0f};
		glm::vec3 Color{1.0f};
		float Intensity = 1.0f;
	};

	[[nodiscard]] EnvironmentSunState
	ComputeEnvironmentSunState(float ClockTime, float Brightness, const Color3 &SunColor);
	[[nodiscard]] float ComputeEnvironmentExposure(float ExposureCompensation);
}

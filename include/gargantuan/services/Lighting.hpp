#pragma once

#include "gargantuan/services/generated/Lighting.hpp"

namespace gargantuan {
	class Lighting final : public Instance {
		I_Lighting;

	  private:
		Color3 Ambient{0.2f, 0.2f, 0.2f};
		Color3 SunColor{1.0f, 1.0f, 1.0f};
		float Brightness = 1.0f;
		float ClockTime = 12.0f;
		float ExposureCompensation = 0.0f;
		Color3 EnvironmentColor{0.08f, 0.12f, 0.2f};
		bool FogEnabled = false;
		Color3 FogColor{0.5f, 0.6f, 0.7f};
		float FogStart = 0.0f;
		float FogEnd = 1000.0f;
	};
}

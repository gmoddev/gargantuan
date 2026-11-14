// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/classes/generated/LocalLight.hpp"

namespace gargantuan {
	class LocalLight : public Instance {
		I_LocalLight;

	  private:
		Color3 LightColor{1.0f, 1.0f, 1.0f};
		float LightIntensity = 16.0f;
		float LightRange = 16.0f;
		CFrame LightCFrame;
	};
}

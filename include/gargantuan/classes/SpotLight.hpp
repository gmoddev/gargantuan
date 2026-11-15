// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/classes/LocalLight.hpp"
#include "gargantuan/classes/generated/SpotLight.hpp"

namespace gargantuan {
	class SpotLight final : public LocalLight {
		I_SpotLight;

	  private:
		Vector2 LightConeAngles{20.0f, 35.0f};
	};
}

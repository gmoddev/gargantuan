#pragma once

#include "gargantuan/math/EasingCurves.hpp"
#include "gargantuan/scripting/Userdata.hpp"

#include <cstdint>
#include <lua.h>

namespace gargantuan {
	G_USERDATA_DECL(
		TweenInfo, using Pointer = std::shared_ptr<TweenInfo>;

		float Time = 1.0f;
		bool Reverses = false;
		int32_t RepeatCount = 0;
		float DelayTime = 0;
		Enums::EasingDirection EasingDirection = Enums::EasingDirection::Out;
		Enums::EasingStyle EasingStyle = Enums::EasingStyle::Quad;

		void Validate() const;

		TweenInfo(
			float time = 1.0f,
			Enums::EasingStyle easingStyle = Enums::EasingStyle::Quad,
			Enums::EasingDirection easingDirection = Enums::EasingDirection::Out,
			int32_t repeatCount = 0,
			bool reverses = false,
			float delayTime = 0
		);
	)
}

#include "gargantuan/datatypes/TweenInfo.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	G_USERDATA_IMPL(
		TweenInfo,
		.Tag = UserdataTag::TweenInfo,
		.Type = "TweenInfo",
		.Properties = {
			{"Time", Property::fromReadonlyMember<&TweenInfo::Time>()},
			{"EasingStyle", Property::fromReadonlyMember<&TweenInfo::EasingStyle>()},
			{"EasingDirection", Property::fromReadonlyMember<&TweenInfo::EasingDirection>()},
			{"RepeatCount", Property::fromReadonlyMember<&TweenInfo::RepeatCount>()},
			{"Reverses", Property::fromReadonlyMember<&TweenInfo::Reverses>()},
			{"DelayTime", Property::fromReadonlyMember<&TweenInfo::DelayTime>()},
			{"TweenInfo", Property::fromReadonlyMember<&TweenInfo::Time>()},
		}
	);

	TweenInfo::TweenInfo(
		float time,
		Enums::EasingStyle easingStyle,
		Enums::EasingDirection easingDirection,
		int32_t repeatCount,
		bool reverses,
		float delayTime
	)
		: Time(time), Reverses(reverses), RepeatCount(repeatCount), DelayTime(delayTime),
		  EasingDirection(easingDirection), EasingStyle(easingStyle) {}

	void TweenInfo::Validate() const {
		if (!std::isfinite(Time) || Time < 0.0f || Time > 86'400.0f ||
			!std::isfinite(DelayTime) || DelayTime < 0.0f || DelayTime > 86'400.0f ||
			RepeatCount < 0 || RepeatCount > 1'000)
			throw std::invalid_argument("[Tween:Info] Time and delay must be in [0, 86400], repeat count in [0, 1000]");
		const auto Style = EasingCurves::CURVES.find(EasingStyle);
		if (Style == EasingCurves::CURVES.end() || !Style->second.contains(EasingDirection))
			throw std::invalid_argument("[Tween:Info] Unknown easing style or direction");
	}
}

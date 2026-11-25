#include "gargantuan/math/EasingCurves.hpp"

#include <cmath>
#include <glm/exponential.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/trigonometric.hpp>

#define CURVE_ENTRY_MANUAL(style, inImpl, outImpl, inOutImpl)                                                          \
	{                                                                                                                  \
		Enums::EasingStyle::style,                                                                                     \
		{                                                                                                              \
			{Enums::EasingDirection::In, inImpl},                                                                      \
			{Enums::EasingDirection::Out, outImpl},                                                                    \
			{Enums::EasingDirection::InOut, inOutImpl},                                                                \
		},                                                                                                             \
	}

#define CURVE_ENTRY(style) CURVE_ENTRY_MANUAL(style, EaseIn##style, EaseOut##style, EaseInOut##style)

#define GENERIC_IN_OUT_IMPL(style)                                                                                     \
	float EaseInOut##style(float x) {                                                                                  \
		return x < 0.5f ? EaseIn##style(2 * x) / 2 : EaseOut##style(2 * x - 1) / 2 + 0.5f;                             \
	}

// Bounce and Elastic implementations are from BoatTween
//   https://github.com/boatbeaker/BoatTween/
// Implementations for other easing curves are from easings.net
//   https://easings.net/
namespace gargantuan::EasingCurves {
	static const float PI = glm::pi<float>();

	// Linear
	float EaseLinear(float x) {
		return x;
	}

	// Sine
	float EaseInSine(float x) {
		return 1 - glm::cos((x * PI) / 2);
	}

	float EaseOutSine(float x) {
		return glm::sin((x * PI / 2));
	}

	float EaseInOutSine(float x) {
		return -(glm::cos(PI * x) - 1) / 2;
	}

	// Back
	static const float BACK_C1 = 1.70158;
	static const float BACK_C2 = BACK_C1 * 1.525;
	static const float BACK_C3 = BACK_C1 + 1;

	float EaseInBack(float x) {
		return BACK_C3 * x * x * x - BACK_C1 * x * x;
	}

	float EaseOutBack(float x) {
		return 1 + BACK_C3 * glm::pow(x - 1, 3) + BACK_C1 * glm::pow(x - 1, 2);
	}

	float EaseInOutBack(float x) {
		return x < 0.5 ? (glm::pow(2 * x, 2) * ((BACK_C2 + 1) * 2 * x - BACK_C2)) / 2
					   : (glm::pow(2 * x - 2, 2) * ((BACK_C2 + 1) * (x * 2 - 2) + BACK_C2) + 2) / 2;
	}

	// Quad
	float EaseInQuad(float x) {
		return x * x;
	}

	float EaseOutQuad(float x) {
		return 1 - (1 - x) * (1 - x);
	}

	float EaseInOutQuad(float x) {
		return x < 0.5f ? 2 * x * x : 1 - glm::pow(-2 * x + 2, 2) / 2;
	}

	// Quart
	float EaseInQuart(float x) {
		return x * x * x * x;
	}

	float EaseOutQuart(float x) {
		return 1 - glm::pow(1 - x, 4);
	}

	float EaseInOutQuart(float x) {
		return x < 0.5 ? 8 * x * x * x * x : 1 - glm::pow(-2 * x + 2, 4) / 2;
	}

	// Quint
	float EaseInQuint(float x) {
		return x * x * x * x * x;
	}

	float EaseOutQuint(float x) {
		return 1 - glm::pow(1 - x, 5);
	}

	float EaseInOutQuint(float x) {
		return x < 0.5 ? 16 * x * x * x * x * x : 1 - glm::pow(-2 * x + 2, 5) / 2;
	}

	// Bounce
	float EaseOutBounce(float x) {
		constexpr float Factor = 7.5625f;
		constexpr float Width = 2.75f;
		if (x < 1.0f / Width) return Factor * x * x;
		if (x < 2.0f / Width) {
			x -= 1.5f / Width;
			return Factor * x * x + 0.75f;
		}
		if (x < 2.5f / Width) {
			x -= 2.25f / Width;
			return Factor * x * x + 0.9375f;
		}
		x -= 2.625f / Width;
		return Factor * x * x + 0.984375f;
	}

	float EaseInBounce(float x) { return 1.0f - EaseOutBounce(1.0f - x); }

	GENERIC_IN_OUT_IMPL(Bounce);

	// Elastic
	float EaseOutElastic(float x) {
		const float Phase = (10 * x - 0.75f) * (2 * PI / 3);
		return std::exp2(-10 * x) * std::sin(Phase) + 1;
	}
	float EaseInElastic(float x) { return 1 - EaseOutElastic(1 - x); }
	GENERIC_IN_OUT_IMPL(Elastic);

	// Exponential
	float EaseInExponential(float x) { return std::exp2(10 * x - 10); }
	float EaseOutExponential(float x) { return 1 - EaseInExponential(1 - x); }
	GENERIC_IN_OUT_IMPL(Exponential);

	// Circular
	float EaseInCircular(float x) { return 1 - std::sqrt(1 - x * x); }
	float EaseOutCircular(float x) { return std::sqrt(1 - (1 - x) * (1 - x)); }
	GENERIC_IN_OUT_IMPL(Circular);

	// Cubic
	float EaseInCubic(float x) { return x * x * x; }
	float EaseOutCubic(float x) { return 1 - EaseInCubic(1 - x); }
	GENERIC_IN_OUT_IMPL(Cubic);

	const CurveMap CURVES = {
		CURVE_ENTRY_MANUAL(Linear, EaseLinear, EaseLinear, EaseLinear),
		CURVE_ENTRY(Sine),
		CURVE_ENTRY(Back),
		CURVE_ENTRY(Quad),
		CURVE_ENTRY(Quart),
		CURVE_ENTRY(Quint),
		CURVE_ENTRY(Bounce),
		CURVE_ENTRY(Elastic),
		CURVE_ENTRY(Exponential),
		CURVE_ENTRY(Circular),
		CURVE_ENTRY(Cubic),
	};

	float CalculateAlpha(float x, Enums::EasingStyle style, Enums::EasingDirection direction) {
		if (x == 0.0f || x == 1.0f) return x;
		return CURVES.at(style).at(direction)(x);
	}
}

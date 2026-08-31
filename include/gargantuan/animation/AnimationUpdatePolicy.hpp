#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <span>

#include <glm/glm.hpp>

namespace gargantuan {
	enum class AnimationUpdatePolicyClass : std::uint8_t {
		FullRate,
		ReducedRate,
		VisualFrozen,
		SemanticRequired,
	};

	enum class AnimationDistanceBand : std::uint8_t {
		Near,
		Mid,
		Far,
		VeryFar,
	};

	enum class AnimationRuntimeEnvironment : std::uint8_t {
		Conservative,
		Graphical,
		Headless,
	};

	struct AnimationUpdateContext {
		AnimationRuntimeEnvironment Environment = AnimationRuntimeEnvironment::Conservative;
		glm::vec3 ViewOrigin{0.0f};
		bool HasViewOrigin = false;
		std::uint64_t VisibilityGeneration = 0;
		std::uint64_t VisibilityPublication = 0;
		bool VisibilityComplete = false;
		std::span<const ObjectId> VisibleObjects;
		bool SemanticRequirementsComplete = true;
		std::span<const ObjectId> SemanticRequiredObjects;
	};

	struct AnimationUpdatePolicySettings {
		static constexpr float NearExitDistance = 64.0f;
		static constexpr float NearEnterDistance = 56.0f;
		static constexpr float MidExitDistance = 160.0f;
		static constexpr float MidEnterDistance = 144.0f;
		static constexpr float FarExitDistance = 320.0f;
		static constexpr float FarEnterDistance = 288.0f;
		static constexpr double RecentlyVisibleGraceSeconds = 0.25;
		static constexpr std::uint64_t MidCadenceNanoseconds = 33'333'333;
		static constexpr std::uint64_t FarCadenceNanoseconds = 66'666'667;
		static constexpr std::uint64_t VeryFarCadenceNanoseconds = 100'000'000;
	};
}

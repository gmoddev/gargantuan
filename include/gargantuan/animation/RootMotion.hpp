#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>

namespace gargantuan {
	class Character;

	struct RootMotionDelta {
		glm::vec3 Translation{0.0f};
		float YawRadians = 0.0f;
		ObjectId SourceAnimator;
		ObjectId SourceRig;
		std::uint64_t SourceTrackSequence = 0;
		std::uint64_t AnimationRevision = 0;
		double IntervalStart = 0.0;
		double IntervalEnd = 0.0;
	};

	struct CharacterRootMotionRequest {
		std::weak_ptr<Character> Target;
		ObjectId TargetCharacter;
		RootMotionDelta Delta;
	};
}

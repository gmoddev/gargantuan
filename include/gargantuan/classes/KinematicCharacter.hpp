#pragma once

#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/generated/KinematicCharacter.hpp"

namespace gargantuan {
	class KinematicCharacter : public Character {
		I_KinematicCharacter;

	  public:
		void ApplyRuntimeControllerFacts(glm::vec3 NewVelocity, glm::vec3 NewFloorNormal, bool NewGrounded);
	};
}

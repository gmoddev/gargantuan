#include "gargantuan/classes/KinematicCharacter.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	void
	KinematicCharacter::ApplyRuntimeControllerFacts(glm::vec3 NewVelocity, glm::vec3 NewFloorNormal, bool NewGrounded) {
		const auto Finite = [](const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		};
		if (GetDestroyed() || IsDestroying() || !Finite(NewVelocity) || !Finite(NewFloorNormal))
			throw std::invalid_argument("[Character:Authority] runtime controller facts are invalid");
		Velocity = NewVelocity;
		FloorNormal = NewFloorNormal;
		Grounded = NewGrounded;
	}
}

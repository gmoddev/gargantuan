#pragma once

#include "gargantuan/classes/generated/BasePart.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <glm/glm.hpp>

namespace gargantuan {
	class BasePart : public Instance {
		I_BasePart;

		glm::vec3 AccumulatedImpulse = {0.0f, 0.0f, 0.0f};
		[[nodiscard]] virtual PhysicsShapeDesc GetPhysicsShape() const = 0;

	};
} // namespace gargantuan

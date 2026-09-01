#pragma once

#include "gargantuan/classes/generated/BasePart.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <glm/glm.hpp>

namespace gargantuan {
	class Character;

	class BasePart : public Instance {
		I_BasePart;

		glm::vec3 AccumulatedImpulse = {0.0f, 0.0f, 0.0f};
		[[nodiscard]] virtual PhysicsShapeDesc GetPhysicsShape() const = 0;
		friend class Character;
		void SetCharacterSimulationCFrame(const gargantuan::CFrame &Value);

	};
} // namespace gargantuan

#pragma once

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/generated/Constraint.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <memory>
#include <tuple>

namespace gargantuan {
	class Constraint : public Instance {
		I_Constraint;

		virtual std::tuple<std::shared_ptr<BasePart>, std::shared_ptr<BasePart>> GetActiveParts() const;
		[[nodiscard]] virtual PhysicsConstraintDesc GetPhysicsConstraint(
			PhysicsBodyId BodyA,
			PhysicsBodyId BodyB
		) const = 0;
	};
}

#pragma once

#include "gargantuan/classes/generated/WeldConstraint.hpp"

namespace gargantuan {
	class WeldConstraint : public Constraint {
		I_WeldConstraint;

		virtual std::tuple<std::shared_ptr<BasePart>, std::shared_ptr<BasePart>> GetActiveParts() const override;
		[[nodiscard]] PhysicsConstraintDesc GetPhysicsConstraint(
			PhysicsBodyId BodyA,
			PhysicsBodyId BodyB
		) const override;
	};
}

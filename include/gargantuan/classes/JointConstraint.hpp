#pragma once

#include "gargantuan/classes/generated/JointConstraint.hpp"
#include <string>

namespace gargantuan {
	class JointConstraint : public Constraint {
		I_JointConstraint;

	  private:
		std::string JointType = "BallSocket";

		[[nodiscard]] PhysicsConstraintDesc GetPhysicsConstraint(
			PhysicsBodyId BodyA,
			PhysicsBodyId BodyB
		) const override;
	};
}

#pragma once

#include "gargantuan/classes/generated/DeformableBody.hpp"
#include "gargantuan/physics/SoftBodyTypes.hpp"

namespace gargantuan {
	G_ENUM(
		DeformableQuality,

		Automatic,
		Low,
		Medium,
		High
	)

	G_ENUM(
		DeformableCollisionMode,

		None,
		RigidPrimitives
	)

	class DeformableBody : public Instance {
		I_DeformableBody;

		friend class WorldRoot;
		glm::vec3 AccumulatedForce{0.0f};
		glm::vec3 AccumulatedImpulse{0.0f};

	  protected:
		[[nodiscard]] virtual SoftBodyKind GetSoftBodyKind() const = 0;
		[[nodiscard]] virtual glm::uvec3 GetSoftBodyResolution() const = 0;
	};
}

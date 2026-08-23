#pragma once

#include "gargantuan/classes/generated/MeshPart.hpp"

namespace gargantuan {
	class MeshPart : public BasePart {
		I_MeshPart;

	  private:
		std::string Mesh;
		std::string Material;

		[[nodiscard]] PhysicsShapeDesc GetPhysicsShape() const override;
	};
}

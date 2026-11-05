#pragma once

#include "gargantuan/classes/generated/MeshPart.hpp"

namespace gargantuan {
	class AnimationRuntime;
	G_ENUM(MeshCollisionMode, Box, StaticMesh)

	class MeshPart : public BasePart {
		I_MeshPart;

	  private:
		friend class AnimationRuntime;
		std::string Mesh;
		std::string Material;

		[[nodiscard]] const std::string &GetMeshReferenceRuntime() const {
			return Mesh;
		}

		[[nodiscard]] PhysicsShapeDesc GetPhysicsShape() const override;
	};
}

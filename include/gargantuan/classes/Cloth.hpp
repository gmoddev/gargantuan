#pragma once

#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/generated/Cloth.hpp"

namespace gargantuan {
	class Cloth : public DeformableBody {
		I_Cloth;

	  protected:
		[[nodiscard]] SoftBodyKind GetSoftBodyKind() const override { return SoftBodyKind::Cloth; }
		[[nodiscard]] glm::uvec3 GetSoftBodyResolution() const override {
			return {static_cast<unsigned>(GetResolutionX()), static_cast<unsigned>(GetResolutionY()), 1};
		}
	};
}

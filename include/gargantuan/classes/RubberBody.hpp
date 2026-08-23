#pragma once

#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/generated/RubberBody.hpp"

namespace gargantuan {
	class RubberBody : public DeformableBody {
		I_RubberBody;

	  protected:
		[[nodiscard]] SoftBodyKind GetSoftBodyKind() const override { return SoftBodyKind::Rubber; }
		[[nodiscard]] glm::uvec3 GetSoftBodyResolution() const override {
			return {
				static_cast<unsigned>(GetResolutionX()),
				static_cast<unsigned>(GetResolutionY()),
				static_cast<unsigned>(GetResolutionZ()),
			};
		}
	};
}

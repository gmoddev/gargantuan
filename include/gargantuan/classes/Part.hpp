#pragma once

#include "gargantuan/classes/generated/Part.hpp"

namespace gargantuan {
	G_ENUM(
		PartType,

		Ball,
		Block,
		Cylinder,
		Wedge,
		CornerWedge
	)

	class Part : public BasePart {
		I_Part;

		void CreateBodyShape(b3BodyId bodyId, b3ShapeDef &bodyShape) override;
	};
} // namespace gargantuan

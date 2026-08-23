#pragma once

#include "gargantuan/classes/GuiInputEvent.hpp"
#include "gargantuan/classes/generated/GuiBase2d.hpp"

namespace gargantuan {
	G_ENUM(AccessibilityRole, Automatic, None, Group, Text, Image, Button);

	class GuiRuntime;
	class GuiBase2d : public GuiBase {
		I_GuiBase2d;

		friend class GuiRuntime;

	  public:
		void CommitRuntimeGeometry(Vector2 PositionValue, Vector2 SizeValue, float RotationValue) {
			AbsolutePosition = PositionValue;
			AbsoluteSize = SizeValue;
			AbsoluteRotation = RotationValue;
		}
	};
}

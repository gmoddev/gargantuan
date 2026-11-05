#pragma once

#include "gargantuan/classes/InputObject.hpp"

namespace gargantuan {
	G_ENUM(InputActionType, Bool, Scalar, Vector2);
	G_ENUM(InputBindingType, Key, MouseButton, PointerDelta);

	[[nodiscard]] inline bool IsInputModifier(Enums::KeyCode Value) {
		switch (Value) {
		case Enums::KeyCode::LeftControl:
		case Enums::KeyCode::RightControl:
		case Enums::KeyCode::LeftShift:
		case Enums::KeyCode::RightShift:
		case Enums::KeyCode::LeftAlt:
		case Enums::KeyCode::RightAlt:
		case Enums::KeyCode::LeftMeta:
		case Enums::KeyCode::RightMeta:
			return true;
		default:
			return false;
		}
	}
}

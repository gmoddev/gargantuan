#pragma once

#include "gargantuan/classes/generated/InputAction.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/input/InputDefinitions.hpp"

namespace gargantuan {
	class InputService;
	class InputAction : public Instance {
		I_InputAction;

	  private:
		friend class InputService;
		Vector2 RuntimeValue;
		float RuntimeScalar = 0.0f;
		bool RuntimeDown = false;
		Enums::InputActionType ActionType = Enums::InputActionType::Bool;
		void CommitState(Vector2 Value, bool Down, float Scalar);
	};
}

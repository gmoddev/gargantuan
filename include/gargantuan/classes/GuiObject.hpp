#pragma once

#include "gargantuan/classes/generated/GuiObject.hpp"
#include "gargantuan/datatypes/Rect.hpp"
#include "gargantuan/reflection/Enums.hpp"

namespace gargantuan {
	G_ENUM(GuiState, Idle, Hover, Press, NonInteractable);
	G_ENUM(InputSink, None, Activate, All = 100);
	G_ENUM(AutomaticSize, None, X, Y, XY);

	class GuiRuntime;
	class GuiObject : public GuiBase2d {
		I_GuiObject;

		friend class GuiRuntime;

	  public:
		void CommitRuntimeInteraction(bool ActiveValue, Enums::GuiState StateValue) {
			Active = ActiveValue;
			GuiState = StateValue;
		}
	};
}

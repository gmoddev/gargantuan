#pragma once

#include "gargantuan/classes/generated/UIListLayout.hpp"

namespace gargantuan {
	G_ENUM(GuiFillDirection, Horizontal, Vertical);

	class UIListLayout : public GuiBase {
		I_UIListLayout;
	};
}

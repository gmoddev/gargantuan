#pragma once

#include "gargantuan/classes/generated/TextLabel.hpp"

namespace gargantuan {
	G_ENUM(TextXAlignment, Left, Center, Right);
	G_ENUM(TextYAlignment, Top, Center, Bottom);

	class TextLabel : public GuiObject {
		I_TextLabel;

	  public:
		TextLabel();
	};
}

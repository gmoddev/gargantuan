#pragma once

#include "gargantuan/classes/generated/ImageLabel.hpp"

namespace gargantuan {
	G_ENUM(GuiImageScaleType, Stretch, Fit);

	class ImageLabel : public GuiObject {
		I_ImageLabel;

	  private:
		std::string Image;

	  public:
		ImageLabel();
	};
}

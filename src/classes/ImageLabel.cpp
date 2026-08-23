#include "gargantuan/classes/ImageLabel.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	ImageLabel::ImageLabel() { BackgroundTransparency = 1.0f; }

	std::string ImageLabel::GetImage() const { return Image; }

	void ImageLabel::SetImage(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Asset:Reference] Image requires a strict asset:// or builtin:// reference");
		ValidatePropertyMutation("Image", Value);
		if (Image == Value) return;
		Image = std::move(Value);
		NotifyPropertyCommitted("Image");
	}
}

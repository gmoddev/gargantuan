#include "gargantuan/classes/TextLabel.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	TextLabel::TextLabel() { BackgroundTransparency = 1.0f; }

	std::string TextLabel::GetFontFace() const { return FontFace; }

	void TextLabel::SetFontFace(std::string Value) {
		AssertCanMutate();
		if (!AssetReference::Parse(Value))
			throw std::invalid_argument("[Asset:Reference] FontFace requires a strict asset:// or builtin:// reference");
		ValidatePropertyMutation("FontFace", Value);
		if (FontFace == Value) return;
		FontFace = std::move(Value);
		NotifyPropertyCommitted("FontFace");
	}
}

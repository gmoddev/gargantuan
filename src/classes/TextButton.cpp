#include "gargantuan/classes/TextButton.hpp"

namespace gargantuan {
	TextButton::TextButton() {
		BackgroundTransparency = 0.0f;
		BackgroundColor3 = Color3(0.18f, 0.22f, 0.30f);
		Interactable = true;
		Selectable = true;
		InputSink = Enums::InputSink::Activate;
		GuiState = Enums::GuiState::Idle;
	}
}

#include "gargantuan/classes/TextBox.hpp"

namespace gargantuan {
	TextBox::TextBox() {
		BackgroundTransparency = 0.0f;
		BackgroundColor3 = Color3(0.08f, 0.10f, 0.14f);
		TextXAlignment = Enums::TextXAlignment::Left;
		Interactable = true;
		Selectable = true;
		InputSink = Enums::InputSink::All;
		GuiState = Enums::GuiState::Idle;
	}
}

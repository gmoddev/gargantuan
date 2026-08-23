#include "gargantuan/classes/ScrollingFrame.hpp"

namespace gargantuan {
	ScrollingFrame::ScrollingFrame() {
		ClipsDescendants = true;
		Interactable = true;
		Selectable = true;
		InputSink = Enums::InputSink::All;
		GuiState = Enums::GuiState::Idle;
	}
}

#include "gargantuan/classes/ScrollingFrame.hpp"

namespace gargantuan {
	ScrollingFrame::ScrollingFrame() {
		ClipsDescendants = true;
		GuiState = Enums::GuiState::Idle;
	}
}

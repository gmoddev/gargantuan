#pragma once

#include "gargantuan/classes/generated/TextBox.hpp"

#include <string>
#include <utility>

namespace gargantuan {
	class GuiRuntime;
	class TextBox : public TextLabel {
		I_TextBox;

		friend class GuiRuntime;

	  public:
		TextBox();

		void CommitRuntimeEditing(int Caret, int Selection, int Length, std::string Composition) {
			CaretPosition = Caret;
			SelectionStart = Selection;
			SelectionLength = Length;
			CompositionText = std::move(Composition);
		}
	};
}

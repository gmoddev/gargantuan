#pragma once

#include "gargantuan/classes/generated/ScrollingFrame.hpp"

namespace gargantuan {
	G_ENUM(ScrollingDirection, X, Y, XY);

	class GuiRuntime;
	class ScrollingFrame : public Frame {
		I_ScrollingFrame;

		friend class GuiRuntime;

	  public:
		ScrollingFrame();

		void CommitRuntimeContentExtent(Vector2 Value) { ContentExtent = Value; }
	};
}

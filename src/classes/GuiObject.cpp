#include "gargantuan/classes/GuiObject.hpp"
#include "gargantuan/gui/GuiRuntime.hpp"

namespace gargantuan {
	void GuiObject::CaptureFocus() {
		if (auto *Runtime = GuiRuntime::Find(*this)) Runtime->RequestFocus(GetObjectId());
	}

	void GuiObject::ReleaseFocus() {
		if (auto *Runtime = GuiRuntime::Find(*this)) Runtime->ReleaseFocus(GetObjectId());
	}

	void GuiObject::CapturePointer(int PointerId) {
		if (auto *Runtime = GuiRuntime::Find(*this)) Runtime->CapturePointer(PointerId, GetObjectId());
	}

	void GuiObject::ReleasePointer(int PointerId) {
		if (auto *Runtime = GuiRuntime::Find(*this)) Runtime->ReleasePointer(PointerId, GetObjectId());
	}
}

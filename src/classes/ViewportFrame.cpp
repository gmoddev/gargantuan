#include "gargantuan/classes/ViewportFrame.hpp"

#include "gargantuan/classes/Camera.hpp"

#include <stdexcept>

namespace gargantuan {
	ViewportFrame::~ViewportFrame() {
		if (CameraAncestry) CameraAncestry->Disconnect();
	}

	std::shared_ptr<ViewportFrame> ViewportFrame::FindContaining(const Instance &Object) {
		for (auto Parent = Object.ParentReference.lock(); Parent; Parent = Parent->ParentReference.lock())
			if (auto Viewport = std::dynamic_pointer_cast<ViewportFrame>(Parent)) return Viewport;
		return nullptr;
	}

	std::optional<std::shared_ptr<Camera>> ViewportFrame::GetCurrentCamera() const {
		auto Camera = CameraValue.lock();
		if (!Camera || Camera->GetDestroyed() || Camera->IsDestroying() ||
			FindContaining(*Camera).get() != this)
			return std::nullopt;
		return Camera;
	}

	void ViewportFrame::SetCurrentCamera(std::optional<std::shared_ptr<Camera>> Value) {
		AssertCanMutate();
		ValidatePropertyMutation("CurrentCamera", Value);
		auto Camera = Value.value_or(nullptr);
		if (Camera && (Camera->GetDestroyed() || Camera->IsDestroying() || FindContaining(*Camera).get() != this))
			throw std::invalid_argument("[Gui:ViewportFrame] CurrentCamera must be a live Camera in this viewport subtree");
		if (CameraValue.lock() == Camera) return;
		SignalConnection::Pointer Connection;
		if (Camera) {
			Connection = Camera->AncestryChanged->Connect([Owner = weak_from_this()](auto) {
				auto Frame = std::static_pointer_cast<ViewportFrame>(Owner.lock());
				if (!Frame || Frame->GetDestroyed() || Frame->IsDestroying() || Frame->GetCurrentCamera()) return;
				Frame->CameraValue.reset();
				if (auto Subscription = std::move(Frame->CameraAncestry)) Subscription->Disconnect();
				Frame->NotifyPropertyCommitted("CurrentCamera");
			});
		}
		if (CameraAncestry) CameraAncestry->Disconnect();
		CameraAncestry = std::move(Connection);
		CameraValue = std::move(Camera);
		NotifyPropertyCommitted("CurrentCamera");
	}
}

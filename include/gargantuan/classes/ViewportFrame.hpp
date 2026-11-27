#pragma once

#include "gargantuan/classes/GuiObject.hpp"
#include "gargantuan/classes/generated/ViewportFrame.hpp"

namespace gargantuan {
	class ViewportFrame : public GuiObject {
		I_ViewportFrame;

	  public:
		~ViewportFrame() override;
		[[nodiscard]] static std::shared_ptr<ViewportFrame> FindContaining(const Instance &Object);

	  private:
		std::weak_ptr<SpatialRegion> ViewRegion;
		SignalConnection::Pointer RegionAncestry;
		std::weak_ptr<Camera> CameraValue;
		SignalConnection::Pointer CameraAncestry;
	};
}

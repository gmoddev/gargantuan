#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <memory>
#include <optional>

namespace gargantuan {
	class AnimationRuntime;
	class SpatialRegion;
	class ViewportFrame;

	// Main-owned, retained while at least one GUI camera samples this world.
	class SpatialViewportCapture final {
	  public:
		SpatialViewportCapture();
		~SpatialViewportCapture();
		[[nodiscard]] std::optional<RenderViewportScene> Capture(const ViewportFrame &Viewport, SpatialRegion &World,
			const AnimationRuntime *Animation, std::uint32_t Width, std::uint32_t Height);

	  private:
		struct State;
		std::unique_ptr<State> Current;
	};
}

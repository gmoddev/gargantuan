#include "gargantuan/editor/EditorViewport.hpp"

#include <cassert>
#include <cstddef>
#include <memory>

using namespace gargantuan;

namespace {
	RenderPublicationPtr MakePublication(std::uint32_t Width, std::uint32_t Height, RenderPublicationId Id) {
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Id;
		Publication->FullResync = true;
		Publication->Frame.ViewportWidth = Width;
		Publication->Frame.ViewportHeight = Height;
		return Publication;
	}
}

int main() {
	{
		EditorViewportRenderer Renderer(64, 64);
		auto Frame = Renderer.Capture(MakePublication(64, 64, 1));
		assert(Frame.Width == 64 && Frame.Height == 64);
		assert(Frame.RgbPixels.size() == static_cast<std::size_t>(64 * 64 * 3));

		Renderer.Resize(32, 48);
		Frame = Renderer.Capture(MakePublication(32, 48, 2));
		assert(Frame.Width == 32 && Frame.Height == 48);
		assert(Frame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	}

	EditorViewportRenderer Restarted(32, 48);
	auto RestartedFrame = Restarted.Capture(MakePublication(32, 48, 50));
	assert(RestartedFrame.Width == 32 && RestartedFrame.Height == 48);
	assert(RestartedFrame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	return 0;
}

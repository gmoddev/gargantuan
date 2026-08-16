#include "gargantuan/editor/EditorViewport.hpp"

#include <cassert>
#include <cstddef>
#include <memory>

using namespace gargantuan;

namespace {
	RenderSnapshotPtr MakeSnapshot(std::uint32_t Width, std::uint32_t Height, RenderSnapshotId Id) {
		auto Snapshot = std::make_shared<RenderSnapshot>();
		Snapshot->Id = Id;
		Snapshot->ViewportWidth = Width;
		Snapshot->ViewportHeight = Height;
		return Snapshot;
	}
}

int main() {
	EditorViewportRenderer Renderer(64, 64);
	auto Frame = Renderer.Capture(MakeSnapshot(64, 64, 1));
	assert(Frame.Width == 64 && Frame.Height == 64);
	assert(Frame.RgbPixels.size() == static_cast<std::size_t>(64 * 64 * 3));

	Renderer.Resize(32, 48);
	Frame = Renderer.Capture(MakeSnapshot(32, 48, 2));
	assert(Frame.Width == 32 && Frame.Height == 48);
	assert(Frame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	return 0;
}

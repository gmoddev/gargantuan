#include "gargantuan/editor/EditorViewport.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

using namespace gargantuan;

namespace {
	std::shared_ptr<RenderPublication>
	MakePublication(std::uint32_t Width, std::uint32_t Height, RenderPublicationId Id) {
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Id;
		Publication->FullResync = true;
		Publication->Frame.ViewportWidth = Width;
		Publication->Frame.ViewportHeight = Height;
		return Publication;
	}

	std::shared_ptr<const std::vector<std::uint8_t>>
	SolidPixels(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) {
		return std::make_shared<const std::vector<std::uint8_t>>(
			std::initializer_list<std::uint8_t>{Red, Green, Blue, 255}
		);
	}

	RenderUiFrame FullscreenUi(std::uint32_t Width, std::uint32_t Height) {
		RenderUiFrame Ui{Width, Height, 1.0f, {}};
		RenderUiBatch Batch;
		Batch.Vertices = {
			{{0.0f, 0.0f}, {0.0f, 0.0f}, {0.2f, 0.8f, 0.3f, 1.0f}},
			{{static_cast<float>(Width), 0.0f}, {1.0f, 0.0f}, {0.2f, 0.8f, 0.3f, 1.0f}},
			{{static_cast<float>(Width), static_cast<float>(Height)}, {1.0f, 1.0f}, {0.2f, 0.8f, 0.3f, 1.0f}},
			{{0.0f, static_cast<float>(Height)}, {0.0f, 1.0f}, {0.2f, 0.8f, 0.3f, 1.0f}},
		};
		Batch.Indices = {0, 1, 2, 2, 3, 0};
		Ui.Batches.push_back(std::move(Batch));
		return Ui;
	}

	std::size_t CenterPixel(std::uint32_t Width, std::uint32_t Height) {
		return (static_cast<std::size_t>(Height / 2) * Width + Width / 2) * 3;
	}
}

int main() {
	{
		EditorViewportRenderer Renderer(64, 64);
		auto RedFallback = MakePublication(64, 64, 1);
		RedFallback->Frame.Environment.EnvironmentColor = {1.0f, 0.0f, 0.0f};
		auto Frame = Renderer.Capture(RedFallback);
		assert(Frame.Width == 64 && Frame.Height == 64);
		assert(Frame.RgbPixels.size() == static_cast<std::size_t>(64 * 64 * 3));
		const auto Center = CenterPixel(64, 64);
		assert(Frame.RgbPixels[Center] > Frame.RgbPixels[Center + 1] + 100);

		auto GreenFallback = MakePublication(64, 64, 2);
		GreenFallback->Frame.Environment.EnvironmentColor = {0.0f, 1.0f, 0.0f};
		Frame = Renderer.Capture(GreenFallback);
		assert(Frame.RgbPixels[Center + 1] > Frame.RgbPixels[Center] + 100);

		auto SixFaceSky = MakePublication(64, 64, 3);
		SixFaceSky->EnvironmentChanged = true;
		RenderSkyState Sky{.FaceDimension = 1};
		for (std::size_t Index = 0; Index < Sky.Faces.size(); ++Index) {
			const RenderTextureIdentity Texture{100 + Index, 1};
			Sky.Faces[Index] = {Texture, 1};
			const bool NegativeZ = Index == static_cast<std::size_t>(RenderSkyFace::NegativeZ);
			SixFaceSky->TextureCreates.push_back(
				{Texture,
				 1,
				 1,
				 1,
				 RenderTextureFormat::Rgba8Unorm,
				 NegativeZ ? SolidPixels(0, 0, 255) : SolidPixels(255, 0, 0)}
			);
		}
		SixFaceSky->Frame.Environment.Sky = Sky;
		Frame = Renderer.Capture(SixFaceSky);
		assert(Frame.RgbPixels[Center + 2] > Frame.RgbPixels[Center] + 100);

		auto LowExposureUi = MakePublication(64, 64, 4);
		LowExposureUi->Frame.Environment.ExposureMultiplier = 1.0f / 256.0f;
		LowExposureUi->UiChanged = true;
		LowExposureUi->Ui = FullscreenUi(64, 64);
		const auto LowExposureFrame = Renderer.Capture(LowExposureUi);
		auto HighExposureUi = MakePublication(64, 64, 5);
		HighExposureUi->Frame.Environment.ExposureMultiplier = 256.0f;
		HighExposureUi->UiChanged = true;
		HighExposureUi->Ui = FullscreenUi(64, 64);
		const auto HighExposureFrame = Renderer.Capture(HighExposureUi);
		for (std::size_t Channel = 0; Channel < 3; ++Channel)
			assert(LowExposureFrame.RgbPixels[Center + Channel] == HighExposureFrame.RgbPixels[Center + Channel]);

		Renderer.Resize(32, 48);
		Frame = Renderer.Capture(MakePublication(32, 48, 6));
		assert(Frame.Width == 32 && Frame.Height == 48);
		assert(Frame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	}

	EditorViewportRenderer Restarted(32, 48);
	auto RestartedFrame = Restarted.Capture(MakePublication(32, 48, 50));
	assert(RestartedFrame.Width == 32 && RestartedFrame.Height == 48);
	assert(RestartedFrame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	return 0;
}

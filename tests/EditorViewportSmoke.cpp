#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <format>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <memory>
#include <stdexcept>
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

	std::shared_ptr<RenderPublication> MaterialScene(RenderPublicationId Id, RenderMaterialState Material) {
		auto Publication = MakePublication(64, 64, Id);
		auto &Camera = Publication->Frame.Camera;
		Camera.Position = {0.0f, 0.0f, 3.0f};
		Camera.ViewMatrix = glm::lookAtRH(Camera.Position, glm::vec3(0.0f), glm::vec3(0, 1, 0));
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 10.0f);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
		Publication->Frame.Environment.EnvironmentColor = glm::vec3(0.0f);
		Publication->Frame.Environment.SunDirection = {0.0f, 0.0f, 1.0f};
		Publication->Frame.Environment.SunIntensity = 0.0f;
		Publication->Frame.Environment.AmbientColor = glm::vec3(1.0f);
		Publication->EnvironmentChanged = true;
		const RenderMeshIdentity Mesh{100, 1};
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::initializer_list<RenderVertex>{
			{{-1, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0.5f, 0.5f}},
			{{1, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0.5f, 0.5f}},
			{{1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0.5f, 0.5f}},
			{{-1, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0.5f, 0.5f}},
		});
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3});
		Publication->MeshCreates.push_back({
			.Mesh = Mesh, .TopologyRevision = 1, .VertexRevision = 1,
			.Vertices = Vertices, .Indices = Indices, .Bounds = {{-1, -1, 0}, {1, 1, 0}},
		});
		RenderItem Item;
		Item.Object = {90, 1};
		Item.CastShadow = false;
		Publication->Creates.push_back({.Item = Item, .Mesh = Mesh, .Material = Material});
		return Publication;
	}

	void CheckMaterials(EditorViewportRenderer &Renderer) {
		const RenderTextureIdentity Texture{200, 1};
		RenderMaterialState Material;
		Material.ShadingModel = RenderShadingModel::MetallicRoughness;
		Material.DoubleSided = true;
		Material.BaseColorTexture = Texture;
		auto Filter = MaterialScene(10, Material);
		Filter->TextureCreates.push_back({
			Texture, 1, 2, 1, RenderTextureFormat::Rgba8Unorm,
			std::make_shared<const std::vector<std::uint8_t>>(
				std::initializer_list<std::uint8_t>{0, 0, 0, 255, 255, 255, 255, 255}),
		});
		const auto Filtered = Renderer.Capture(Filter);
		const auto Center = CenterPixel(64, 64);
		const auto Channel = Filtered.RgbPixels[Center];
		// The midpoint of black and white is 0.5 linear, approximately 206
		// after the world tone curve and sRGB transfer.
		if (Channel < 203 || Channel > 209)
			throw std::runtime_error("[Renderer:Material] Base color filtered outside the linear-light pixel bound");

		Material.BaseColorTexture.reset();
		Material.NormalTexture = Texture;
		const auto LitNormal = [&](RenderPublicationId Id, std::uint8_t X, std::uint8_t Z) {
			auto Scene = MaterialScene(Id, Material);
			Scene->Frame.Environment.AmbientColor = glm::vec3(0.0f);
			Scene->Frame.Environment.SunIntensity = 1.0f;
			Scene->TextureCreates.push_back({
				Texture, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, SolidPixels(X, 128, Z),
			});
			return Renderer.Capture(Scene).RgbPixels[Center];
		};
		const auto Flat = LitNormal(11, 128, 255);
		const auto Tilted = LitNormal(12, 255, 128);
		if (int(Flat) < int(Tilted) + 32)
			throw std::runtime_error("[Renderer:Material] Tangent-space normal did not redirect the direct light");

		Material.NormalTexture.reset();
		Material.Metallic = 1.0f;
		Material.MetallicRoughnessTexture = Texture;
		const auto PackedMaterial = [&](RenderPublicationId Id, std::uint8_t Blue) {
			auto Scene = MaterialScene(Id, Material);
			Scene->TextureCreates.push_back({
				Texture, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, SolidPixels(255, 180, Blue),
			});
			return Renderer.Capture(Scene).RgbPixels[Center];
		};
		const auto Dielectric = PackedMaterial(13, 0);
		const auto Metal = PackedMaterial(14, 255);
		if (Dielectric < 200 || Metal > 3)
			throw std::runtime_error("[Renderer:Material] Packed metallic blue channel did not gate diffuse ambient");

		Material = {};
		Material.DoubleSided = true;
		Material.OpacityMode = RenderOpacityMode::Transparent;
		Material.BaseColorFactor = {1.0f, 0.0f, 0.0f, 0.5f};
		auto Blended = MaterialScene(15, Material);
		auto Far = Blended->Creates.front();
		Far.Item.Object = {91, 1};
		Far.Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0, -1));
		Far.Item.InverseModelMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0, 1));
		Far.Material.BaseColorFactor = {0.0f, 0.0f, 1.0f, 0.5f};
		Blended->Creates.push_back(Far);
		const auto Composition = Renderer.Capture(Blended);
		const auto Red = Composition.RgbPixels[Center];
		const auto Blue = Composition.RgbPixels[Center + 2];
		if (Red < 95 || Red > 110 || Blue < 45 || Blue > 58)
			throw std::runtime_error("[Renderer:Material] Blended surfaces did not composite back to front");

		const auto ShadowProbe = [&](RenderPublicationId Id, bool AddCaster, std::uint8_t Alpha) {
			RenderMaterialState Receiver;
			Receiver.DoubleSided = true;
			auto Scene = MaterialScene(Id, Receiver);
			auto &Light = Scene->Frame.Environment;
			Light.SunDirection = glm::normalize(glm::vec3(0.5f, 0.0f, 1.0f));
			Light.SunIntensity = 1.0f;
			Light.AmbientColor = glm::vec3(0.0f);
			if (AddCaster) {
				auto Caster = Scene->Creates.front();
				Caster.Item.Object = {91, 1};
				Caster.Item.CastShadow = true;
				Caster.Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 1)) *
					glm::scale(glm::mat4(1), glm::vec3(0.18f));
				Caster.Item.InverseModelMatrix = glm::inverse(Caster.Item.ModelMatrix);
				Caster.Material.BaseColorTexture = Texture;
				Caster.Material.OpacityMode = RenderOpacityMode::Masked;
				Scene->Creates.push_back(Caster);
				Scene->TextureCreates.push_back({
					Texture, 1, 1, 1, RenderTextureFormat::Rgba8Unorm,
					std::make_shared<const std::vector<std::uint8_t>>(
						std::initializer_list<std::uint8_t>{255, 255, 255, Alpha}),
				});
			}
			return Renderer.Capture(Scene).RgbPixels[Center];
		};
		const auto Unshadowed = ShadowProbe(16, false, 0);
		const auto SolidShadow = ShadowProbe(17, true, 255);
		const auto CutoutShadow = ShadowProbe(18, true, 0);
		if (Unshadowed < 150 || int(Unshadowed) - int(SolidShadow) < 80 ||
			std::abs(int(Unshadowed) - int(CutoutShadow)) > 2)
			throw std::runtime_error(std::format(
				"[Renderer:Shadow] A masked shadow did not follow the sampled alpha (lit {}, solid {}, cutout {})",
				Unshadowed, SolidShadow, CutoutShadow));
	}

	void CheckWorldSubmission() {
		SDLRenderer Renderer(Vector2(64, 64), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Scene = MaterialScene(1, {});
		auto Outside = Scene->Creates.front();
		Outside.Item.Object = {91, 1};
		Outside.Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(32, 0, 0));
		Outside.Item.InverseModelMatrix = glm::translate(glm::mat4(1), glm::vec3(-32, 0, 0));
		Scene->Creates.push_back(Outside);
		auto Inside = Scene->Creates.front();
		Inside.Item.Object = {92, 1};
		Scene->Creates.push_back(Inside);
		Renderer.Draw(Scene);
		const auto Metrics = Renderer.GetMetrics();
		if (Metrics.CameraCulledObjects != 1 || Metrics.WorldGeometryBindings != 1 || Metrics.WorldMaterialBindings != 1)
			throw std::runtime_error("[Renderer:Material] Camera culling or shared submission bindings differ from the fixture");
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

		CheckMaterials(Renderer);
		Renderer.Resize(32, 48);
		Frame = Renderer.Capture(MakePublication(32, 48, 19));
		assert(Frame.Width == 32 && Frame.Height == 48);
		assert(Frame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	}

	EditorViewportRenderer Restarted(32, 48);
	auto RestartedFrame = Restarted.Capture(MakePublication(32, 48, 50));
	assert(RestartedFrame.Width == 32 && RestartedFrame.Height == 48);
	assert(RestartedFrame.RgbPixels.size() == static_cast<std::size_t>(32 * 48 * 3));
	CheckWorldSubmission();
	return 0;
}

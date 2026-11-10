#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace gargantuan;

namespace gargantuan {
	struct EditorViewportRendererTestAccess {
		static void InjectPaletteUploadFailure(EditorViewportRenderer &Renderer) {
			Renderer.InjectPaletteUploadFailureForTesting();
		}
	};
}

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

	int DisplayByte(double Linear, double Exposure, bool Reinhard = false) {
		const auto Value = Linear * Exposure;
		const auto Mapped = Reinhard ? Value / (Value + 1.0) : std::clamp((Value * (2.51 * Value + 0.03)) /
			(Value * (2.43 * Value + 0.59) + 0.14), 0.0, 1.0);
		const auto Encoded = Mapped <= 0.0031308 ? Mapped * 12.92 : 1.055 * std::pow(Mapped, 1.0 / 2.4) - 0.055;
		return static_cast<int>(std::lround(Encoded * 255.0));
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
		const bool CompositionValid = Renderer.UsesHdrScene()
			? std::abs(int(Red) - DisplayByte(0.5, 1.0)) <= 2 &&
				std::abs(int(Blue) - DisplayByte(0.25, 1.0)) <= 2
			: Red >= 95 && Red <= 110 && Blue >= 45 && Blue <= 58;
		if (!CompositionValid)
			throw std::runtime_error("[Renderer:Material] Blended surfaces did not composite back to front");

		const auto ShadowScene = [&](RenderPublicationId Id, bool AddCaster) {
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
					SolidPixels(255, 255, 255),
				});
			}
			return Scene;
		};
		const auto Unshadowed = Renderer.Capture(ShadowScene(16, false)).RgbPixels[Center];
		const auto SolidScene = ShadowScene(17, true);
		const auto SolidImage = Renderer.Capture(SolidScene).RgbPixels;
		const auto SolidShadow = SolidImage[Center];
		const auto ShadowDelta = [&](RenderPublicationId Id, RenderPublicationId Base) {
			auto Delta = std::make_shared<RenderPublication>();
			Delta->Id = Id;
			Delta->BaseId = Base;
			Delta->Frame = SolidScene->Frame;
			return Delta;
		};
		if (Renderer.Capture(ShadowDelta(18, 17)).RgbPixels != SolidImage)
			throw std::runtime_error("[Renderer:Shadow] A cached static map changed exact viewport pixels");
		auto Cutout = ShadowDelta(19, 18);
		Cutout->TextureUpdates.push_back({Texture, 2, 0, 0, 1, 1,
			std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255, 255, 255, 0})});
		const auto CutoutShadow = Renderer.Capture(Cutout).RgbPixels[Center];
		if (Unshadowed < 150 || int(Unshadowed) - int(SolidShadow) < 80 ||
			std::abs(int(Unshadowed) - int(CutoutShadow)) > 2)
			throw std::runtime_error(std::format(
				"[Renderer:Shadow] A masked shadow did not follow the sampled alpha (lit {}, solid {}, cutout {})",
				Unshadowed, SolidShadow, CutoutShadow));
		auto Restore = ShadowDelta(20, 19);
		Restore->TextureUpdates.push_back({Texture, 3, 0, 0, 1, 1, SolidPixels(255, 255, 255)});
		if (Renderer.Capture(Restore).RgbPixels != SolidImage)
			throw std::runtime_error("[Renderer:Shadow] Restoring only mask pixels did not restore the exact image");

		auto CameraMove = ShadowDelta(21, 20);
		auto &MovedCamera = CameraMove->Frame.Camera;
		MovedCamera.Position.x += 0.06f;
		MovedCamera.ViewMatrix = glm::lookAtRH(MovedCamera.Position,
			MovedCamera.Position + MovedCamera.LookDirection, MovedCamera.UpDirection);
		MovedCamera.ViewProjectionMatrix = MovedCamera.ProjectionMatrix * MovedCamera.ViewMatrix;
		const auto CachedCameraImage = Renderer.Capture(CameraMove).RgbPixels;
		auto Reconstruct = ShadowScene(22, true);
		Reconstruct->Frame = CameraMove->Frame;
		if (Renderer.Capture(Reconstruct).RgbPixels != CachedCameraImage)
			throw std::runtime_error("[Renderer:Shadow] Camera motion differs between cached and reconstructed depth");
		bool Rejected = false;
		try { (void)Renderer.Capture(ShadowDelta(23, 21)); }
		catch (const std::invalid_argument &) { Rejected = true; }
		auto Retry = ShadowDelta(23, 22);
		Retry->Frame = CameraMove->Frame;
		if (!Rejected || Renderer.Capture(Retry).RgbPixels != CachedCameraImage)
			throw std::runtime_error("[Renderer:Shadow] Stale publication or retry changed the committed shadow image");

		auto MoveCaster = ShadowDelta(24, 23);
		MoveCaster->Frame = CameraMove->Frame;
		auto Caster = Reconstruct->Creates.back();
		Caster.Item.ModelMatrix[3].x += 6.0f;
		Caster.Item.InverseModelMatrix = glm::inverse(Caster.Item.ModelMatrix);
		MoveCaster->Updates.push_back({Caster.Item.Object, RenderUpdateDomain::Transform, Caster.Item,
			Caster.Mesh, Caster.Material, Caster.Visible, Caster.Primitives});
		if (Renderer.Capture(MoveCaster).RgbPixels[Center] < 150)
			throw std::runtime_error("[Renderer:Shadow] A moved caster left stale depth on the receiver");
	}

	void CheckWorldSubmission() {
		SDLRenderer Renderer(Vector2(64, 64), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Scene = MaterialScene(1, {});
		Scene->Frame.Environment.SunIntensity = 1.0f;
		Scene->Creates.front().Item.CastShadow = true;
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
		if ((Metrics.HdrSceneBytes != 0) != (Metrics.SceneResolveDraws == 1) ||
			(Metrics.HdrSceneBytes && Metrics.HdrSceneBytes != 64 * 64 * 8))
			throw std::runtime_error("[Renderer:HDR] Resolve and target memory metrics do not agree");
		auto Static = std::make_shared<RenderPublication>();
		Static->Id = 2;
		Static->BaseId = 1;
		Static->Frame = Scene->Frame;
		Renderer.Draw(Static);
		const auto Warm = Renderer.GetMetrics();
		if (Metrics.ShadowMapBuilds != 1 || Metrics.ShadowMapPasses != 1 || Metrics.ShadowCasterDraws == 0 ||
			Warm.ShadowCacheHits != 1 || Warm.ShadowMapBuilds != Metrics.ShadowMapBuilds ||
			Warm.ShadowMapPasses != Metrics.ShadowMapPasses || Warm.ShadowCasterDraws != Metrics.ShadowCasterDraws)
			throw std::runtime_error("[Renderer:Shadow] Static runtime frame did not skip the entire shadow pass");
	}

	void CheckPublicationRecovery() {
		EditorViewportRenderer Renderer(64, 64);
		RenderMaterialState Material;
		Material.DoubleSided = true;
		Material.BaseColorFactor = {1.0f, 0.0f, 0.0f, 1.0f};
		const auto Initial = MaterialScene(40, Material);
		auto &Mesh = Initial->MeshCreates.front();
		Mesh.Skeleton.Bytes[0] = 1;
		Mesh.SkeletonJointCount = 1;
		Mesh.SkinInfluences = std::make_shared<const std::vector<RenderSkinInfluence>>(
			Mesh.Vertices->size(), RenderSkinInfluence{{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
		const auto InitialPixels = Renderer.Capture(Initial).RgbPixels;
		if (!Renderer.Pick(32, 32) || InitialPixels[CenterPixel(64, 64)] < 150)
			throw std::runtime_error("[Renderer:Viewport] Recovery fixture did not begin with visible geometry");

		auto Stale = std::make_shared<RenderPublication>();
		Stale->Id = 41;
		Stale->BaseId = 39;
		Stale->Frame = Initial->Frame;
		bool RejectedStale = false;
		try { Renderer.ApplyPublication(Stale); }
		catch (const std::invalid_argument &) { RejectedStale = true; }
		if (!RejectedStale || Renderer.Capture(Initial).RgbPixels != InitialPixels)
			throw std::runtime_error("[Renderer:Viewport] Malformed delta disturbed the last valid frame");

		auto Delta = std::make_shared<RenderPublication>(*Stale);
		Delta->BaseId = Initial->Id;
		Delta->AnimationPoseUpdates.push_back({
			.Object = Initial->Creates.front().Item.Object, .SourceMesh = Mesh.Mesh, .PoseRevision = 1,
			.Palette = {Mesh.Skeleton, std::make_shared<const std::vector<RenderSkinPaletteEntry>>(1)},
		});
		EditorViewportRendererTestAccess::InjectPaletteUploadFailure(Renderer);
		bool ObservedFailure = false;
		try { Renderer.ApplyPublication(Delta); }
		catch (const std::runtime_error &Error) {
			ObservedFailure = std::string_view(Error.what()).find("injected palette upload failure") != std::string_view::npos;
		}
		if (!ObservedFailure || Renderer.Pick(32, 32))
			throw std::runtime_error("[Renderer:Viewport] Failed palette publication left live projection state");

		auto Next = std::make_shared<RenderPublication>();
		Next->Id = 42;
		Next->BaseId = Delta->Id;
		Next->Frame = Initial->Frame;
		for (const auto &Publication : {Delta, Next}) {
			bool RequestedResync = false;
			try { Renderer.ApplyPublication(Publication); }
			catch (const std::logic_error &Error) {
				RequestedResync = std::string_view(Error.what()).find("requires a full RenderPublication") != std::string_view::npos;
			}
			if (!RequestedResync) throw std::runtime_error("[Renderer:Viewport] Native failure admitted a delta before resync");
		}
		auto Recovery = std::make_shared<RenderPublication>(*Initial);
		Recovery->Id = 43;
		Recovery->AnimationPoseUpdates = Delta->AnimationPoseUpdates;
		if (Renderer.Capture(Recovery).RgbPixels != InitialPixels || !Renderer.Pick(32, 32))
			throw std::runtime_error("[Renderer:Viewport] Full resync did not restore projection and native residency");
		Next->Id = 44;
		Next->BaseId = Recovery->Id;
		if (Renderer.Capture(Next).RgbPixels != InitialPixels)
			throw std::runtime_error("[Renderer:Viewport] Valid delta did not resume after full resync");
	}

	void CheckHdrExposure(EditorViewportRenderer &Renderer) {
		const auto Center = CenterPixel(64, 64);
		RenderPublicationId Id = 100;
		for (const bool Reinhard : {false, true}) for (const auto Exposure : {1.0f / 256.0f, 0.125f, 1.0f, 256.0f}) {
			auto Scene = MaterialScene(Id++, {});
			auto &Light = Scene->Frame.Environment;
			Light.AmbientColor = glm::vec3(0.0f);
			Light.SunIntensity = 8.0f;
			Light.ExposureMultiplier = Exposure;
			Light.ToneMapping = Reinhard ? RenderToneMapping::Reinhard : RenderToneMapping::Filmic;
			Scene->Creates.front().Material.DoubleSided = true;
			const auto Frame = Renderer.Capture(Scene);
			const auto Expected = DisplayByte(8.0, Exposure, Reinhard);
			if (std::abs(int(Frame.RgbPixels.at(Center)) - Expected) > 2)
				throw std::runtime_error(std::format("[Renderer:HDR] Exposure {} yielded {}, expected {}",
					Exposure, Frame.RgbPixels.at(Center), Expected));
		}
	}

	void CheckAdvancedMaterials(EditorViewportRenderer &Renderer) {
		const RenderTextureIdentity Image{404, 1};
		RenderMaterialState Material;
		Material.DoubleSided = true;
		Material.ShadingModel = RenderShadingModel::MetallicRoughness;
		Material.EmissiveFactor = {0.5f, 0.5f, 0.5f};
		Material.EmissiveTexture = Image;
		auto Emissive = MaterialScene(30, Material);
		Emissive->Frame.Environment.AmbientColor = glm::vec3(0.0f);
		Emissive->TextureCreates.push_back({Image, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, SolidPixels(255, 0, 0)});
		const auto Center = CenterPixel(64, 64);
		const auto Emission = Renderer.Capture(Emissive).RgbPixels;
		if (std::abs(int(Emission.at(Center)) - DisplayByte(0.5, 1.0)) > 2 || Emission.at(Center + 1) > 2)
			throw std::runtime_error("[Renderer:Material] Textured emissive radiance failed the no-light probe");

		Material.EmissiveFactor = glm::vec3(0.0f);
		Material.EmissiveTexture.reset();
		Material.OcclusionTexture = Image;
		const auto AmbientProbe = [&](RenderPublicationId Id, float OcclusionStrength, bool DirectOnly) {
			Material.OcclusionStrength = OcclusionStrength;
			auto Scene = MaterialScene(Id, Material);
			if (DirectOnly) {
				Scene->Frame.Environment.AmbientColor = glm::vec3(0.0f);
				Scene->Frame.Environment.SunIntensity = 1.0f;
			}
			Scene->TextureCreates.push_back({Image, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, SolidPixels(0, 255, 255)});
			return Renderer.Capture(Scene).RgbPixels.at(Center);
		};
		const auto Ambient = AmbientProbe(31, 0.0f, false);
		const auto Occluded = AmbientProbe(32, 1.0f, false);
		if (Ambient < 200 || Occluded > 2 || AmbientProbe(33, 0.0f, true) != AmbientProbe(34, 1.0f, true))
			throw std::runtime_error("[Renderer:Material] Occlusion must attenuate indirect light only");
	}

	void CheckMipSampling(EditorViewportRenderer &Renderer) {
		const RenderTextureIdentity Image{404, 1};
		constexpr std::uint32_t Dimension = 256;
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(Dimension * Dimension * 4, 255);
		for (std::uint32_t Y = 0; Y < Dimension; ++Y) for (std::uint32_t X = 0; X < Dimension; ++X) {
			const auto At = (Y * Dimension + X) * 4;
			const std::uint8_t Value = ((X + Y) % 2) ? 255 : 0;
			for (unsigned Channel = 0; Channel < 3; ++Channel) (*Pixels)[At + Channel] = Value;
		}
		const auto Capture = [&](RenderPublicationId Id, RenderTextureMinFilter Min) {
			RenderMaterialState Material;
			Material.DoubleSided = true;
			Material.ShadingModel = RenderShadingModel::MetallicRoughness;
			Material.BaseColorTexture = Image;
			Material.TextureSamplers[0].MinFilter = Min;
			Material.TextureSamplers[0].MagFilter = RenderTextureMagFilter::Linear;
			auto Scene = MaterialScene(Id, Material);
			auto Vertices = std::make_shared<std::vector<RenderVertex>>(*Scene->MeshCreates.front().Vertices);
			const std::array Coordinates{glm::vec2(0, 0), glm::vec2(16, 0), glm::vec2(16, 16), glm::vec2(0, 16)};
			for (std::size_t Index = 0; Index < Coordinates.size(); ++Index)
				(*Vertices)[Index].TextureCoordinate = Coordinates[Index];
			Scene->MeshCreates.front().Vertices = Vertices;
			Scene->TextureCreates.push_back({Image, 1, Dimension, Dimension, RenderTextureFormat::Rgba8Unorm, Pixels});
			return Renderer.Capture(Scene).RgbPixels.at(CenterPixel(64, 64));
		};
		const auto Mipped = Capture(35, RenderTextureMinFilter::LinearMipmapLinear);
		const auto Nearest = Capture(36, RenderTextureMinFilter::Nearest);
		if (std::abs(int(Mipped) - DisplayByte(0.5, 1.0)) > 3 || std::abs(int(Mipped) - int(Nearest)) < 15)
			throw std::runtime_error(std::format("[Renderer:Material] Minified sRGB checker differs from the linear mip reference ({} vs {})",
				Mipped, Nearest));
	}

	void CheckLocalLights(EditorViewportRenderer &Renderer) {
		const auto Center = CenterPixel(64, 64);
		RenderMaterialState Material;
		Material.DoubleSided = true;
		Material.ShadingModel = RenderShadingModel::MetallicRoughness;
		const auto Capture = [&](RenderPublicationId Id, RenderLocalLightKind Kind, const glm::vec3 &Direction) {
			auto Scene = MaterialScene(Id, Material);
			auto &Environment = Scene->Frame.Environment;
			Environment.AmbientColor = glm::vec3(0.0f);
			Environment.LocalLights = std::make_shared<const std::vector<RenderLocalLightState>>(
				std::initializer_list<RenderLocalLightState>{{.Object = {1, 1}, .Kind = Kind, .Position = {0, 0, 1.5f},
					.Direction = Direction, .Color = {0, 0, 1}, .Intensity = 8.0f, .Range = 6.0f,
					.InnerConeCosine = Kind == RenderLocalLightKind::Point ? 1.0f : 0.95f,
					.OuterConeCosine = Kind == RenderLocalLightKind::Point ? 1.0f : 0.8f}});
			return Renderer.Capture(Scene).RgbPixels.at(Center + 2);
		};
		const auto Point = Capture(40, RenderLocalLightKind::Point, {0, 0, -1});
		const auto OnCone = Capture(41, RenderLocalLightKind::Spot, {0, 0, -1});
		const auto OffCone = Capture(42, RenderLocalLightKind::Spot, {1, 0, 0});
		if (Point < 100 || std::abs(int(Point) - int(OnCone)) > 2 || OffCone > 2)
			throw std::runtime_error(std::format("[Renderer:LocalLights] Point/cone response failed ({} {} {})", Point, OnCone, OffCone));
	}

	void CheckLocalLightBudget() {
		SDLRenderer Renderer(Vector2(64, 64), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Scene = MaterialScene(1, {});
		auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
		for (std::uint32_t Index = 1; Index <= 500; ++Index)
			Lights->push_back({.Object = {Index, 1}, .Position = {0, 0, 1.0f}, .Range = 4.0f});
		Lights->push_back({.Object = {501, 1}, .Position = {50, 0, 0}, .Range = 1.0f});
		Scene->Frame.Environment.LocalLights = Lights;
		Renderer.Draw(Scene);
		const auto Metrics = Renderer.GetMetrics();
		if (Metrics.VisibleLocalLights != 500 || Metrics.OverBudgetLightTiles != 4 ||
			Metrics.DroppedLightTileIndices != 4 * (500 - 24) || Metrics.LocalLightUploadBytes == 0)
			throw std::runtime_error("[Renderer:LocalLights] The tiled light budget did not bound overlap");
		auto Unchanged = std::make_shared<RenderPublication>();
		Unchanged->Id = 2;
		Unchanged->BaseId = 1;
		Unchanged->Frame = Scene->Frame;
		Renderer.Draw(Unchanged);
		if (Renderer.GetMetrics().LocalLightUploadBytes != Metrics.LocalLightUploadBytes)
			throw std::runtime_error("[Renderer:LocalLights] An unchanged view rebuilt its tile lists");
	}
}

int main(int ArgumentCount, char **Arguments) {
	const bool RequireHdr = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--require-hdr";
	{
		EditorViewportRenderer Renderer(64, 64);
		if (RequireHdr && !Renderer.UsesHdrScene()) throw std::runtime_error("[Renderer:HDR] RGBA16F scene required");
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
		if (Frame.RgbPixels[Center + 2] <= Frame.RgbPixels[Center] + 100)
			throw std::runtime_error("[Renderer:HDR] Sky face orientation differs from the reference");
		auto RotatedSky = std::make_shared<RenderPublication>();
		RotatedSky->Id = 70;
		RotatedSky->BaseId = SixFaceSky->Id;
		RotatedSky->Frame = SixFaceSky->Frame;
		RotatedSky->Frame.Environment.EnvironmentRotation = 90.0f;
		RotatedSky->EnvironmentChanged = true;
		Frame = Renderer.Capture(RotatedSky);
		if (Frame.RgbPixels[Center] <= Frame.RgbPixels[Center + 2] + 100)
			throw std::runtime_error("[Renderer:HDR] Authored Sky rotation did not change the sampled face");

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
			if (LowExposureFrame.RgbPixels[Center + Channel] != HighExposureFrame.RgbPixels[Center + Channel])
				throw std::runtime_error("[Renderer:HDR] World exposure affected the GUI");

		CheckMaterials(Renderer);
		if (Renderer.UsesHdrScene()) CheckHdrExposure(Renderer);
		CheckAdvancedMaterials(Renderer);
		CheckMipSampling(Renderer);
		CheckLocalLights(Renderer);
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
	CheckPublicationRecovery();
	CheckLocalLightBudget();
	EditorViewportRenderer Fallback(64, 64, false);
	if (Fallback.UsesHdrScene()) throw std::runtime_error("[Renderer:HDR] Direct SDR fallback did not activate");
	CheckMaterials(Fallback);
	return 0;
}

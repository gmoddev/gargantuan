#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	struct EditorViewportRendererTestAccess {
		static void InjectPostSkyFailure(EditorViewportRenderer &Renderer) {
			Renderer.InjectPostSkyFailureForTesting();
		}
		static SDLRendererMetrics GetMetrics(const EditorViewportRenderer &Renderer) {
			return Renderer.GetMetricsForTesting();
		}
	};
}

namespace {
	using namespace gargantuan;
	constexpr std::uint32_t Width = 256, Height = 192;
	constexpr RenderMeshIdentity PanelMesh{1, 1};
	constexpr RenderTextureIdentity RedImage{100, 1}, BlueImage{101, 1};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::shared_ptr<const std::vector<std::uint8_t>> Solid(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue,
		std::uint32_t Dimension = 1) {
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(Dimension * Dimension * 4);
		for (std::size_t Index = 0; Index < Pixels->size(); Index += 4) {
			(*Pixels)[Index] = Red; (*Pixels)[Index + 1] = Green;
			(*Pixels)[Index + 2] = Blue; (*Pixels)[Index + 3] = 255;
		}
		return Pixels;
	}

	std::shared_ptr<const std::vector<std::uint8_t>> DirectionalFace(std::size_t Face) {
		if (Face == 0) return Solid(0, 0, 255, 8);
		if (Face == 1) return Solid(0, 255, 0, 8);
		if (Face != 4) return Solid(8, 8, 8, 8);
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(8 * 8 * 4);
		for (std::uint32_t Y = 0; Y < 8; ++Y) for (std::uint32_t X = 0; X < 8; ++X) {
			const auto Offset = (Y * 8 + X) * 4;
			(*Pixels)[Offset] = static_cast<std::uint8_t>(24 + X * 32);
			(*Pixels)[Offset + 3] = 255;
		}
		return Pixels;
	}

	RenderSkyState Source(RenderTextureIdentity Image, std::uint32_t Dimension, std::uint64_t Revision = 1) {
		RenderSkyState Result{.FaceDimension = Dimension};
		Result.Faces.fill({Image, Revision});
		return Result;
	}

	RenderSkyState DirectionalSource() {
		RenderSkyState Result{.FaceDimension = 8};
		for (std::size_t Face = 0; Face < 6; ++Face) Result.Faces[Face] = {{200 + Face, 1}, 1};
		return Result;
	}

	RenderReflectionProbeState Probe(ObjectId Object, glm::vec3 Position, RenderSkyState Sky) {
		RenderReflectionProbeState Result;
		Result.Object = Object;
		Result.Position = Position;
		Result.HalfSize = {3, 4, 4};
		Result.BlendDistance = 1.5f;
		Result.Source = Sky;
		return Result;
	}

	std::array<int, 3> Pixel(const EditorViewportFrame &Frame, std::uint32_t X, std::uint32_t Y) {
		const auto Offset = (static_cast<std::size_t>(Y) * Frame.Width + X) * 3;
		return {Frame.RgbPixels[Offset], Frame.RgbPixels[Offset + 1], Frame.RgbPixels[Offset + 2]};
	}

	int Difference(const std::array<int, 3> &Left, const std::array<int, 3> &Right) {
		return std::abs(Left[0] - Right[0]) + std::abs(Left[1] - Right[1]) + std::abs(Left[2] - Right[2]);
	}

	void WriteFrame(const std::filesystem::path &Path, const EditorViewportFrame &Frame) {
		std::ofstream File(Path, std::ios::binary);
		Require(static_cast<bool>(File), "[Render:ProbeGpu] Could not create capture image");
		File << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
		File.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()),
			static_cast<std::streamsize>(Frame.RgbPixels.size()));
		Require(static_cast<bool>(File), "[Render:ProbeGpu] Could not write capture image");
	}

	class ProbeSuite final {
	  public:
		explicit ProbeSuite(const std::filesystem::path &Directory)
			: Evidence(Directory), CaptureRenderer(Width, Height), MetricsRenderer({Width, Height},
				{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false}) {
			std::filesystem::create_directories(Evidence);
			Csv.open(Evidence / "ReflectionProbeFrames.csv");
			Require(static_cast<bool>(Csv), "[Render:ProbeGpu] Could not create frame log");
			Csv << "case,iteration,center_r,center_g,center_b,left_r,left_g,left_b,right_r,right_g,right_b,"
				"resident,array_bytes,updates,preprocess_builds,upload_bytes\n";
			Require(CaptureRenderer.UsesHdrScene(), "[Render:ProbeGpu] HDR scene target is required");
		}

		std::uint64_t RedRevision = 1;

		EditorViewportFrame Run(std::string_view Name, std::vector<RenderReflectionProbeState> Probes,
			float CameraX = 0.0f, bool FullResync = false, bool Glossy = false,
			glm::vec3 Shift = {}, bool ReimportRed = false, unsigned Frames = 6, bool Cancel = false) {
			EditorViewportFrame Last;
			for (auto &Value : Probes) Value.Position += Shift;
			for (unsigned Iteration = 0; Iteration < Frames; ++Iteration) {
				auto Publication = std::make_shared<RenderPublication>();
				Publication->Id = NextId++;
				Publication->BaseId = FullResync && Iteration == 0 ? 0 : PreviousId;
				Publication->FullResync = FullResync && Iteration == 0;
				Publication->EnvironmentChanged = true;
				Publication->Frame.ViewportWidth = Width;
				Publication->Frame.ViewportHeight = Height;
				const glm::vec3 Eye = Shift + glm::vec3(CameraX, 0, 8);
				const glm::vec3 Target = Shift + glm::vec3(CameraX, 0, 0);
				auto &Camera = Publication->Frame.Camera;
				Camera.Position = Eye;
				Camera.LookDirection = glm::normalize(Target - Eye);
				Camera.RightDirection = glm::normalize(glm::cross(Camera.LookDirection, glm::vec3(0, 1, 0)));
				Camera.UpDirection = glm::normalize(glm::cross(Camera.RightDirection, Camera.LookDirection));
				Camera.VerticalFieldOfView = 45.0f;
				Camera.FarPlane = 100.0f;
				Camera.ViewMatrix = glm::lookAtRH(Eye, Target, Camera.UpDirection);
				Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(45.0f),
					static_cast<float>(Width) / Height, Camera.NearPlane, Camera.FarPlane);
				Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
				auto &Environment = Publication->Frame.Environment;
				Environment.EnvironmentColor = glm::vec3(0);
				Environment.AmbientColor = glm::vec3(0);
				Environment.SunIntensity = 0;
				Environment.IblIntensity = 1;
				Environment.ReflectionProbes = std::make_shared<const std::vector<RenderReflectionProbeState>>(Probes);
				if (Publication->FullResync) AppendScene(*Publication, Glossy, Shift);
				if (ReimportRed && Iteration == 0) Publication->TextureUpdates.push_back({RedImage,
					RedRevision, 0, 0, 1, 1, Solid(0, 255, 0)});
				if (Cancel) {
					Require(Frames == 1, "[Render:ProbeGpu] Canceled fixture requires one attempted frame");
					EditorViewportRendererTestAccess::InjectPostSkyFailure(CaptureRenderer);
					bool Rejected = false;
					try { (void)CaptureRenderer.Capture(Publication); }
					catch (const std::exception &Error) {
						if (std::string_view(Error.what()).find("injected post-sky failure") == std::string_view::npos) throw;
						Rejected = true;
					}
					Require(Rejected, "[Render:ProbeGpu] Post-sky injection did not cancel the frame");
					MetricsRenderer.Draw(Publication);
					PreviousId = Publication->Id;
					Csv << Name << ",0,0,0,0,0,0,0,0,0,0,0,0,0,0,"
						<< CaptureMetrics().ProbeUploadBytes << '\n' << std::flush;
					return {};
				}
				Last = CaptureRenderer.Capture(Publication);
				MetricsRenderer.Draw(Publication);
				PreviousId = Publication->Id;
				const auto Center = Pixel(Last, Width / 2, Height / 2);
				const auto Left = Pixel(Last, Width / 4, Height / 2);
				const auto Right = Pixel(Last, Width * 3 / 4, Height / 2);
				const auto Metrics = MetricsRenderer.GetMetrics();
				Csv << Name << ',' << Iteration << ',' << Center[0] << ',' << Center[1] << ',' << Center[2]
					<< ',' << Left[0] << ',' << Left[1] << ',' << Left[2]
					<< ',' << Right[0] << ',' << Right[1] << ',' << Right[2]
					<< ',' << Metrics.ResidentReflectionProbes << ',' << Metrics.ProbeArrayBytes
					<< ',' << Metrics.ProbeUpdates << ',' << Metrics.ProbePreprocessBuilds
					<< ',' << Metrics.ProbeUploadBytes << '\n';
				Require(Metrics.ResidentReflectionProbes <= MaximumResidentReflectionProbes &&
					Metrics.ProbeArrayBytes <= 4ull * 1024 * 1024,
					"[Render:ProbeGpu] Probe array residency exceeded its bound");
			}
			WriteFrame(Evidence / (std::string(Name) + ".ppm"), Last);
			return Last;
		}

		[[nodiscard]] SDLRendererMetrics Metrics() const { return MetricsRenderer.GetMetrics(); }
		[[nodiscard]] SDLRendererMetrics CaptureMetrics() const {
			return EditorViewportRendererTestAccess::GetMetrics(CaptureRenderer);
		}

	  private:
		void AppendScene(RenderPublication &Publication, bool Glossy, glm::vec3 Shift) {
			Publication.TextureCreates.push_back({RedImage, RedRevision, 1, 1, RenderTextureFormat::Rgba8Unorm,
				RedRevision == 1 ? Solid(255, 8, 8) : Solid(0, 255, 0)});
			Publication.TextureCreates.push_back({BlueImage, 1, 1, 1, RenderTextureFormat::Rgba8Unorm,
				Solid(8, 8, 255)});
			for (std::size_t Face = 0; Face < 6; ++Face)
				Publication.TextureCreates.push_back({{200 + Face, 1}, 1, 8, 8,
					RenderTextureFormat::Rgba8Unorm, DirectionalFace(Face)});
			Publication.MeshCreates.push_back({
				.Mesh = PanelMesh, .TopologyRevision = 1, .VertexRevision = 1,
				.Vertices = std::make_shared<const std::vector<RenderVertex>>(
					std::initializer_list<RenderVertex>{
						{{-6, -4, 0}, {0, 0, 1}}, {{6, -4, 0}, {0, 0, 1}},
						{{6, 4, 0}, {0, 0, 1}}, {{-6, 4, 0}, {0, 0, 1}},
					}),
				.Indices = std::make_shared<const std::vector<std::uint32_t>>(
					std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3}),
				.Bounds = {{-6, -4, 0}, {6, 4, 0}},
			});
			RenderItem Item;
			Item.Object = {1, 1};
			Item.CastShadow = false;
			Item.ModelMatrix = glm::translate(glm::mat4(1), Shift);
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			RenderMaterialState Material;
			Material.ShadingModel = RenderShadingModel::MetallicRoughness;
			Material.BaseColorFactor = {1, 1, 1, 1};
			Material.Metallic = Glossy ? 1.0f : 0.0f;
			Material.Roughness = Glossy ? 0.05f : 1.0f;
			Material.DoubleSided = true;
			Publication.Creates.push_back({.Item = Item, .Mesh = PanelMesh, .Material = Material});
		}

		std::filesystem::path Evidence;
		EditorViewportRenderer CaptureRenderer;
		SDLRenderer MetricsRenderer;
		std::ofstream Csv;
		RenderPublicationId NextId = 1, PreviousId = 0;
	};

	void RunCases(const std::filesystem::path &Evidence) {
		ProbeSuite Suite(Evidence);
		const auto Red = Probe({10, 1}, {-2, 0, 0}, Source(RedImage, 1));
		const auto Blue = Probe({11, 1}, {2, 0, 0}, Source(BlueImage, 1));
		const auto Empty = Suite.Run("outdoor-global-only", {}, 0, true);
		const auto EmptyCenter = Pixel(Empty, Width / 2, Height / 2);
		Require(std::max({EmptyCenter[0], EmptyCenter[1], EmptyCenter[2]}) <= 8,
			"[Render:ProbeGpu] Unlit baseline unexpectedly contains local radiance");
		const auto Left = Suite.Run("red-room", {Red, Blue}, -2);
		const auto BuildsAfterAdmission = Suite.Metrics().ProbePreprocessBuilds;
		const auto Right = Suite.Run("blue-room", {Red, Blue}, 2);
		const auto RedCenter = Pixel(Left, Width / 2, Height / 2);
		const auto BlueCenter = Pixel(Right, Width / 2, Height / 2);
		Require(RedCenter[0] > RedCenter[2] + 35 && BlueCenter[2] > BlueCenter[0] + 35,
			"[Render:ProbeGpu] Adjacent local rooms do not preserve their colored radiance");
		const auto Doorway = Suite.Run("doorway-blend", {Red, Blue});
		const auto Blend = Pixel(Doorway, Width / 2, Height / 2);
		Require(Blend[0] > EmptyCenter[0] + 25 && Blend[2] > EmptyCenter[2] + 25,
			"[Render:ProbeGpu] Doorway loses the two local contributions");
		const auto Outside = Suite.Run("outside-both-volumes", {Red, Blue}, 5);
		Require(Difference(Pixel(Outside, Width / 2, Height / 2), EmptyCenter) <= 8,
			"[Render:ProbeGpu] Local lighting leaked outside both authored volumes");
		auto PriorityRed = Red, PriorityBlue = Blue;
		PriorityRed.Position = PriorityBlue.Position = {0, 0, 0};
		PriorityRed.Priority = 2;
		const auto Priority = Suite.Run("overlap-priority", {PriorityRed, PriorityBlue});
		const auto PriorityColor = Pixel(Priority, Width / 2, Height / 2);
		Require(PriorityColor[0] > PriorityColor[2] + 25,
			"[Render:ProbeGpu] Highest-priority overlapping probe did not dominate");
		Require(Suite.Metrics().ProbePreprocessBuilds == BuildsAfterAdmission,
			"[Render:ProbeGpu] Camera movement, blend, or priority needlessly rebuilt a probe source");
		auto CanceledRed = PriorityRed;
		CanceledRed.Priority += 1;
		Suite.Run("canceled-probe-record-upload", {CanceledRed, PriorityBlue}, 0, false, false, {}, false, 1, true);
		const auto BytesAfterCancel = Suite.CaptureMetrics().ProbeUploadBytes;
		const auto RestoredPriority = Suite.Run("restored-after-canceled-upload", {PriorityRed, PriorityBlue},
			0, false, false, {}, false, 1);
		Require(RestoredPriority.RgbPixels == Priority.RgbPixels,
			"[Render:ProbeGpu] Canceled probe update changed the restored frame");
		Require(Suite.CaptureMetrics().ProbeUploadBytes >= BytesAfterCancel + 2 * 112,
			"[Render:ProbeGpu] Canceled upload left the cycled record backing trusted");
		Suite.RedRevision = 2;
		PriorityRed.Source = Source(RedImage, 1, 2);
		const auto BuildsBeforeReimport = Suite.Metrics().ProbePreprocessBuilds;
		const auto Reimported = Suite.Run("source-face-reimport", {PriorityRed}, 0, false, false, {}, true);
		const auto ReimportedColor = Pixel(Reimported, Width / 2, Height / 2);
		Require(ReimportedColor[1] > ReimportedColor[0] + 35,
			"[Render:ProbeGpu] Same-identity source reimport left stale probe radiance");
		Require(Suite.Metrics().ProbePreprocessBuilds == BuildsBeforeReimport + 1,
			"[Render:ProbeGpu] Source-face reimport did not rebuild exactly one probe source");
		const auto Resynced = Suite.Run("source-resync", {PriorityRed}, 0, true);
		Require(Resynced.RgbPixels == Reimported.RgbPixels,
			"[Render:ProbeGpu] Full resync changed the settled reimported probe frame");
		const auto Removed = Suite.Run("probe-removed", {});
		Require(Difference(Pixel(Removed, Width / 2, Height / 2), EmptyCenter) <= 8,
			"[Render:ProbeGpu] Removed probe left local light behind");
		const auto Added = Suite.Run("probe-added", {PriorityRed});
		Require(Difference(Pixel(Added, Width / 2, Height / 2), ReimportedColor) <= 8,
			"[Render:ProbeGpu] Re-added probe failed to restore its source radiance");
		const glm::vec3 LargeShift(900000, 0, 0);
		const auto Large = Suite.Run("large-coordinate-room", {PriorityRed}, 0, true, false, LargeShift);
		Require(Difference(Pixel(Large, Width / 2, Height / 2), ReimportedColor) <= 18,
			"[Render:ProbeGpu] Large world coordinates changed local probe lighting beyond float tolerance");

		auto Directional = Probe({12, 1}, {0, 0, 0}, DirectionalSource());
		Directional.HalfSize = {4, 4, 4};
		const auto Unrotated = Suite.Run("directional-unrotated", {Directional}, 0, true, true);
		Directional.Rotation = glm::mat3(glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 1, 0)));
		const auto Rotated = Suite.Run("directional-rotated", {Directional}, 0, false, true);
		Require(Difference(Pixel(Unrotated, Width / 2, Height / 2), Pixel(Rotated, Width / 2, Height / 2)) > 25,
			"[Render:ProbeGpu] Probe rotation did not redirect the reflection source");
		Directional.Rotation = glm::mat3(1);
		Directional.CaptureOffset = {2.5f, 0, 0};
		const auto Offset = Suite.Run("directional-capture-offset", {Directional}, 0, false, true);
		Require(Difference(Pixel(Unrotated, Width / 2, Height / 2), Pixel(Offset, Width / 2, Height / 2)) > 8,
			"[Render:ProbeGpu] Capture offset did not change box-projected reflection sampling");

		std::vector<RenderReflectionProbeState> Many;
		for (std::uint32_t Index = 0; Index < 16; ++Index) {
			auto Value = Probe({100 + Index, 1}, {static_cast<float>(Index) * 2.0f - 15.0f, 0, 0},
				Source(Index & 1 ? BlueImage : RedImage, 1, Index & 1 ? 1 : Suite.RedRevision));
			Many.push_back(Value);
		}
		Suite.Run("resident-budget-16-authored", Many, 0, true, false, {}, false, 16);
		Suite.Run("resident-budget-shift", Many, 4, false, false, {}, false, 8);
		Require(Suite.Metrics().ProbeArrayBytes <= 4ull * 1024 * 1024 &&
			Suite.Metrics().ResidentReflectionProbes == MaximumResidentReflectionProbes,
			"[Render:ProbeGpu] Sixteen authored probes exceeded or failed twelve-slot residency");
		std::vector<RenderReflectionProbeState> FarPriority;
		for (std::uint32_t Index = 0; Index < 12; ++Index) {
			auto Value = Probe({200 + Index, 1}, {1000.0f + 10.0f * Index, 0, 0}, Source(BlueImage, 1));
			Value.Priority = 10;
			FarPriority.push_back(Value);
		}
		FarPriority.push_back(Probe({220, 1}, {0, 0, 0}, Source(RedImage, 1, Suite.RedRevision)));
		const auto Nearby = Suite.Run("nearby-over-far-priority-residency", FarPriority, 0, true, false, {}, false, 16);
		const auto NearbyColor = Pixel(Nearby, Width / 2, Height / 2);
		Require(NearbyColor[1] > NearbyColor[2] + 35,
			"[Render:ProbeGpu] Distant high-priority probes evicted the only nearby probe");
		std::cout << "[Render:ProbeGpu] captures=" << Evidence.string() << " resident="
			<< Suite.Metrics().ResidentReflectionProbes << " updates=" << Suite.Metrics().ProbeUpdates << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const auto Evidence = ArgumentCount >= 2 ? std::filesystem::path(Arguments[1]) :
			std::filesystem::current_path() / "ReflectionProbeGpuEvidence";
		RunCases(Evidence);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:ProbeGpu] " << Error.what() << '\n';
		return 1;
	}
}

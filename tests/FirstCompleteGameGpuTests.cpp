#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;

	struct SDLGuard {
		SDLGuard() {
			if (!SDL_Init(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("Failed to initialize SDL video: ") + SDL_GetError());
		}
		~SDLGuard() { SDL_Quit(); }
	};

	class TestWorkspace final {
	  public:
		TestWorkspace() {
			const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			Root = std::filesystem::temp_directory_path() / ("gargantuan-first-complete-game-gpu-" + Suffix);
			std::filesystem::create_directory(Root);
			ProjectRoot = Root / "Project";
			PackageRoot = Root / "Package";
			std::filesystem::copy(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), ProjectRoot,
				std::filesystem::copy_options::recursive);
		}

		~TestWorkspace() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		std::filesystem::path Root;
		std::filesystem::path ProjectRoot;
		std::filesystem::path PackageRoot;
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	bool Near(const glm::mat4 &Left, const glm::mat4 &Right, float Epsilon = 2e-3f) {
		for (std::size_t Column = 0; Column < 4; ++Column)
			for (std::size_t Row = 0; Row < 4; ++Row)
				if (std::abs(Left[Column][Row] - Right[Column][Row]) > Epsilon) return false;
		return true;
	}

	bool Near(const glm::vec3 &Left, const glm::vec3 &Right, float Epsilon = 2e-3f) {
		return glm::length(Left - Right) <= Epsilon;
	}

	glm::mat4 FrameMatrix(const CFrame &Frame) {
		auto Matrix = glm::mat4(Frame.Rotation);
		Matrix[3] = glm::vec4(Frame.Position, 1.0f);
		return Matrix;
	}

	const RenderAnimationPoseUpdate *FindPose(const RenderPublication &Publication, ObjectId Object) {
		const auto Found = std::ranges::find_if(Publication.AnimationPoseUpdates,
			[&](const auto &Pose) { return Pose.Object == Object; });
		return Found == Publication.AnimationPoseUpdates.end() ? nullptr : &*Found;
	}

	bool HasPackageError(const std::vector<PackageDiagnostic> &Diagnostics) {
		return std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		});
	}

	class InspectingRenderer final : public BaseRenderer {
	  public:
		explicit InspectingRenderer(const Vector2 &ViewportSize)
			: Backend(ViewportSize,
				SDLRendererOptions{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false}) {}

		void Draw(RenderPublicationPtr Publication) override {
			LastPublication = Publication;
			Backend.Draw(std::move(Publication));
		}
		void Resize(int Width, int Height) override { Backend.Resize(Width, Height); }
		void Destroy() override { Backend.Destroy(); }
		[[nodiscard]] RendererCapabilities GetCapabilities() const override { return Backend.GetCapabilities(); }
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return Backend.GetViewportSize();
		}
		[[nodiscard]] SDLRendererMetrics GetMetrics() const { return Backend.GetMetrics(); }
		[[nodiscard]] std::string GetDriverName() const { return Backend.GetDriverName(); }
		[[nodiscard]] RenderPublicationPtr TakePublication() { return std::move(LastPublication); }

	  private:
		SDLRenderer Backend;
		RenderPublicationPtr LastPublication;
	};

	RenderPublicationPtr StepAndTake(Engine &RuntimeEngine, InspectingRenderer &Renderer) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		RuntimeEngine.Step();
		auto Publication = Renderer.TakePublication();
		Require(Publication != nullptr, "packaged Engine did not publish a render frame");
		return Publication;
	}
}

int main() {
	try {
		const SDLGuard SDL;
		TestWorkspace WorkspaceRoot;
		BootstrapProjectRuntimeSchema(WorkspaceRoot.ProjectRoot);
		DiskFilesystem Filesystem(WorkspaceRoot.ProjectRoot);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto AuthoringWorld = ProjectValue.DeserializeGame();
		AuthoringWorld->InitializeLoadedProjectRevision();
		AuthoringWorld->Root = WorkspaceRoot.ProjectRoot;
		AuthoringWorld->Filesystem = &Filesystem;
		const auto Revision = AuthoringWorld->GetAuthoritativeRevision();
		auto Payload = PackageBuilder::Capture(ProjectValue, AuthoringWorld, Revision, Revision);
		auto Build = PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = std::filesystem::path(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
			.OutputDirectory = WorkspaceRoot.PackageRoot,
			.Configuration = PackageConfiguration::Release,
		});
		Require(Build.Ok && !HasPackageError(Build.Diagnostics),
			"FirstCompleteGame Release package did not build for the GPU proof");
		Require(!HasPackageError(PackageBuilder::Validate(WorkspaceRoot.PackageRoot)),
			"FirstCompleteGame Release package failed validation before GPU launch");
		std::vector<PackageDiagnostic> LoadDiagnostics;
		auto Loaded = PackageBuilder::Load(WorkspaceRoot.PackageRoot, LoadDiagnostics);
		Require(Loaded && !HasPackageError(LoadDiagnostics),
			"FirstCompleteGame Release package could not be loaded");
		AuthoringWorld->Destroy();
		AuthoringWorld.reset();
		std::filesystem::remove_all(WorkspaceRoot.ProjectRoot);
		BootstrapPackagedRuntimeSchema(Loaded->PreRunSource);
		auto RuntimeWorld = PackageBuilder::LoadWorld(*Loaded, WorkspaceRoot.PackageRoot);
		auto WorkspaceValue = RuntimeWorld ?
			std::dynamic_pointer_cast<Workspace>(RuntimeWorld->GetService("Workspace")) : nullptr;
		auto AnimatedBeacon = WorkspaceValue ? std::dynamic_pointer_cast<MeshPart>(
			WorkspaceValue->FindFirstChild("AnimatedBeacon", true)) : nullptr;
		auto BeaconAnchor = AnimatedBeacon ? std::dynamic_pointer_cast<Attachment>(
			AnimatedBeacon->FindFirstChild("BeaconTipAnchor", false)) : nullptr;
		Require(AnimatedBeacon && BeaconAnchor && BeaconAnchor->GetJointPath() == "BeaconRoot/BeaconTip",
			"FirstCompleteGame GPU proof could not find the semantic AnimatedBeacon anchor");
		const auto BeaconObject = AnimatedBeacon->GetObjectId();

		auto Renderer = std::make_unique<InspectingRenderer>(Vector2(320.0f, 200.0f));
		Require(Renderer->GetCapabilities().GpuSkinning,
			"the production SDL renderer did not select GPU skinning capability");
		Engine RuntimeEngine(RuntimeWorld, Renderer.get());
		RuntimeEngine.ProcessService->Alive = true;
		std::uint64_t InitialRevision = 0;
		for (std::size_t Attempt = 0; InitialRevision == 0 && Attempt < 30; ++Attempt) {
			auto Publication = StepAndTake(RuntimeEngine, *Renderer);
			if (const auto *Pose = FindPose(*Publication, BeaconObject)) {
				Require(Pose->Mode == RenderAnimationSkinningMode::GpuPalette &&
					!Pose->PosedMesh.IsValid() && Pose->Palette.Entries &&
					Publication->MeshVertexUpdates.empty(),
					"packaged AnimatedBeacon did not enter the renderer-neutral GPU palette path");
				InitialRevision = Pose->PoseRevision;
			}
		}
		Require(InitialRevision != 0, "packaged AnimatedBeacon never published a GPU palette pose");
		auto InitialSemantic = RuntimeEngine.Spatial->ResolveAttachment(BeaconAnchor);
		auto InitialPose = RuntimeEngine.Animation->GetPose(BeaconObject);
		auto MeshResource = RuntimeEngine.Assets->ResolveMeshResource(AnimatedBeacon->GetMesh());
		Require(InitialSemantic && InitialSemantic->Animated && InitialPose &&
			InitialPose->JointModelTransforms && InitialPose->SkinPalette && MeshResource &&
			MeshResource->Value.Skeleton && MeshResource->Value.Skeleton->Joints,
			"packaged GPU proof did not retain the renderer-independent semantic pose");
		const auto &Joints = *MeshResource->Value.Skeleton->Joints;
		const auto Joint = std::ranges::find_if(Joints, [](const auto &Value) {
			return Value.Path == "BeaconRoot/BeaconTip";
		});
		Require(Joint != Joints.end(), "packaged Mesh artifact omitted the canonical BeaconTip joint path");
		const auto JointIndex = static_cast<std::size_t>(std::distance(Joints.begin(), Joint));
		Require(JointIndex < InitialPose->JointModelTransforms->size() &&
			JointIndex < InitialPose->SkinPalette->size(),
			"packaged semantic joint index exceeded the evaluated pose");
		const auto OwnerMatrix = FrameMatrix(AnimatedBeacon->GetCFrame()) *
			glm::scale(glm::mat4(1.0f), AnimatedBeacon->GetSize());
		const auto LocalMatrix = FrameMatrix(BeaconAnchor->GetCFrame());
		const auto ExpectedSemantic = OwnerMatrix * (*InitialPose->JointModelTransforms)[JointIndex] * LocalMatrix;
		const auto BindSocket = glm::inverse(Joint->InverseBindMatrix) *
			glm::vec4(BeaconAnchor->GetCFrame().Position, 1.0f);
		const auto GpuSocket = OwnerMatrix *
			((*InitialPose->SkinPalette)[JointIndex].PositionMatrix * BindSocket);
		Require(Near(InitialSemantic->Matrix, ExpectedSemantic) &&
			Near(InitialSemantic->WorldCFrame.Position, glm::vec3(GpuSocket)),
			"semantic anchor diverged from the packaged GPU palette's skinned socket");
		const auto Baseline = Renderer->GetMetrics();
		Require(Baseline.GpuSkinningRigs >= 1 && Baseline.CpuFallbackRigs == 0 &&
			Baseline.SkinnedSourceResourceCreations == 1 && Baseline.CpuSkinnedVertexUploads == 0,
			"initial FirstCompleteGame GPU residency did not share one skinned source mesh");

		std::uint64_t LatestRevision = InitialRevision;
		std::size_t BeaconPaletteUpdates = 0;
		for (std::size_t Frame = 0; Frame < 12; ++Frame) {
			auto Publication = StepAndTake(RuntimeEngine, *Renderer);
			if (const auto *Pose = FindPose(*Publication, BeaconObject)) {
				Require(Pose->Mode == RenderAnimationSkinningMode::GpuPalette &&
					!Pose->PosedMesh.IsValid() && Publication->MeshVertexUpdates.empty() &&
					Publication->MeshCreates.empty(),
					"packaged AnimatedBeacon steady state published CPU-skinned dynamic geometry");
				Require(Pose->PoseRevision > LatestRevision,
					"packaged AnimatedBeacon pose revisions did not increase monotonically");
				LatestRevision = Pose->PoseRevision;
				++BeaconPaletteUpdates;
			}
		}
		const auto Steady = Renderer->GetMetrics();
		Require(BeaconPaletteUpdates > 0 && LatestRevision > InitialRevision &&
			Steady.PaletteUploads - Baseline.PaletteUploads >= BeaconPaletteUpdates &&
			Steady.VertexBufferCreations == Baseline.VertexBufferCreations &&
			Steady.IndexBufferCreations == Baseline.IndexBufferCreations &&
			Steady.TransferBufferCreations == Baseline.TransferBufferCreations &&
			Steady.PipelineCreations == Baseline.PipelineCreations &&
			Steady.ShaderCreations == Baseline.ShaderCreations &&
			Steady.PaletteBufferCreations == Baseline.PaletteBufferCreations &&
			Steady.PaletteTransferBufferCreations == Baseline.PaletteTransferBufferCreations &&
			Steady.PaletteScratchAllocations == Baseline.PaletteScratchAllocations &&
			Steady.CpuSkinnedVertexUploads == 0 && Steady.MainShadowPoseMismatches == 0,
			"FirstCompleteGame GPU animation created resources or CPU vertex uploads after warmup");
		auto SteadySemantic = RuntimeEngine.Spatial->ResolveAttachment(BeaconAnchor);
		Require(SteadySemantic && SteadySemantic->Animated &&
			SteadySemantic->Revision > InitialSemantic->Revision &&
			glm::length(SteadySemantic->WorldCFrame.Position - InitialSemantic->WorldCFrame.Position) > 0.01f,
			"packaged semantic anchor did not follow the advancing GPU-skinned pose");

		auto RestartedRenderer = std::make_unique<InspectingRenderer>(Vector2(320.0f, 200.0f));
		RuntimeEngine.Renderer = RestartedRenderer.get();
		Renderer.reset();
		RuntimeEngine.RenderPublishing.RequestFullResync();
		auto CurrentFullPose = StepAndTake(RuntimeEngine, *RestartedRenderer);
		Require(CurrentFullPose != nullptr,
			"renderer restart did not receive the current complete AnimatedBeacon pose");
		const auto *RestartPose = FindPose(*CurrentFullPose, BeaconObject);
		Require(CurrentFullPose->FullResync && RestartPose && RestartPose->PoseRevision > LatestRevision,
			"requesting renderer resync restarted or regressed animation time");
		auto RestartSemantic = RuntimeEngine.Spatial->ResolveAttachment(BeaconAnchor);
		Require(RestartSemantic && RestartSemantic->Animated &&
			RestartSemantic->Revision > SteadySemantic->Revision,
			"renderer replacement interrupted the renderer-independent semantic anchor pose");
		const auto Restarted = RestartedRenderer->GetMetrics();
		Require(Restarted.GpuSkinningRigs >= 1 && Restarted.PaletteUploads >= 1 &&
			Restarted.PaletteBufferCreations >= 1 && Restarted.SkinnedSourceResourceCreations == 1 &&
			Restarted.CpuSkinnedVertexUploads == 0 && Restarted.MainShadowPoseMismatches == 0,
			"fresh SDL renderer did not recreate the current AnimatedBeacon GPU pose");

		std::cout << "[Game:FirstCompleteGameGpu] package=Release driver=" << RestartedRenderer->GetDriverName()
			<< " poseUpdates=" << BeaconPaletteUpdates
			<< " paletteUploads=" << Steady.PaletteUploads - Baseline.PaletteUploads
			<< " paletteBytes=" << Steady.PaletteUploadBytes - Baseline.PaletteUploadBytes
			<< " skinnedSourceResources=" << Steady.SkinnedSourceResourceCreations
			<< " cpuVertexUploads=" << Steady.CpuSkinnedVertexUploads
			<< " rendererRestart=PASS\n";
		RuntimeEngine.Destroy();
		RestartedRenderer.reset();
		RuntimeWorld.reset();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Game:FirstCompleteGameGpu] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

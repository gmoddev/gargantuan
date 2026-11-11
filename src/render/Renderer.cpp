// #define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"

#include "gargantuan/Log.hpp"
#include "render/sdl/SDLDirectionalShadowCache.hpp"
#include "render/sdl/SDLInstanceGpuCache.hpp"
#include "render/sdl/SDLMaterialSamplers.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSceneColor.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "render/sdl/SDLShadowResources.hpp"
#include "render/sdl/SDLSpotShadowAtlas.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <atomic>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;

		std::uint64_t Nanoseconds(Clock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
		}

		std::uint64_t NextVisibilityGeneration() {
			static std::atomic<std::uint64_t> Next{1};
			const auto Result = Next.fetch_add(1, std::memory_order_relaxed);
			if (Result == 0) throw std::overflow_error("Renderer visibility generation exhausted");
			return Result;
		}
	}

	void BaseRenderer::Draw(RenderSnapshotPtr Snapshot) {
		if (!Snapshot) throw std::invalid_argument("Renderer compatibility draw requires an immutable RenderSnapshot");
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Snapshot->Id;
		Publication->FullResync = true;
		Publication->Frame = {
			Snapshot->ViewportWidth, Snapshot->ViewportHeight, 1.0f, Snapshot->Camera, Snapshot->Environment,
		};
		Publication->EnvironmentChanged = true;
		Publication->Diagnostics = Snapshot->Diagnostics;
		Publication->Creates.reserve(Snapshot->Items.size());
		for (const auto &Item : Snapshot->Items) {
			RenderMaterialState Material;
			Material.BaseColorFactor = Item.Color;
			Material.OpacityMode = Item.Color.a < 1.0f ? RenderOpacityMode::Transparent : RenderOpacityMode::Opaque;
			Publication->Creates.push_back({Item, std::nullopt, Material});
		}
		Draw(std::shared_ptr<const RenderPublication>(std::move(Publication)));
	}

	struct SDLRenderer::Backend final {
		int Width = 0;
		int Height = 0;
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTextureFormat SwapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDLRendererOptions Options;
		bool WindowClaimed = false;
		SDL_GPUTexture *ColorTexture = nullptr;
		SDL_GPUTexture *HdrSceneTexture = nullptr;
		bool HdrSceneEnabled = false;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		std::vector<std::unique_ptr<SDLRenderPass>> RenderPasses;
		std::unique_ptr<SDLMeshCache> MeshResources;
		std::unique_ptr<SDLInstanceGpuCache> InstanceResources;
		std::unique_ptr<SDLTextureCache> TextureResources;
		SDLMaterialSamplers MaterialSamplers;
		std::unique_ptr<SDLSkinPaletteCache> SkinPalettes;
		RenderProjection Projection;
		std::vector<ObjectId> VisibleAnimationObjects;
		std::uint64_t VisibilityGeneration = 0;
		RenderPublicationId VisibilityPublication = InvalidRenderPublicationId;
		bool VisibilityComplete = false;
		SDLDirectionalShadowCache ShadowCache;
		SDLShadowResources ShadowResources;
		SDLSpotShadowAtlas SpotShadowAtlas;
		std::unordered_map<ObjectId, std::uint64_t> ShadowPoseRevisions;
		SDLRendererMetrics Metrics;
		std::uint32_t RemainingPaletteUploadFailures = 0;
		std::uint32_t RemainingShaderCreationFailures = 0;
		std::uint32_t RemainingPipelineCreationFailures = 0;
		bool RequiresFullResync = false;
	};

	const std::vector<SDLRenderPassDefinition> &GetSDLRenderPassConstructors() {
		static const std::vector<SDLRenderPassDefinition> Constructors{
			{CreateShadowPass, SDLPassTarget::Scene, SDLPassPhase::Shadow},
			{CreateSkyPass, SDLPassTarget::Scene, SDLPassPhase::Sky},
			{CreateOpaquePass, SDLPassTarget::Scene, SDLPassPhase::Opaque},
			{CreateSceneResolvePass, SDLPassTarget::Resolve, SDLPassPhase::Resolve},
			{CreateGuiPass, SDLPassTarget::Output, SDLPassPhase::Gui},
		};
		return Constructors;
	}

	SDLRenderer::SDLRenderer(const Vector2 &ViewportSize) : SDLRenderer(ViewportSize, {}) {}

	SDLRenderer::SDLRenderer(const Vector2 &ViewportSize, SDLRendererOptions Options)
		: BaseRenderer(ViewportSize), State(std::make_unique<Backend>()) {
		static constexpr auto WindowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		static constexpr auto ShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_METALLIB |
			SDL_GPU_SHADERFORMAT_MSL;

		State->Options = Options;
		State->VisibilityGeneration = NextVisibilityGeneration();
		State->VisibleAnimationObjects.reserve(4'096);
		State->RemainingPaletteUploadFailures = Options.InjectPaletteUploadFailures;
		State->RemainingShaderCreationFailures = Options.InjectShaderCreationFailures;
		State->RemainingPipelineCreationFailures = Options.InjectPipelineCreationFailures;
		State->Gpu = SDL_CreateGPUDevice(ShaderFormats, Options.DebugDevice, nullptr);
		if (!State->Gpu) throw std::runtime_error(std::format("Failed to create GPU device: {}", SDL_GetError()));

		try {
			if (!Options.Offscreen) {
				State->Window = SDL_CreateWindow("Gargantuan", ViewportSize.GetX(), ViewportSize.GetY(), WindowFlags);
				if (!State->Window) throw std::runtime_error(std::format("Failed to create window: {}", SDL_GetError()));
				if (!SDL_ClaimWindowForGPUDevice(State->Gpu, State->Window))
					throw std::runtime_error(std::format("Failed to claim window for GPU: {}", SDL_GetError()));
				State->WindowClaimed = true;
				State->SwapchainFormat = SDL_GetGPUSwapchainTextureFormat(State->Gpu, State->Window);
			} else State->SwapchainFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			State->HdrSceneEnabled = Options.EnableHdrScene && SupportsSDLHdrScene(State->Gpu);
			if (Options.EnableInstancing) {
				const auto SceneFormat = State->HdrSceneEnabled ? SDLHdrSceneFormat : State->SwapchainFormat;
				const float OutputMode = SceneFormat == SDLHdrSceneFormat ? 2.0f :
					SDLTargetEncodesSrgb(SceneFormat) ? 0.0f : 1.0f;
				State->InstanceResources = std::make_unique<SDLInstanceGpuCache>(State->Gpu, OutputMode);
			}
			State->MeshResources = std::make_unique<SDLMeshCache>(State->Gpu, &State->Metrics);
			State->TextureResources = std::make_unique<SDLTextureCache>(State->Gpu, &State->Metrics);
			State->SkinPalettes = std::make_unique<SDLSkinPaletteCache>(
				State->Gpu, &State->Metrics, &State->RemainingPaletteUploadFailures);

			SDL_GPUTextureCreateInfo ShadowMapInfo{
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = 2048,
				.height = 2048,
				.layer_count_or_depth = 1,
				.num_levels = 1,
			};
			State->ShadowMapTexture = SDL_CreateGPUTexture(State->Gpu, &ShadowMapInfo);
			SDL_GPUSamplerCreateInfo SamplerInfo{
				.min_filter = SDL_GPU_FILTER_LINEAR,
				.mag_filter = SDL_GPU_FILTER_LINEAR,
				.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
				.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
				.enable_compare = true,
			};
			State->ShadowSampler = SDL_CreateGPUSampler(State->Gpu, &SamplerInfo);
			if (!State->ShadowMapTexture || !State->ShadowSampler)
				throw std::runtime_error(std::format("Failed to create shadow resources: {}", SDL_GetError()));

			int InitialWidth = static_cast<int>(ViewportSize.GetX());
			int InitialHeight = static_cast<int>(ViewportSize.GetY());
			if (!Options.Offscreen && !SDL_GetWindowSizeInPixels(State->Window, &InitialWidth, &InitialHeight))
				throw std::runtime_error(std::format("Failed to query renderer window size: {}", SDL_GetError()));
			Resize(InitialWidth, InitialHeight);

			for (const auto &Definition : GetSDLRenderPassConstructors()) {
				if (Definition.Target == SDLPassTarget::Resolve && !State->HdrSceneEnabled) continue;
				if (State->RemainingShaderCreationFailures > 0) {
					--State->RemainingShaderCreationFailures;
					throw std::runtime_error("[Render:Shader] injected shader creation failure");
				}
				const auto Format = State->HdrSceneEnabled && Definition.Target == SDLPassTarget::Scene
					? SDLHdrSceneFormat : State->SwapchainFormat;
				auto Pass = Definition.Construct(State->Gpu, Format, &State->Metrics);
				if (!Pass) throw std::runtime_error("Failed to construct renderer pass");
				if (State->RemainingPipelineCreationFailures > 0) {
					--State->RemainingPipelineCreationFailures;
					Pass->Destroy(State->Gpu);
					throw std::runtime_error("[Render:Pipeline] injected pipeline creation failure");
				}
				State->RenderPasses.push_back(std::move(Pass));
			}
		} catch (...) {
			Destroy();
			throw;
		}
	}

	SDLRenderer::~SDLRenderer() { Destroy(); }

	void SDLRenderer::Destroy() {
		if (!State) return;
		State->VisibilityComplete = false;
		State->VisibleAnimationObjects.clear();
		if (State->Gpu) SDL_WaitForGPUIdle(State->Gpu);
		if (State->MeshResources) {
			State->MeshResources->Destroy();
			State->MeshResources.reset();
		}
		if (State->InstanceResources) {
			State->InstanceResources->Destroy();
			State->InstanceResources.reset();
		}
		if (State->TextureResources) {
			State->TextureResources->Destroy();
			State->TextureResources.reset();
		}
		if (State->SkinPalettes) {
			State->SkinPalettes->Destroy();
			State->SkinPalettes.reset();
		}
		for (auto &Pass : State->RenderPasses) if (Pass) Pass->Destroy(State->Gpu);
		State->RenderPasses.clear();
		if (State->Gpu) State->ShadowResources.Destroy(State->Gpu);
		if (State->Gpu) State->MaterialSamplers.Destroy(State->Gpu);
		if (State->ColorTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->ColorTexture);
		if (State->HdrSceneTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->HdrSceneTexture);
		if (State->DepthTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->DepthTexture);
		if (State->ShadowMapTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->ShadowMapTexture);
		if (State->ShadowSampler && State->Gpu) SDL_ReleaseGPUSampler(State->Gpu, State->ShadowSampler);
		State->ColorTexture = nullptr;
		State->HdrSceneTexture = nullptr;
		State->Metrics.HdrSceneBytes = 0;
		State->DepthTexture = nullptr;
		State->ShadowMapTexture = nullptr;
		State->ShadowSampler = nullptr;
		if (State->WindowClaimed && State->Gpu && State->Window)
			SDL_ReleaseWindowFromGPUDevice(State->Gpu, State->Window);
		State->WindowClaimed = false;
		if (State->Gpu) SDL_DestroyGPUDevice(State->Gpu);
		State->Gpu = nullptr;
		if (State->Window) SDL_DestroyWindow(State->Window);
		State->Window = nullptr;
		State->Width = 0;
		State->Height = 0;
		State->SwapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
	}

	void SDLRenderer::Draw(RenderPublicationPtr Publication) {
		if (!Publication) throw std::invalid_argument("SDLRenderer requires an immutable RenderPublication");
		if (!State || !State->Gpu || (!State->Options.Offscreen && !State->Window) || !State->MeshResources ||
			!State->TextureResources || !State->SkinPalettes)
			throw std::logic_error("SDLRenderer is not initialized");
		if (State->RequiresFullResync && !Publication->FullResync)
			throw std::logic_error("SDLRenderer requires a full RenderPublication after resource recovery");
		State->Metrics.LastProjectionNanoseconds = 0;
		State->Metrics.LastVisibilityNanoseconds = 0;
		State->Metrics.LastMeshTransferNanoseconds = 0;
		State->Metrics.LastTextureTransferNanoseconds = 0;
		State->Metrics.LastUiPreparationNanoseconds = 0;
		State->Metrics.LastSubmissionNanoseconds = 0;
		State->Metrics.LastGpuCompletionWaitNanoseconds = 0;
		State->Metrics.LastShadowClassificationNanoseconds = 0;
		State->Metrics.LastProjectionValidationNanoseconds = 0;
		State->Metrics.LastProjectionMutationNanoseconds = 0;
		State->Metrics.LastMeshApplicationNanoseconds = 0;
		State->Metrics.LastPaletteApplicationNanoseconds = 0;
		State->Metrics.LastCommandAcquireNanoseconds = 0;
		State->Metrics.LastShadowRecordingNanoseconds = 0;
		State->Metrics.LastSkyRecordingNanoseconds = 0;
		State->Metrics.LastOpaqueRecordingNanoseconds = 0;
		State->Metrics.LastResolveRecordingNanoseconds = 0;
		State->Metrics.LastGuiRecordingNanoseconds = 0;
		State->Metrics.LastGpuSubmitNanoseconds = 0;
		State->Metrics.LastInstanceApplicationNanoseconds = 0;
		State->Metrics.LastInstanceUploadNanoseconds = 0;
		State->Metrics.LastInstancePreparationNanoseconds = 0;
		State->Metrics.LastInstanceTraversalNanoseconds = 0;
		State->Metrics.LastInstanceCandidateNanoseconds = 0;
		State->Metrics.LastInstanceVisibleUploadNanoseconds = 0;
		State->Metrics.LastCameraTraversalNanoseconds = 0;
		State->Metrics.LastCameraCullNanoseconds = 0;
		State->Metrics.LastCameraResidencyNanoseconds = 0;
		State->Metrics.LastOpaqueGroupingNanoseconds = 0;
		State->Metrics.LastWorldDrawRecordingNanoseconds = 0;
		State->Metrics.LastTransparentNanoseconds = 0;
		try {
			if (!Publication->FullResync) for (const auto &Update : Publication->AnimationPoseUpdates) {
				const auto *Existing = State->Projection.GetAnimationPose(Update.Object);
				if (Existing && Update.PoseRevision <= Existing->PoseRevision)
					++State->Metrics.StalePoseDrops;
			}
			const auto ProjectionStart = Clock::now();
			State->ShadowCache.ApplyPublication(State->Projection, *Publication);
			State->SpotShadowAtlas.ApplyPublication(State->Projection, *Publication);
			State->Metrics.LastShadowClassificationNanoseconds = Nanoseconds(Clock::now() - ProjectionStart);
			const auto Changes = State->Projection.Apply(*Publication);
			if (State->Options.EnableInstancing && State->InstanceResources) {
				const auto InstanceStart = Clock::now();
				(void)State->InstanceResources->Apply(*Publication);
				State->Metrics.LastInstanceApplicationNanoseconds = Nanoseconds(Clock::now() - InstanceStart);
			}
			State->Metrics.LastProjectionValidationNanoseconds = Changes.ValidationNanoseconds;
			State->Metrics.LastProjectionMutationNanoseconds = Changes.MutationNanoseconds;
			State->Metrics.EnvironmentApplications += Changes.EnvironmentsUpdated;
			State->Metrics.LastProjectionNanoseconds = Nanoseconds(Clock::now() - ProjectionStart);
			State->Metrics.CpuProjectionNanoseconds += State->Metrics.LastProjectionNanoseconds;
			const auto MeshStart = Clock::now();
			State->MeshResources->ApplyPublication(*Publication);
			State->MeshResources->UploadToGpu();
			State->Metrics.LastMeshApplicationNanoseconds = Nanoseconds(Clock::now() - MeshStart);
			const auto PaletteStart = Clock::now();
			State->SkinPalettes->ApplyPublication(*Publication);
			State->Metrics.LastPaletteApplicationNanoseconds = Nanoseconds(Clock::now() - PaletteStart);
			State->Metrics.LastMeshTransferNanoseconds = Nanoseconds(Clock::now() - MeshStart);
			State->Metrics.CpuMeshTransferNanoseconds += State->Metrics.LastMeshTransferNanoseconds;
			const auto TextureStart = Clock::now();
			State->TextureResources->ApplyPublication(*Publication);
			State->Metrics.LastTextureTransferNanoseconds = Nanoseconds(Clock::now() - TextureStart);
			State->Metrics.CpuTextureTransferNanoseconds += State->Metrics.LastTextureTransferNanoseconds;
			State->RequiresFullResync = false;
			if (Publication->FullResync) State->ShadowPoseRevisions.clear();
			for (const auto &Remove : Publication->Removes)
				State->ShadowPoseRevisions.erase(Remove.Object);
		} catch (...) {
			State->Projection.Clear();
			State->MeshResources->Destroy();
			if (State->InstanceResources) State->InstanceResources->Destroy();
			State->TextureResources->Destroy();
			State->SkinPalettes->Destroy();
			State->ShadowCache.Invalidate();
			State->SpotShadowAtlas.Invalidate();
			State->ShadowPoseRevisions.clear();
			State->VisibilityComplete = false;
			State->VisibleAnimationObjects.clear();
			State->RequiresFullResync = true;
			throw;
		}
		const auto VisibilityStart = Clock::now();
		State->VisibilityComplete = State->Projection.BuildAnimationVisibilityFeedback(
			State->VisibleAnimationObjects, 4'096
		);
		State->VisibilityPublication = State->Projection.GetLastPublicationId();
		State->Metrics.LastVisibilityNanoseconds = Nanoseconds(Clock::now() - VisibilityStart);
		State->Metrics.CpuVisibilityNanoseconds += State->Metrics.LastVisibilityNanoseconds;
		if (!State->DepthTexture || !State->ShadowMapTexture) return;

		const auto SubmissionStart = Clock::now();
		auto *Commands = SDL_AcquireGPUCommandBuffer(State->Gpu);
		State->Metrics.LastCommandAcquireNanoseconds = Nanoseconds(Clock::now() - SubmissionStart);
		if (!Commands) {
			LOG_ERROR(App, "[Render:SDL] Failed to acquire command buffer: %s", SDL_GetError());
			return;
		}

		State->Metrics.GpuSkinningRigs = State->SkinPalettes->GetGpuRigCount();
		State->Metrics.CpuFallbackRigs = 0;
		for (const auto &[Object, Pose] : State->Projection.GetAnimationPoses()) {
			(void)Object;
			if (Pose.Mode == RenderAnimationSkinningMode::CpuFallback) ++State->Metrics.CpuFallbackRigs;
		}
		SDLFrameContext Frame(
			State->Projection, *State->MeshResources, *State->SkinPalettes, State->ShadowCache, State->ShadowPoseRevisions);
		Frame.EnableOpaqueGrouping = State->Options.EnableOpaqueGrouping;
		Frame.ProfileDetailed = State->Options.ProfileDetailed;
		Frame.UseCompactTraversal = State->Options.UseCompactTraversal;
		Frame.EnableInstancing = State->Options.EnableInstancing && State->InstanceResources &&
			State->InstanceResources->GetRecords().IsAvailable();
		Frame.EnableSceneEnvelope = State->Options.EnableSceneEnvelope;
		Frame.UseContiguousInstancing = State->Options.UseContiguousInstancing;
		Frame.Commands = Commands;
		Frame.HdrSceneTexture = State->HdrSceneTexture;
		Frame.DepthTexture = State->DepthTexture;
		Frame.ShadowMapTexture = State->ShadowMapTexture;
		Frame.ShadowSampler = State->ShadowSampler;
		Frame.TextureResources = State->TextureResources.get();
		Frame.InstanceResources = State->InstanceResources.get();
		Frame.MaterialSamplers = &State->MaterialSamplers;
		Frame.Metrics = &State->Metrics;
		if (State->Options.Offscreen) {
			Frame.SwapchainTexture = State->ColorTexture;
			Frame.Width = static_cast<std::uint32_t>(State->Width);
			Frame.Height = static_cast<std::uint32_t>(State->Height);
		} else if (!SDL_AcquireGPUSwapchainTexture(Frame.Commands, State->Window, &Frame.SwapchainTexture, &Frame.Width, &Frame.Height)) {
			LOG_TRACE(App, "[Render:SDL] Failed to acquire swapchain texture: %s", SDL_GetError());
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		}
		if (!Frame.SwapchainTexture) {
			LOG_TRACE(App, "[Render:SDL] Acquired a null swapchain texture");
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		}
		if (Frame.Width != static_cast<std::uint32_t>(State->Width) ||
			Frame.Height != static_cast<std::uint32_t>(State->Height) ||
			Frame.Width != State->Projection.GetFrame().ViewportWidth ||
			Frame.Height != State->Projection.GetFrame().ViewportHeight) {
			if (State->Options.Offscreen) SDL_CancelGPUCommandBuffer(Frame.Commands);
			else if (!SDL_SubmitGPUCommandBuffer(Frame.Commands))
				LOG_ERROR(App, "[Render:SDL] Failed to retire the resized swapchain image: %s", SDL_GetError());
			LOG_TRACE(App, "[Render:SDL] RenderPublication viewport does not match the acquired target");
			return;
		}
		try {
			State->ShadowResources.PrepareFrame(State->Gpu, Frame, State->SpotShadowAtlas);
			if (Frame.EnableInstancing) {
				try {
					const auto InstanceStart = Clock::now();
					State->InstanceResources->Upload(Frame.Commands, &State->Metrics);
					State->Metrics.LastInstanceUploadNanoseconds = Nanoseconds(Clock::now() - InstanceStart);
				}
				catch (const std::exception &Error) {
					LOG_WARN(App, "[Renderer:Instances] Disabled optional instancing: %s", Error.what());
					State->InstanceResources->OnFrameAborted();
					State->Options.EnableInstancing = false;
					Frame.EnableInstancing = false;
				}
			}
			const auto &Definitions = GetSDLRenderPassConstructors();
			for (std::size_t Index = 0; Index < State->RenderPasses.size(); ++Index) {
				const auto PassStart = Clock::now();
				if (auto *Recorded = State->RenderPasses[Index]->Draw(State->Gpu, Frame)) SDL_EndGPURenderPass(Recorded);
				const auto Elapsed = Nanoseconds(Clock::now() - PassStart);
				switch (Definitions[Index].Phase) {
					case SDLPassPhase::Shadow: State->Metrics.LastShadowRecordingNanoseconds += Elapsed; break;
					case SDLPassPhase::Sky: State->Metrics.LastSkyRecordingNanoseconds += Elapsed; break;
					case SDLPassPhase::Opaque: State->Metrics.LastOpaqueRecordingNanoseconds += Elapsed; break;
					case SDLPassPhase::Resolve: State->Metrics.LastResolveRecordingNanoseconds += Elapsed; break;
					case SDLPassPhase::Gui: State->Metrics.LastGuiRecordingNanoseconds += Elapsed; break;
				}
			}
		} catch (...) {
			if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
			for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
			if (State->Options.Offscreen) SDL_CancelGPUCommandBuffer(Frame.Commands);
			else SDL_SubmitGPUCommandBuffer(Frame.Commands);
			throw;
		}
		if (State->Options.WaitForGpuCompletion) {
			const auto SubmitStart = Clock::now();
			auto *Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Frame.Commands);
			State->Metrics.LastGpuSubmitNanoseconds = Nanoseconds(Clock::now() - SubmitStart);
			State->Metrics.LastSubmissionNanoseconds = Nanoseconds(Clock::now() - SubmissionStart);
			if (!Fence) {
				if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
				for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
				throw std::runtime_error(std::format("Failed to submit SDL GPU frame: {}", SDL_GetError()));
			}
			const auto WaitStart = Clock::now();
			if (!SDL_WaitForGPUFences(State->Gpu, true, &Fence, 1)) {
				SDL_ReleaseGPUFence(State->Gpu, Fence);
				if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
				for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
				throw std::runtime_error(std::format("Failed to wait for SDL GPU frame: {}", SDL_GetError()));
			}
			if (State->InstanceResources) State->InstanceResources->OnFrameSubmitted();
			State->Metrics.LastGpuCompletionWaitNanoseconds = Nanoseconds(Clock::now() - WaitStart);
			State->Metrics.GpuCompletionWaitNanoseconds += State->Metrics.LastGpuCompletionWaitNanoseconds;
			SDL_ReleaseGPUFence(State->Gpu, Fence);
		} else {
			const auto SubmitStart = Clock::now();
			if (!SDL_SubmitGPUCommandBuffer(Frame.Commands)) {
				if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
				for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
				throw std::runtime_error(std::format("Failed to submit SDL GPU frame: {}", SDL_GetError()));
			}
			if (State->InstanceResources) State->InstanceResources->OnFrameSubmitted();
			State->Metrics.LastGpuSubmitNanoseconds = Nanoseconds(Clock::now() - SubmitStart);
			State->Metrics.LastSubmissionNanoseconds = Nanoseconds(Clock::now() - SubmissionStart);
		}
		State->Metrics.CpuSubmissionNanoseconds += State->Metrics.LastSubmissionNanoseconds;
		if (State->ShadowCache.OnFrameSubmitted()) ++State->Metrics.ShadowMapBuilds;
		State->ShadowResources.OnFrameSubmitted();
		for (auto &Pass : State->RenderPasses) Pass->OnFrameSubmitted();
		(void)State->SpotShadowAtlas.OnFrameSubmitted();
		++State->Metrics.FramesSubmitted;
		if (Publication->FullResync) ++State->Metrics.FullResyncs;
	}

	void SDLRenderer::Resize(int WidthValue, int HeightValue) {
		if (WidthValue < 1 || HeightValue < 1) return;
		if (!State || !State->Gpu || (!State->Options.Offscreen && !State->WindowClaimed))
			throw std::logic_error("Cannot resize an uninitialized SDLRenderer");
		const auto DepthInfo = GetSDLSceneDepthTargetInfo(State->Gpu,
			static_cast<std::uint32_t>(WidthValue), static_cast<std::uint32_t>(HeightValue));
		if (!State->Options.Offscreen && !SDL_SetGPUSwapchainParameters(
			State->Gpu, State->Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC
		)) throw std::runtime_error(std::format("Failed to configure renderer swapchain: {}", SDL_GetError()));
		SDL_GPUTexture *ReplacementColor = nullptr;
		if (State->Options.Offscreen) {
			SDL_GPUTextureCreateInfo ColorInfo{
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = State->SwapchainFormat,
				.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = static_cast<std::uint32_t>(WidthValue),
				.height = static_cast<std::uint32_t>(HeightValue),
				.layer_count_or_depth = 1,
				.num_levels = 1,
			};
			ReplacementColor = SDL_CreateGPUTexture(State->Gpu, &ColorInfo);
			if (!ReplacementColor)
				throw std::runtime_error(std::format("Failed to create offscreen color target: {}", SDL_GetError()));
		}

		auto *ReplacementDepth = SDL_CreateGPUTexture(State->Gpu, &DepthInfo);
		if (!ReplacementDepth) {
			if (ReplacementColor) SDL_ReleaseGPUTexture(State->Gpu, ReplacementColor);
			throw std::runtime_error(std::format("Failed to create renderer depth target: {}", SDL_GetError()));
		}
		SDL_GPUTexture *ReplacementHdr = nullptr;
		if (State->HdrSceneEnabled) {
			ReplacementHdr = CreateSDLHdrScene(State->Gpu, static_cast<std::uint32_t>(WidthValue),
				static_cast<std::uint32_t>(HeightValue));
			if (!ReplacementHdr) {
				SDL_ReleaseGPUTexture(State->Gpu, ReplacementDepth);
				if (ReplacementColor) SDL_ReleaseGPUTexture(State->Gpu, ReplacementColor);
				throw std::runtime_error(std::format("[Renderer:HDR] Could not allocate scene color: {}", SDL_GetError()));
			}
		}
		if (State->ColorTexture) SDL_ReleaseGPUTexture(State->Gpu, State->ColorTexture);
		if (State->HdrSceneTexture) SDL_ReleaseGPUTexture(State->Gpu, State->HdrSceneTexture);
		if (State->DepthTexture) SDL_ReleaseGPUTexture(State->Gpu, State->DepthTexture);
		State->ColorTexture = ReplacementColor;
		State->HdrSceneTexture = ReplacementHdr;
		State->Metrics.HdrSceneBytes = ReplacementHdr ?
			static_cast<std::uint64_t>(WidthValue) * static_cast<std::uint64_t>(HeightValue) * 8u : 0;
		State->DepthTexture = ReplacementDepth;
		State->Width = WidthValue;
		State->Height = HeightValue;
	}

	std::string SDLRenderer::GetDriverName() const {
		if (!State || !State->Gpu) return "destroyed";
		const auto *Driver = SDL_GetGPUDeviceDriver(State->Gpu);
		return Driver ? Driver : "unknown";
	}

	SDLRendererMetrics SDLRenderer::GetMetrics() const {
		if (!State) return {};
		auto Metrics = State->Metrics;
		if (State->TextureResources) {
			Metrics.MaterialViewBytes = State->TextureResources->GetMaterialViewBytes();
			Metrics.MaterialSourceBytes = State->TextureResources->GetMaterialSourceBytes();
			Metrics.MaterialViewFallbacks = State->TextureResources->GetMaterialViewFallbacks();
			Metrics.MaterialUploadStagingBytes = State->TextureResources->GetMaterialUploadStagingBytes();
			Metrics.MaterialViewEvictions = State->TextureResources->GetMaterialViewEvictions();
			Metrics.DeferredMaterialViewUploads = State->TextureResources->GetDeferredMaterialViewUploads();
			Metrics.MaterialSourceEvictions = State->TextureResources->GetMaterialSourceEvictions();
			Metrics.MaterialSourceRehydrates = State->TextureResources->GetMaterialSourceRehydrates();
		}
		return Metrics;
	}

	RenderAnimationVisibilityFeedback SDLRenderer::GetAnimationVisibilityFeedback() const {
		if (!State) return {};
		return {
			State->VisibilityGeneration,
			State->VisibilityPublication,
			State->VisibilityComplete,
			State->VisibleAnimationObjects,
		};
	}

	void SDLRenderer::WaitForIdle() {
		if (!State || !State->Gpu) return;
		if (!SDL_WaitForGPUIdle(State->Gpu))
			throw std::runtime_error(std::format("Failed to wait for SDL GPU idle: {}", SDL_GetError()));
	}

	std::pair<std::uint32_t, std::uint32_t> SDLRenderer::GetViewportSize() const {
		if (!State) return {0, 0};
		return {static_cast<std::uint32_t>(State->Width), static_cast<std::uint32_t>(State->Height)};
	}
}

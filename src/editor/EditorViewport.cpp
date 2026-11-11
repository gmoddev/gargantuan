// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <glm/geometric.hpp>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		constexpr SDL_GPUShaderFormat ShaderFormats = static_cast<SDL_GPUShaderFormat>(
			SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_MSL
		);
		constexpr SDL_GPUTextureFormat ColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

		std::optional<float> IntersectItem(const RenderItem &Item, const glm::vec3 &Origin, const glm::vec3 &Direction) {
			const auto LocalOrigin = glm::vec3(Item.InverseModelMatrix * glm::vec4(Origin, 1.0f));
			const auto LocalDirection = glm::vec3(Item.InverseModelMatrix * glm::vec4(Direction, 0.0f));
			constexpr glm::vec3 HalfSize(0.5f);
			float Minimum = 0.0f;
			float Maximum = std::numeric_limits<float>::infinity();
			for (int Axis = 0; Axis < 3; ++Axis) {
				if (std::abs(LocalDirection[Axis]) < 1e-6f) {
					if (LocalOrigin[Axis] < -HalfSize[Axis] || LocalOrigin[Axis] > HalfSize[Axis]) return std::nullopt;
					continue;
				}
				float First = (-HalfSize[Axis] - LocalOrigin[Axis]) / LocalDirection[Axis];
				float Second = (HalfSize[Axis] - LocalOrigin[Axis]) / LocalDirection[Axis];
				if (First > Second) std::swap(First, Second);
				Minimum = std::max(Minimum, First);
				Maximum = std::min(Maximum, Second);
				if (Maximum < Minimum) return std::nullopt;
			}
			return Minimum;
		}
	}

	std::optional<EditorViewportPick> PickEditorViewport(const RenderSnapshot &Snapshot, float X, float Y) {
		if (Snapshot.Id == InvalidRenderSnapshotId || Snapshot.ViewportWidth == 0 || Snapshot.ViewportHeight == 0 ||
			!std::isfinite(X) || !std::isfinite(Y) || X < 0.0f || Y < 0.0f ||
			X >= Snapshot.ViewportWidth || Y >= Snapshot.ViewportHeight) return std::nullopt;
		const auto &Camera = Snapshot.Camera;
		const float Tangent = std::tan(glm::radians(Camera.VerticalFieldOfView) * 0.5f);
		const float NormalizedX = ((X + 0.5f) / static_cast<float>(Snapshot.ViewportWidth)) * 2.0f - 1.0f;
		const float NormalizedY = 1.0f - ((Y + 0.5f) / static_cast<float>(Snapshot.ViewportHeight)) * 2.0f;
		const float Aspect = static_cast<float>(Snapshot.ViewportWidth) / static_cast<float>(Snapshot.ViewportHeight);
		const auto Direction = glm::normalize(
			Camera.LookDirection + Camera.RightDirection * (NormalizedX * Tangent * Aspect) +
			Camera.UpDirection * (NormalizedY * Tangent)
		);

		std::optional<EditorViewportPick> Closest;
		for (const auto &Item : Snapshot.Items) {
			auto Distance = IntersectItem(Item, Camera.Position, Direction);
			if (!Distance || (Closest && *Distance >= Closest->Distance)) continue;
			Closest = EditorViewportPick{Item.Object, *Distance};
		}
		return Closest;
	}

	std::optional<EditorViewportPick> PickEditorViewport(const RenderProjection &Projection, float X, float Y) {
		const auto &Frame = Projection.GetFrame();
		if (Projection.GetLastPublicationId() == InvalidRenderPublicationId || Frame.ViewportWidth == 0 ||
			Frame.ViewportHeight == 0 || !std::isfinite(X) || !std::isfinite(Y) || X < 0.0f || Y < 0.0f ||
			X >= Frame.ViewportWidth || Y >= Frame.ViewportHeight) return std::nullopt;
		const auto &Camera = Frame.Camera;
		const float Tangent = std::tan(glm::radians(Camera.VerticalFieldOfView) * 0.5f);
		const float NormalizedX = ((X + 0.5f) / static_cast<float>(Frame.ViewportWidth)) * 2.0f - 1.0f;
		const float NormalizedY = 1.0f - ((Y + 0.5f) / static_cast<float>(Frame.ViewportHeight)) * 2.0f;
		const float Aspect = static_cast<float>(Frame.ViewportWidth) / static_cast<float>(Frame.ViewportHeight);
		const auto Direction = glm::normalize(
			Camera.LookDirection + Camera.RightDirection * (NormalizedX * Tangent * Aspect) +
			Camera.UpDirection * (NormalizedY * Tangent)
		);

		std::optional<EditorViewportPick> Closest;
		for (const auto &[Object, Projected] : Projection.GetObjects()) {
			(void)Object;
			if (!Projected.Visible) continue;
			auto Distance = IntersectItem(Projected.Item, Camera.Position, Direction);
			if (!Distance || (Closest && *Distance >= Closest->Distance)) continue;
			Closest = EditorViewportPick{Projected.Item.Object, *Distance};
		}
		return Closest;
	}

	struct EditorViewportRenderer::Backend final {
		bool OwnsVideoSubsystem = false;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTexture *ColorTexture = nullptr;
		SDL_GPUTexture *HdrSceneTexture = nullptr;
		bool HdrSceneEnabled = false;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		SDL_GPUTransferBuffer *DownloadBuffer = nullptr;
		std::vector<std::unique_ptr<SDLRenderPass>> RenderPasses;
		std::unique_ptr<SDLMeshCache> MeshResources;
		std::unique_ptr<SDLInstanceGpuCache> InstanceResources;
		bool EnableInstancing = false;
		std::unique_ptr<SDLTextureCache> TextureResources;
		SDLMaterialSamplers MaterialSamplers;
		std::unique_ptr<SDLSkinPaletteCache> SkinPalettes;
		RenderProjection Projection;
		SDLDirectionalShadowCache ShadowCache;
		SDLShadowResources ShadowResources;
		SDLSpotShadowAtlas SpotShadowAtlas;
		SDLRendererMetrics Metrics;
		bool RequiresFullResync = false;
		std::uint32_t RemainingPaletteUploadFailures = 0;
		std::unordered_map<ObjectId, std::uint64_t> ShadowPoseRevisions;
		std::vector<std::uint8_t> BgraPixels;

		void InvalidatePublication() {
			RequiresFullResync = true;
			MeshResources->Destroy();
			if (InstanceResources) InstanceResources->Destroy();
			TextureResources->Destroy();
			SkinPalettes->Destroy();
			ShadowCache.Invalidate();
			SpotShadowAtlas.Invalidate();
			ShadowPoseRevisions.clear();
			Projection.Clear();
		}

		void Destroy() {
			if (Gpu) SDL_WaitForGPUIdle(Gpu);
			if (MeshResources) { MeshResources->Destroy(); MeshResources.reset(); }
			if (InstanceResources) { InstanceResources->Destroy(); InstanceResources.reset(); }
			if (TextureResources) { TextureResources->Destroy(); TextureResources.reset(); }
			if (SkinPalettes) { SkinPalettes->Destroy(); SkinPalettes.reset(); }
			for (auto &Pass : RenderPasses) if (Pass) Pass->Destroy(Gpu);
			RenderPasses.clear();
			if (Gpu) ShadowResources.Destroy(Gpu);
			if (Gpu) MaterialSamplers.Destroy(Gpu);
			if (DownloadBuffer && Gpu) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
			if (ColorTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
			if (HdrSceneTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, HdrSceneTexture);
			if (DepthTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			if (ShadowMapTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, ShadowMapTexture);
			if (ShadowSampler && Gpu) SDL_ReleaseGPUSampler(Gpu, ShadowSampler);
			DownloadBuffer = nullptr;
			ColorTexture = nullptr;
			HdrSceneTexture = nullptr;
			DepthTexture = nullptr;
			ShadowMapTexture = nullptr;
			ShadowSampler = nullptr;
			if (Gpu) SDL_DestroyGPUDevice(Gpu);
			Gpu = nullptr;
			if (OwnsVideoSubsystem) SDL_QuitSubSystem(SDL_INIT_VIDEO);
			OwnsVideoSubsystem = false;
			BgraPixels.clear();
			ShadowPoseRevisions.clear();
		}

		void RecreateTargets() {
			if (!Gpu || Width == 0 || Height == 0) throw std::invalid_argument("Viewport dimensions must be nonzero");
			const auto DownloadBytes = static_cast<std::uint64_t>(Width) * Height * 4;
			if (DownloadBytes > std::numeric_limits<std::uint32_t>::max())
				throw std::invalid_argument("Viewport dimensions exceed the GPU download buffer size limit");
			std::vector<std::uint8_t> ReplacementPixels(static_cast<std::size_t>(DownloadBytes));
			SDL_WaitForGPUIdle(Gpu);
			SDL_GPUTextureCreateInfo ColorInfo{
				.type = SDL_GPU_TEXTURETYPE_2D, .format = ColorFormat, .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
				.width = Width, .height = Height, .layer_count_or_depth = 1, .num_levels = 1,
			};
			const auto DepthInfo = GetSDLSceneDepthTargetInfo(Gpu, Width, Height);
			SDL_GPUTransferBufferCreateInfo DownloadInfo{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = static_cast<std::uint32_t>(DownloadBytes),
			};
			auto *ReplacementColor = SDL_CreateGPUTexture(Gpu, &ColorInfo);
			auto *ReplacementDepth = SDL_CreateGPUTexture(Gpu, &DepthInfo);
			auto *ReplacementHdr = HdrSceneEnabled ? CreateSDLHdrScene(Gpu, Width, Height) : nullptr;
			auto *ReplacementDownload = SDL_CreateGPUTransferBuffer(Gpu, &DownloadInfo);
			if (!ReplacementColor || !ReplacementDepth || !ReplacementDownload || (HdrSceneEnabled && !ReplacementHdr)) {
				if (ReplacementDownload) SDL_ReleaseGPUTransferBuffer(Gpu, ReplacementDownload);
				if (ReplacementColor) SDL_ReleaseGPUTexture(Gpu, ReplacementColor);
				if (ReplacementDepth) SDL_ReleaseGPUTexture(Gpu, ReplacementDepth);
				if (ReplacementHdr) SDL_ReleaseGPUTexture(Gpu, ReplacementHdr);
				throw std::runtime_error(std::format("Failed to create viewport targets: {}", SDL_GetError()));
			}
			if (DownloadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
			if (ColorTexture) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
			if (HdrSceneTexture) SDL_ReleaseGPUTexture(Gpu, HdrSceneTexture);
			if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			ColorTexture = ReplacementColor;
			HdrSceneTexture = ReplacementHdr;
			DepthTexture = ReplacementDepth;
			DownloadBuffer = ReplacementDownload;
			BgraPixels = std::move(ReplacementPixels);
		}
	};

	EditorViewportRenderer::EditorViewportRenderer(std::uint32_t Width, std::uint32_t Height,
		bool EnableHdrScene, bool EnableInstancing)
		: State(std::make_unique<Backend>()) {
		State->Width = Width;
		State->Height = Height;
		// Each offscreen renderer holds its own SDL subsystem reference. A preview
		// must survive recreation/destruction of the authoring viewport, and vice versa.
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
			throw std::runtime_error(std::format("Failed to initialize SDL video: {}", SDL_GetError()));
		State->OwnsVideoSubsystem = true;
		State->Gpu = SDL_CreateGPUDevice(ShaderFormats, true, nullptr);
		if (!State->Gpu) {
			State->Destroy();
			throw std::runtime_error(std::format("Failed to create viewport GPU device: {}", SDL_GetError()));
		}
		State->HdrSceneEnabled = EnableHdrScene && SupportsSDLHdrScene(State->Gpu);
		State->EnableInstancing = EnableInstancing;
		if (EnableInstancing) {
			const auto SceneFormat = State->HdrSceneEnabled ? SDLHdrSceneFormat : ColorFormat;
			const float OutputMode = SceneFormat == SDLHdrSceneFormat ? 2.0f :
				SDLTargetEncodesSrgb(SceneFormat) ? 0.0f : 1.0f;
			State->InstanceResources = std::make_unique<SDLInstanceGpuCache>(State->Gpu, OutputMode);
		}
		State->MeshResources = std::make_unique<SDLMeshCache>(State->Gpu);
		State->TextureResources = std::make_unique<SDLTextureCache>(State->Gpu);
		State->SkinPalettes = std::make_unique<SDLSkinPaletteCache>(State->Gpu, nullptr, &State->RemainingPaletteUploadFailures);

		SDL_GPUTextureCreateInfo ShadowInfo{
			.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = 2048, .height = 2048, .layer_count_or_depth = 1, .num_levels = 1,
		};
		State->ShadowMapTexture = SDL_CreateGPUTexture(State->Gpu, &ShadowInfo);
		SDL_GPUSamplerCreateInfo SamplerInfo{
			.min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL, .enable_compare = true,
		};
		State->ShadowSampler = SDL_CreateGPUSampler(State->Gpu, &SamplerInfo);
		if (!State->ShadowMapTexture || !State->ShadowSampler) {
			State->Destroy();
			throw std::runtime_error(std::format("Failed to create viewport shadow resources: {}", SDL_GetError()));
		}
		try {
			State->RecreateTargets();
			for (const auto &Definition : GetSDLRenderPassConstructors()) {
				if (Definition.Target == SDLPassTarget::Resolve && !State->HdrSceneEnabled) continue;
				const auto Format = State->HdrSceneEnabled && Definition.Target == SDLPassTarget::Scene
					? SDLHdrSceneFormat : ColorFormat;
				State->RenderPasses.push_back(Definition.Construct(State->Gpu, Format, nullptr));
			}
		} catch (...) {
			State->Destroy();
			throw;
		}
	}

	EditorViewportRenderer::~EditorViewportRenderer() { if (State) State->Destroy(); }

	bool EditorViewportRenderer::UsesHdrScene() const { return State && State->HdrSceneEnabled; }

	void EditorViewportRenderer::Resize(std::uint32_t Width, std::uint32_t Height) {
		if (!State || (Width == State->Width && Height == State->Height)) return;
		const auto PreviousWidth = State->Width;
		const auto PreviousHeight = State->Height;
		State->Width = Width;
		State->Height = Height;
		try { State->RecreateTargets(); }
		catch (...) { State->Width = PreviousWidth; State->Height = PreviousHeight; throw; }
		State->Projection.Clear();
	}

	void EditorViewportRenderer::ApplyPublication(RenderPublicationPtr Publication) {
		if (!State || !State->Gpu) throw std::logic_error("Viewport renderer is not initialized");
		if (!Publication) throw std::invalid_argument("Viewport requires an immutable RenderPublication");
		if (State->RequiresFullResync && !Publication->FullResync)
			throw std::logic_error("Viewport renderer requires a full RenderPublication after resource recovery");
		if (Publication->Id != InvalidRenderPublicationId && State->Projection.GetLastPublicationId() == Publication->Id) return;
		bool ApplyingNativeResources = false;
		try {
			State->ShadowCache.ApplyPublication(State->Projection, *Publication);
			State->SpotShadowAtlas.ApplyPublication(State->Projection, *Publication);
			(void)State->Projection.Apply(*Publication);
			if (State->EnableInstancing && State->InstanceResources)
				(void)State->InstanceResources->Apply(*Publication);
			ApplyingNativeResources = true;
			State->MeshResources->ApplyPublication(*Publication);
			State->TextureResources->ApplyPublication(*Publication);
			State->SkinPalettes->ApplyPublication(*Publication);
		} catch (const std::invalid_argument &) {
			// Neutral validation rejects before changing the last accepted projection.
			if (ApplyingNativeResources) State->InvalidatePublication();
			throw;
		} catch (...) {
			State->InvalidatePublication();
			throw;
		}
		State->RequiresFullResync = false;
		if (Publication->FullResync) State->ShadowPoseRevisions.clear();
		for (const auto &Remove : Publication->Removes)
			State->ShadowPoseRevisions.erase(Remove.Object);
	}

	EditorViewportCaptureView EditorViewportRenderer::CaptureBgra(RenderPublicationPtr Publication) {
		const auto TotalStart = std::chrono::steady_clock::now();
		ApplyPublication(std::move(Publication));
		const auto &FrameState = State->Projection.GetFrame();
		if (FrameState.ViewportWidth != State->Width || FrameState.ViewportHeight != State->Height)
			throw std::invalid_argument("RenderPublication dimensions do not match the viewport target");
		try {
			State->MeshResources->UploadToGpu();
		} catch (...) {
			State->InvalidatePublication();
			throw;
		}
		auto *Commands = SDL_AcquireGPUCommandBuffer(State->Gpu);
		if (!Commands) throw std::runtime_error(std::format("Failed to acquire viewport command buffer: {}", SDL_GetError()));
		SDLFrameContext Frame(
			State->Projection, *State->MeshResources, *State->SkinPalettes, State->ShadowCache, State->ShadowPoseRevisions);
		Frame.Commands = Commands;
		Frame.Metrics = &State->Metrics;
		Frame.SwapchainTexture = State->ColorTexture;
		Frame.HdrSceneTexture = State->HdrSceneTexture;
		Frame.DepthTexture = State->DepthTexture;
		Frame.ShadowMapTexture = State->ShadowMapTexture;
		Frame.ShadowSampler = State->ShadowSampler;
		Frame.TextureResources = State->TextureResources.get();
		Frame.InstanceResources = State->InstanceResources.get();
		Frame.EnableInstancing = State->EnableInstancing && State->InstanceResources &&
			State->InstanceResources->GetRecords().IsAvailable();
		Frame.MaterialSamplers = &State->MaterialSamplers;
		Frame.Width = State->Width;
		Frame.Height = State->Height;
		try {
			State->ShadowResources.PrepareFrame(State->Gpu, Frame, State->SpotShadowAtlas);
			if (Frame.EnableInstancing) {
				try { State->InstanceResources->Upload(Commands, nullptr); }
				catch (const std::exception &Error) {
					LOG_WARN(App, "[Renderer:Instances] Disabled optional viewport instancing: %s", Error.what());
					State->InstanceResources->OnFrameAborted();
					State->EnableInstancing = false;
					Frame.EnableInstancing = false;
				}
			}
			for (auto &Pass : State->RenderPasses)
				if (auto *Recorded = Pass->Draw(State->Gpu, Frame)) SDL_EndGPURenderPass(Recorded);
		} catch (...) {
			if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
			for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
			SDL_CancelGPUCommandBuffer(Commands);
			throw;
		}
		const auto RenderSubmitted = std::chrono::steady_clock::now();

		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		if (!CopyPass) {
			if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
			for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
			SDL_CancelGPUCommandBuffer(Commands);
			throw std::runtime_error(std::format("Failed to begin viewport download: {}", SDL_GetError()));
		}
		SDL_GPUTextureRegion Source{.texture = State->ColorTexture, .w = State->Width, .h = State->Height, .d = 1};
		SDL_GPUTextureTransferInfo Destination{
			.transfer_buffer = State->DownloadBuffer, .pixels_per_row = State->Width, .rows_per_layer = State->Height,
		};
		SDL_DownloadFromGPUTexture(CopyPass, &Source, &Destination);
		SDL_EndGPUCopyPass(CopyPass);
		auto *Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands);
		if (!Fence) {
			if (State->InstanceResources) State->InstanceResources->OnFrameAborted();
			for (auto &Pass : State->RenderPasses) Pass->OnFrameAborted();
			throw std::runtime_error(std::format("Failed to submit viewport frame: {}", SDL_GetError()));
		}
		if (State->InstanceResources) State->InstanceResources->OnFrameSubmitted();
		if (!SDL_WaitForGPUFences(State->Gpu, true, &Fence, 1)) {
			SDL_ReleaseGPUFence(State->Gpu, Fence);
			throw std::runtime_error(std::format("Failed to wait for viewport frame: {}", SDL_GetError()));
		}
		const auto ReadbackComplete = std::chrono::steady_clock::now();
		(void)State->ShadowCache.OnFrameSubmitted();
		State->ShadowResources.OnFrameSubmitted();
		for (auto &Pass : State->RenderPasses) Pass->OnFrameSubmitted();
		(void)State->SpotShadowAtlas.OnFrameSubmitted();
		auto *Rgba = static_cast<const std::uint8_t *>(SDL_MapGPUTransferBuffer(State->Gpu, State->DownloadBuffer, false));
		if (!Rgba) {
			SDL_ReleaseGPUFence(State->Gpu, Fence);
			throw std::runtime_error(std::format("Failed to map viewport frame: {}", SDL_GetError()));
		}
		for (std::size_t Index = 0; Index < State->BgraPixels.size(); Index += 4) {
			State->BgraPixels[Index] = Rgba[Index + 2];
			State->BgraPixels[Index + 1] = Rgba[Index + 1];
			State->BgraPixels[Index + 2] = Rgba[Index];
			State->BgraPixels[Index + 3] = Rgba[Index + 3];
		}
		SDL_UnmapGPUTransferBuffer(State->Gpu, State->DownloadBuffer);
		SDL_ReleaseGPUFence(State->Gpu, Fence);
		const auto Extracted = std::chrono::steady_clock::now();
		return {
			.Width = State->Width,
			.Height = State->Height,
			.BgraPixels = State->BgraPixels,
			.RenderSubmission = std::chrono::duration_cast<std::chrono::microseconds>(RenderSubmitted - TotalStart),
			.GpuReadbackWait = std::chrono::duration_cast<std::chrono::microseconds>(ReadbackComplete - RenderSubmitted),
			.CpuExtraction = std::chrono::duration_cast<std::chrono::microseconds>(Extracted - ReadbackComplete),
			.Total = std::chrono::duration_cast<std::chrono::microseconds>(Extracted - TotalStart),
		};
	}

	EditorViewportFrame EditorViewportRenderer::Capture(RenderPublicationPtr Publication) {
		const auto Captured = CaptureBgra(std::move(Publication));
		EditorViewportFrame Result{.Width = Captured.Width, .Height = Captured.Height};
		Result.RgbPixels.resize(static_cast<std::size_t>(Captured.Width) * Captured.Height * 3);
		for (std::size_t SourceIndex = 0, DestinationIndex = 0;
			SourceIndex < Captured.BgraPixels.size(); SourceIndex += 4, DestinationIndex += 3) {
			Result.RgbPixels[DestinationIndex] = Captured.BgraPixels[SourceIndex + 2];
			Result.RgbPixels[DestinationIndex + 1] = Captured.BgraPixels[SourceIndex + 1];
			Result.RgbPixels[DestinationIndex + 2] = Captured.BgraPixels[SourceIndex];
		}
		return Result;
	}

	std::optional<EditorViewportPick> EditorViewportRenderer::Pick(float X, float Y) const {
		if (!State || State->RequiresFullResync) return std::nullopt;
		return PickEditorViewport(State->Projection, X, Y);
	}

	void EditorViewportRenderer::InjectPaletteUploadFailureForTesting() {
		State->RemainingPaletteUploadFailures = 1;
	}
}

// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"

#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLRenderPass.hpp"

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

	struct EditorViewportRenderer::Backend final {
		bool OwnsVideoSubsystem = false;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTexture *ColorTexture = nullptr;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		SDL_GPUTransferBuffer *DownloadBuffer = nullptr;
		std::vector<std::unique_ptr<SDLRenderPass>> RenderPasses;
		std::unique_ptr<SDLMeshCache> MeshResources;

		void Destroy() {
			if (Gpu) SDL_WaitForGPUIdle(Gpu);
			if (MeshResources) { MeshResources->Destroy(); MeshResources.reset(); }
			for (auto &Pass : RenderPasses) if (Pass) Pass->Destroy(Gpu);
			RenderPasses.clear();
			if (DownloadBuffer && Gpu) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
			if (ColorTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
			if (DepthTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			if (ShadowMapTexture && Gpu) SDL_ReleaseGPUTexture(Gpu, ShadowMapTexture);
			if (ShadowSampler && Gpu) SDL_ReleaseGPUSampler(Gpu, ShadowSampler);
			DownloadBuffer = nullptr;
			ColorTexture = nullptr;
			DepthTexture = nullptr;
			ShadowMapTexture = nullptr;
			ShadowSampler = nullptr;
			if (Gpu) SDL_DestroyGPUDevice(Gpu);
			Gpu = nullptr;
			if (OwnsVideoSubsystem) SDL_QuitSubSystem(SDL_INIT_VIDEO);
			OwnsVideoSubsystem = false;
		}

		void RecreateTargets() {
			if (!Gpu || Width == 0 || Height == 0) throw std::invalid_argument("Viewport dimensions must be nonzero");
			const auto DownloadBytes = static_cast<std::uint64_t>(Width) * Height * 4;
			if (DownloadBytes > std::numeric_limits<std::uint32_t>::max())
				throw std::invalid_argument("Viewport dimensions exceed the GPU download buffer size limit");
			SDL_WaitForGPUIdle(Gpu);
			SDL_GPUTextureCreateInfo ColorInfo{
				.type = SDL_GPU_TEXTURETYPE_2D, .format = ColorFormat, .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
				.width = Width, .height = Height, .layer_count_or_depth = 1, .num_levels = 1,
			};
			SDL_GPUTextureCreateInfo DepthInfo{
				.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_D16_UNORM,
				.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, .width = Width, .height = Height,
				.layer_count_or_depth = 1, .num_levels = 1,
			};
			SDL_GPUTransferBufferCreateInfo DownloadInfo{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = static_cast<std::uint32_t>(DownloadBytes),
			};
			auto *ReplacementColor = SDL_CreateGPUTexture(Gpu, &ColorInfo);
			auto *ReplacementDepth = SDL_CreateGPUTexture(Gpu, &DepthInfo);
			auto *ReplacementDownload = SDL_CreateGPUTransferBuffer(Gpu, &DownloadInfo);
			if (!ReplacementColor || !ReplacementDepth || !ReplacementDownload) {
				if (ReplacementDownload) SDL_ReleaseGPUTransferBuffer(Gpu, ReplacementDownload);
				if (ReplacementColor) SDL_ReleaseGPUTexture(Gpu, ReplacementColor);
				if (ReplacementDepth) SDL_ReleaseGPUTexture(Gpu, ReplacementDepth);
				throw std::runtime_error(std::format("Failed to create viewport targets: {}", SDL_GetError()));
			}
			if (DownloadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
			if (ColorTexture) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
			if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			ColorTexture = ReplacementColor;
			DepthTexture = ReplacementDepth;
			DownloadBuffer = ReplacementDownload;
		}
	};

	EditorViewportRenderer::EditorViewportRenderer(std::uint32_t Width, std::uint32_t Height)
		: State(std::make_unique<Backend>()) {
		State->Width = Width;
		State->Height = Height;
		if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
				throw std::runtime_error(std::format("Failed to initialize SDL video: {}", SDL_GetError()));
			State->OwnsVideoSubsystem = true;
		}
		State->Gpu = SDL_CreateGPUDevice(ShaderFormats, true, nullptr);
		if (!State->Gpu) {
			State->Destroy();
			throw std::runtime_error(std::format("Failed to create viewport GPU device: {}", SDL_GetError()));
		}
		State->MeshResources = std::make_unique<SDLMeshCache>(State->Gpu);

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
			for (const auto &Constructor : GetSDLRenderPassConstructors())
				State->RenderPasses.push_back(Constructor(State->Gpu, ColorFormat));
		} catch (...) {
			State->Destroy();
			throw;
		}
	}

	EditorViewportRenderer::~EditorViewportRenderer() { if (State) State->Destroy(); }

	void EditorViewportRenderer::Resize(std::uint32_t Width, std::uint32_t Height) {
		if (!State || (Width == State->Width && Height == State->Height)) return;
		const auto PreviousWidth = State->Width;
		const auto PreviousHeight = State->Height;
		State->Width = Width;
		State->Height = Height;
		try { State->RecreateTargets(); }
		catch (...) { State->Width = PreviousWidth; State->Height = PreviousHeight; throw; }
	}

	EditorViewportFrame EditorViewportRenderer::Capture(RenderSnapshotPtr Snapshot) {
		if (!State || !State->Gpu) throw std::logic_error("Viewport renderer is not initialized");
		if (!Snapshot) throw std::invalid_argument("Viewport capture requires an immutable RenderSnapshot");
		if (Snapshot->ViewportWidth != State->Width || Snapshot->ViewportHeight != State->Height)
			throw std::invalid_argument("RenderSnapshot dimensions do not match the viewport target");
		State->MeshResources->UploadToGpu();
		auto *Commands = SDL_AcquireGPUCommandBuffer(State->Gpu);
		if (!Commands) throw std::runtime_error(std::format("Failed to acquire viewport command buffer: {}", SDL_GetError()));
		SDLFrameContext Frame(*Snapshot, *State->MeshResources);
		Frame.Commands = Commands;
		Frame.SwapchainTexture = State->ColorTexture;
		Frame.DepthTexture = State->DepthTexture;
		Frame.ShadowMapTexture = State->ShadowMapTexture;
		Frame.ShadowSampler = State->ShadowSampler;
		Frame.Width = State->Width;
		Frame.Height = State->Height;
		for (auto &Pass : State->RenderPasses) SDL_EndGPURenderPass(Pass->Draw(State->Gpu, Frame));

		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		if (!CopyPass) {
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
		if (!Fence) throw std::runtime_error(std::format("Failed to submit viewport frame: {}", SDL_GetError()));
		if (!SDL_WaitForGPUFences(State->Gpu, true, &Fence, 1)) {
			SDL_ReleaseGPUFence(State->Gpu, Fence);
			throw std::runtime_error(std::format("Failed to wait for viewport frame: {}", SDL_GetError()));
		}
		auto *Rgba = static_cast<const std::uint8_t *>(SDL_MapGPUTransferBuffer(State->Gpu, State->DownloadBuffer, false));
		if (!Rgba) {
			SDL_ReleaseGPUFence(State->Gpu, Fence);
			throw std::runtime_error(std::format("Failed to map viewport frame: {}", SDL_GetError()));
		}
		EditorViewportFrame Result{.Width = State->Width, .Height = State->Height};
		Result.RgbPixels.resize(static_cast<std::size_t>(State->Width) * State->Height * 3);
		for (std::size_t SourceIndex = 0, DestinationIndex = 0;
			SourceIndex < static_cast<std::size_t>(State->Width) * State->Height * 4;
			SourceIndex += 4, DestinationIndex += 3)
			std::memcpy(Result.RgbPixels.data() + DestinationIndex, Rgba + SourceIndex, 3);
		SDL_UnmapGPUTransferBuffer(State->Gpu, State->DownloadBuffer);
		SDL_ReleaseGPUFence(State->Gpu, Fence);
		return Result;
	}
}

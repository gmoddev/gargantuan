// #define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"

#include "gargantuan/Log.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLRenderPass.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gargantuan {
	struct SDLRenderer::Backend final {
		int Width = 0;
		int Height = 0;
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTextureFormat SwapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		bool WindowClaimed = false;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		std::vector<std::unique_ptr<SDLRenderPass>> RenderPasses;
		std::unique_ptr<SDLMeshCache> MeshResources;
	};

	const std::vector<SDLRenderPassConstructor> &GetSDLRenderPassConstructors() {
		static const std::vector<SDLRenderPassConstructor> Constructors{
			CreateShadowPass,
			CreateOpaquePass,
		};
		return Constructors;
	}

	SDLRenderer::SDLRenderer(const Vector2 &ViewportSize)
		: BaseRenderer(ViewportSize), State(std::make_unique<Backend>()) {
		static constexpr auto WindowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		static constexpr auto ShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_METALLIB |
			SDL_GPU_SHADERFORMAT_MSL;

		State->Gpu = SDL_CreateGPUDevice(ShaderFormats, true, nullptr);
		if (!State->Gpu) throw std::runtime_error(std::format("Failed to create GPU device: {}", SDL_GetError()));

		try {
			State->Window = SDL_CreateWindow("Gargantuan", ViewportSize.GetX(), ViewportSize.GetY(), WindowFlags);
			if (!State->Window) throw std::runtime_error(std::format("Failed to create window: {}", SDL_GetError()));
			if (!SDL_ClaimWindowForGPUDevice(State->Gpu, State->Window))
				throw std::runtime_error(std::format("Failed to claim window for GPU: {}", SDL_GetError()));
			State->WindowClaimed = true;
			State->SwapchainFormat = SDL_GetGPUSwapchainTextureFormat(State->Gpu, State->Window);
			State->MeshResources = std::make_unique<SDLMeshCache>(State->Gpu);

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

			int InitialWidth = 0;
			int InitialHeight = 0;
			if (!SDL_GetWindowSizeInPixels(State->Window, &InitialWidth, &InitialHeight))
				throw std::runtime_error(std::format("Failed to query renderer window size: {}", SDL_GetError()));
			Resize(InitialWidth, InitialHeight);

			for (const auto &Constructor : GetSDLRenderPassConstructors()) {
				auto Pass = Constructor(State->Gpu, State->SwapchainFormat);
				if (!Pass) throw std::runtime_error("Failed to construct renderer pass");
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
		if (State->Gpu) SDL_WaitForGPUIdle(State->Gpu);
		if (State->MeshResources) {
			State->MeshResources->Destroy();
			State->MeshResources.reset();
		}
		for (auto &Pass : State->RenderPasses) if (Pass) Pass->Destroy(State->Gpu);
		State->RenderPasses.clear();
		if (State->DepthTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->DepthTexture);
		if (State->ShadowMapTexture && State->Gpu) SDL_ReleaseGPUTexture(State->Gpu, State->ShadowMapTexture);
		if (State->ShadowSampler && State->Gpu) SDL_ReleaseGPUSampler(State->Gpu, State->ShadowSampler);
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

	void SDLRenderer::Draw(RenderSnapshotPtr Snapshot) {
		if (!Snapshot) throw std::invalid_argument("SDLRenderer requires an immutable RenderSnapshot");
		if (!State || !State->Gpu || !State->Window || !State->MeshResources)
			throw std::logic_error("SDLRenderer is not initialized");
		State->MeshResources->UploadToGpu();
		if (!State->DepthTexture || !State->ShadowMapTexture) return;

		auto *Commands = SDL_AcquireGPUCommandBuffer(State->Gpu);
		if (!Commands) {
			LOG_ERROR(App, "[Render:SDL] Failed to acquire command buffer: %s", SDL_GetError());
			return;
		}

		SDLFrameContext Frame(*Snapshot, *State->MeshResources);
		Frame.Commands = Commands;
		Frame.DepthTexture = State->DepthTexture;
		Frame.ShadowMapTexture = State->ShadowMapTexture;
		Frame.ShadowSampler = State->ShadowSampler;
		if (!SDL_AcquireGPUSwapchainTexture(Frame.Commands, State->Window, &Frame.SwapchainTexture, &Frame.Width, &Frame.Height)) {
			LOG_TRACE(App, "[Render:SDL] Failed to acquire swapchain texture: %s", SDL_GetError());
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		}
		if (!Frame.SwapchainTexture) {
			LOG_TRACE(App, "[Render:SDL] Acquired a null swapchain texture");
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		}
		if (Frame.Width != Snapshot->ViewportWidth || Frame.Height != Snapshot->ViewportHeight) {
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			LOG_TRACE(App, "[Render:SDL] RenderSnapshot viewport does not match the acquired target");
			return;
		}
		for (auto &Pass : State->RenderPasses) SDL_EndGPURenderPass(Pass->Draw(State->Gpu, Frame));
		SDL_SubmitGPUCommandBuffer(Frame.Commands);
	}

	void SDLRenderer::Resize(int WidthValue, int HeightValue) {
		if (WidthValue < 1 || HeightValue < 1) return;
		if (!State || !State->Gpu || !State->WindowClaimed)
			throw std::logic_error("Cannot resize an uninitialized SDLRenderer");
		if (!SDL_SetGPUSwapchainParameters(
			State->Gpu, State->Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC
		)) throw std::runtime_error(std::format("Failed to configure renderer swapchain: {}", SDL_GetError()));

		SDL_GPUTextureCreateInfo DepthInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
			.width = static_cast<std::uint32_t>(WidthValue),
			.height = static_cast<std::uint32_t>(HeightValue),
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		auto *ReplacementDepth = SDL_CreateGPUTexture(State->Gpu, &DepthInfo);
		if (!ReplacementDepth)
			throw std::runtime_error(std::format("Failed to create renderer depth target: {}", SDL_GetError()));
		if (State->DepthTexture) SDL_ReleaseGPUTexture(State->Gpu, State->DepthTexture);
		State->DepthTexture = ReplacementDepth;
		State->Width = WidthValue;
		State->Height = HeightValue;
	}

	std::string SDLRenderer::GetDriverName() const {
		if (!State || !State->Gpu) return "destroyed";
		const auto *Driver = SDL_GetGPUDeviceDriver(State->Gpu);
		return Driver ? Driver : "unknown";
	}

	std::pair<std::uint32_t, std::uint32_t> SDLRenderer::GetViewportSize() const {
		if (!State) return {0, 0};
		return {static_cast<std::uint32_t>(State->Width), static_cast<std::uint32_t>(State->Height)};
	}
}

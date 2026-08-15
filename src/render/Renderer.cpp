// #define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/render/MeshProvider.hpp"
#include "gargantuan/render/RenderPass.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>
#include <format>
#include <glm/geometric.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gargantuan {
	static constexpr auto WINDOW_FLAGS = SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	static constexpr auto SHADER_FORMATS = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_METALLIB |
										   SDL_GPU_SHADERFORMAT_MSL;

	const std::vector<RenderPassConstructor> RENDER_PASS_CONSTRUCTORS{
		CreateShadowPass,
		CreateOpaquePass,
		// CreateGuiPass,
	};

	SDLRenderer::SDLRenderer(const Vector2 &ViewportSize) : BaseRenderer(ViewportSize) {
		Gpu = SDL_CreateGPUDevice(SHADER_FORMATS, true, nullptr);
		if (!Gpu) throw std::runtime_error(std::format("Failed to create GPU device: {}", SDL_GetError()));

		try {
			Window = SDL_CreateWindow("Gargantuan", ViewportSize.GetX(), ViewportSize.GetY(), WINDOW_FLAGS);
			if (!Window) throw std::runtime_error(std::format("Failed to create window: {}", SDL_GetError()));

			if (!SDL_ClaimWindowForGPUDevice(Gpu, Window))
				throw std::runtime_error(std::format("Failed to claim window for GPU: {}", SDL_GetError()));
			WindowClaimed = true;

			SwapchainFormat = SDL_GetGPUSwapchainTextureFormat(Gpu, Window);
			MeshResources = std::make_unique<GpuMeshCache>(Gpu);

			SDL_GPUTextureCreateInfo ShadowMapInfo{
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = 2048,
				.height = 2048,
				.layer_count_or_depth = 1,
				.num_levels = 1,
			};
			ShadowMapTexture = SDL_CreateGPUTexture(Gpu, &ShadowMapInfo);

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
			ShadowSampler = SDL_CreateGPUSampler(Gpu, &SamplerInfo);
			if (!ShadowMapTexture || !ShadowSampler)
				throw std::runtime_error(std::format("Failed to create shadow resources: {}", SDL_GetError()));

			int InitialWidth = 0;
			int InitialHeight = 0;
			if (!SDL_GetWindowSizeInPixels(Window, &InitialWidth, &InitialHeight))
				throw std::runtime_error(std::format("Failed to query renderer window size: {}", SDL_GetError()));
			Resize(InitialWidth, InitialHeight);

			for (const auto &Constructor : RENDER_PASS_CONSTRUCTORS) {
				auto Pass = Constructor(Gpu, SwapchainFormat);
				if (!Pass) throw std::runtime_error("Failed to construct renderer pass");
				RenderPasses.push_back(std::move(Pass));
			}
		} catch (...) {
			Destroy();
			throw;
		}
	}

	SDLRenderer::~SDLRenderer() { Destroy(); }

	void SDLRenderer::Destroy() {
		if (Gpu) SDL_WaitForGPUIdle(Gpu);

		if (MeshResources) {
			MeshResources->Destroy();
			MeshResources.reset();
		}
		for (auto &Pass : RenderPasses) {
			if (Pass) Pass->Destroy(Gpu);
		}
		RenderPasses.clear();

		if (DepthTexture && Gpu) {
			SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			DepthTexture = nullptr;
		}

		if (ShadowMapTexture && Gpu) {
			SDL_ReleaseGPUTexture(Gpu, ShadowMapTexture);
			ShadowMapTexture = nullptr;
		}

		if (ShadowSampler && Gpu) {
			SDL_ReleaseGPUSampler(Gpu, ShadowSampler);
			ShadowSampler = nullptr;
		}

		if (WindowClaimed && Gpu && Window) {
			SDL_ReleaseWindowFromGPUDevice(Gpu, Window);
			WindowClaimed = false;
		}
		if (Gpu) SDL_DestroyGPUDevice(Gpu);
		Gpu = nullptr;
		if (Window) SDL_DestroyWindow(Window);
		Window = nullptr;
		Width = 0;
		Height = 0;
		SwapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
	}

	void SDLRenderer::Draw(RenderSnapshotPtr Snapshot) {
		if (!Snapshot) throw std::invalid_argument("SDLRenderer requires an immutable RenderSnapshot");
		if (!Gpu || !Window || !MeshResources) throw std::logic_error("SDLRenderer is not initialized");
		MeshResources->UploadToGpu();

		if (!DepthTexture || !ShadowMapTexture) return;

		SDL_GPUCommandBuffer *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) {
			SDL_Log("Failed to acquire command buffer: %s", SDL_GetError());
			return;
		}

		FrameContext Frame(*Snapshot, *MeshResources);
		Frame.Commands = Commands;

		Frame.DepthTexture = DepthTexture;
		Frame.ShadowMapTexture = ShadowMapTexture;
		Frame.ShadowSampler = ShadowSampler;

		auto SwapchainResult = SDL_AcquireGPUSwapchainTexture(
			Frame.Commands, Window, &Frame.SwapchainTexture, &Frame.Width, &Frame.Height
		);
		if (!SwapchainResult) {
			LOG_TRACE(App, "Failed to acquire swapchain texture: %s", SDL_GetError());
			if (Frame.Commands) SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		} else if (!Frame.SwapchainTexture) {
			LOG_TRACE(App, "Acquired swapchain texture, but it is null");
			if (Frame.Commands) SDL_CancelGPUCommandBuffer(Frame.Commands);
			return;
		}
		if (Frame.Width != Snapshot->ViewportWidth || Frame.Height != Snapshot->ViewportHeight) {
			SDL_CancelGPUCommandBuffer(Frame.Commands);
			LOG_TRACE(App, "RenderSnapshot viewport does not match the acquired swapchain target");
			return;
		}

		for (auto &Pass : RenderPasses) {
			SDL_EndGPURenderPass(Pass->Draw(Gpu, Frame));
		}

		SDL_SubmitGPUCommandBuffer(Frame.Commands);
	}

	void SDLRenderer::Resize(int WidthValue, int HeightValue) {
		if (WidthValue < 1 || HeightValue < 1) return;
		if (!Gpu || !WindowClaimed) throw std::logic_error("Cannot resize an uninitialized SDLRenderer");

		// SDL_SetGPUSwapchainParameters(Gpu, Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE);
		if (!SDL_SetGPUSwapchainParameters(Gpu, Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC))
			throw std::runtime_error(std::format("Failed to configure renderer swapchain: {}", SDL_GetError()));

		SDL_GPUTextureCreateInfo DepthInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
			.width = static_cast<std::uint32_t>(WidthValue),
			.height = static_cast<std::uint32_t>(HeightValue),
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		auto *ReplacementDepth = SDL_CreateGPUTexture(Gpu, &DepthInfo);
		if (!ReplacementDepth)
			throw std::runtime_error(std::format("Failed to create renderer depth target: {}", SDL_GetError()));
		if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
		DepthTexture = ReplacementDepth;
		Width = WidthValue;
		Height = HeightValue;
	}

	std::string SDLRenderer::GetDriverName() const {
		if (!Gpu) return "destroyed";
		const auto *Driver = SDL_GetGPUDeviceDriver(Gpu);
		return Driver ? Driver : "unknown";
	}
}

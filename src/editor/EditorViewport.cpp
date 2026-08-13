// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/render/MeshProvider.hpp"
#include "gargantuan/render/Renderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <cmath>
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

		std::optional<float> IntersectPart(
			const BasePart &part,
			const glm::vec3 &origin,
			const glm::vec3 &direction
		) {
			const auto frame = part.GetCFrame();
			const auto inverseRotation = glm::transpose(frame.Rotation);
			const auto localOrigin = inverseRotation * (origin - frame.Position);
			const auto localDirection = inverseRotation * direction;
			const auto halfSize = glm::abs(part.GetSize()) * 0.5f;
			float minimum = 0.0f;
			float maximum = std::numeric_limits<float>::infinity();
			for (int axis = 0; axis < 3; ++axis) {
				if (std::abs(localDirection[axis]) < 1e-6f) {
					if (localOrigin[axis] < -halfSize[axis] || localOrigin[axis] > halfSize[axis])
						return std::nullopt;
					continue;
				}
				float first = (-halfSize[axis] - localOrigin[axis]) / localDirection[axis];
				float second = (halfSize[axis] - localOrigin[axis]) / localDirection[axis];
				if (first > second) std::swap(first, second);
				minimum = std::max(minimum, first);
				maximum = std::min(maximum, second);
				if (maximum < minimum) return std::nullopt;
			}
			return minimum;
		}
	}

	std::optional<EditorViewportPick> PickEditorViewport(
		const std::shared_ptr<WorldRoot> &world,
		const std::shared_ptr<Camera> &camera,
		std::uint32_t width,
		std::uint32_t height,
		float x,
		float y
	) {
		if (!world || !camera || width == 0 || height == 0 || !std::isfinite(x) || !std::isfinite(y) ||
			x < 0.0f || y < 0.0f || x >= width || y >= height)
			return std::nullopt;
		const auto cameraFrame = camera->GetCFrame();
		const float tangent = std::tan(glm::radians(camera->GetFieldOfView()) * 0.5f);
		const float normalizedX = ((x + 0.5f) / static_cast<float>(width)) * 2.0f - 1.0f;
		const float normalizedY = 1.0f - ((y + 0.5f) / static_cast<float>(height)) * 2.0f;
		const float aspect = static_cast<float>(width) / static_cast<float>(height);
		const auto direction = glm::normalize(
			cameraFrame.GetLookVector() + cameraFrame.GetRightVector() * (normalizedX * tangent * aspect) +
			cameraFrame.GetUpVector() * (normalizedY * tangent)
		);

		std::optional<EditorViewportPick> closest;
		for (const auto &instance : world->GetDescendants()) {
			auto part = std::dynamic_pointer_cast<BasePart>(instance);
			if (!part || part->GetDestroyed() || part->IsDestroying()) continue;
			auto distance = IntersectPart(*part, cameraFrame.Position, direction);
			if (!distance || (closest && *distance >= closest->Distance)) continue;
			closest = EditorViewportPick{part->GetObjectId(), *distance};
		}
		return closest;
	}

	EditorViewportRenderer::EditorViewportRenderer(std::uint32_t width, std::uint32_t height)
		: Width(width), Height(height) {
		if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
				throw std::runtime_error(std::format("Failed to initialize SDL video: {}", SDL_GetError()));
			OwnsVideoSubsystem = true;
		}

		Gpu = SDL_CreateGPUDevice(ShaderFormats, true, nullptr);
		if (!Gpu) {
			if (OwnsVideoSubsystem) SDL_QuitSubSystem(SDL_INIT_VIDEO);
			OwnsVideoSubsystem = false;
			throw std::runtime_error(std::format("Failed to create viewport GPU device: {}", SDL_GetError()));
		}

		SDL_GPUTextureCreateInfo shadowInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = 2048,
			.height = 2048,
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		ShadowMapTexture = SDL_CreateGPUTexture(Gpu, &shadowInfo);

		SDL_GPUSamplerCreateInfo samplerInfo{
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
			.enable_compare = true,
		};
		ShadowSampler = SDL_CreateGPUSampler(Gpu, &samplerInfo);
		if (!ShadowMapTexture || !ShadowSampler) {
			Destroy();
			throw std::runtime_error(std::format("Failed to create viewport shadow resources: {}", SDL_GetError()));
		}

		try {
			RecreateTargets();
			for (const auto &constructor : RENDER_PASS_CONSTRUCTORS) {
				RenderPasses.push_back(constructor(Gpu, ColorFormat));
			}
		} catch (...) {
			Destroy();
			throw;
		}
	}

	EditorViewportRenderer::~EditorViewportRenderer() { Destroy(); }

	void EditorViewportRenderer::Destroy() {
		if (Gpu) SDL_WaitForGPUIdle(Gpu);
		if (Gpu) MeshProvider::Destroy(Gpu);
		for (auto &pass : RenderPasses) pass->Destroy(Gpu);
		RenderPasses.clear();
		if (DownloadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
		if (ColorTexture) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
		if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
		DownloadBuffer = nullptr;
		ColorTexture = nullptr;
		DepthTexture = nullptr;
		if (ShadowMapTexture) SDL_ReleaseGPUTexture(Gpu, ShadowMapTexture);
		if (ShadowSampler) SDL_ReleaseGPUSampler(Gpu, ShadowSampler);
		ShadowMapTexture = nullptr;
		ShadowSampler = nullptr;
		if (Gpu) SDL_DestroyGPUDevice(Gpu);
		Gpu = nullptr;
		if (OwnsVideoSubsystem) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		OwnsVideoSubsystem = false;
	}

	void EditorViewportRenderer::RecreateTargets() {
		if (!Gpu || Width == 0 || Height == 0) throw std::invalid_argument("Viewport dimensions must be nonzero");
		SDL_WaitForGPUIdle(Gpu);
		if (DownloadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
		if (ColorTexture) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
		if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);

		SDL_GPUTextureCreateInfo colorInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = ColorFormat,
			.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			.width = Width,
			.height = Height,
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		ColorTexture = SDL_CreateGPUTexture(Gpu, &colorInfo);

		SDL_GPUTextureCreateInfo depthInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
			.width = Width,
			.height = Height,
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		DepthTexture = SDL_CreateGPUTexture(Gpu, &depthInfo);

		SDL_GPUTransferBufferCreateInfo downloadInfo{
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
			.size = Width * Height * 4,
		};
		DownloadBuffer = SDL_CreateGPUTransferBuffer(Gpu, &downloadInfo);
		if (!ColorTexture || !DepthTexture || !DownloadBuffer) {
			if (DownloadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, DownloadBuffer);
			if (ColorTexture) SDL_ReleaseGPUTexture(Gpu, ColorTexture);
			if (DepthTexture) SDL_ReleaseGPUTexture(Gpu, DepthTexture);
			DownloadBuffer = nullptr;
			ColorTexture = nullptr;
			DepthTexture = nullptr;
			throw std::runtime_error(std::format("Failed to create viewport targets: {}", SDL_GetError()));
		}
	}

	void EditorViewportRenderer::Resize(std::uint32_t width, std::uint32_t height) {
		if (width == Width && height == Height) return;
		const auto previousWidth = Width;
		const auto previousHeight = Height;
		Width = width;
		Height = height;
		try {
			RecreateTargets();
		} catch (...) {
			Width = previousWidth;
			Height = previousHeight;
			throw;
		}
	}

	EditorViewportFrame EditorViewportRenderer::Capture(const DrawContext &context) {
		if (!context.WorldRoot || !context.Camera) throw std::invalid_argument("Viewport draw context is incomplete");
		MeshProvider::UploadToGpu(Gpu);
		auto *commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!commands) throw std::runtime_error(std::format("Failed to acquire viewport command buffer: {}", SDL_GetError()));

		FrameContext frame;
		frame.WorldRoot = context.WorldRoot;
		frame.Camera = context.Camera;
		frame.LightDirection = glm::normalize(glm::vec3(0.75f, 1.0f, 0.5f));
		frame.Commands = commands;
		frame.SwapchainTexture = ColorTexture;
		frame.DepthTexture = DepthTexture;
		frame.ShadowMapTexture = ShadowMapTexture;
		frame.ShadowSampler = ShadowSampler;
		frame.Width = Width;
		frame.Height = Height;
		for (auto &pass : RenderPasses) SDL_EndGPURenderPass(pass->Draw(Gpu, frame));

		auto *copyPass = SDL_BeginGPUCopyPass(commands);
		SDL_GPUTextureRegion source{
			.texture = ColorTexture,
			.w = Width,
			.h = Height,
			.d = 1,
		};
		SDL_GPUTextureTransferInfo destination{
			.transfer_buffer = DownloadBuffer,
			.pixels_per_row = Width,
			.rows_per_layer = Height,
		};
		SDL_DownloadFromGPUTexture(copyPass, &source, &destination);
		SDL_EndGPUCopyPass(copyPass);

		auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
		if (!fence) throw std::runtime_error(std::format("Failed to submit viewport frame: {}", SDL_GetError()));
		if (!SDL_WaitForGPUFences(Gpu, true, &fence, 1)) {
			SDL_ReleaseGPUFence(Gpu, fence);
			throw std::runtime_error(std::format("Failed to wait for viewport frame: {}", SDL_GetError()));
		}

		auto *rgba = static_cast<const std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, DownloadBuffer, false));
		if (!rgba) {
			SDL_ReleaseGPUFence(Gpu, fence);
			throw std::runtime_error(std::format("Failed to map viewport frame: {}", SDL_GetError()));
		}
		EditorViewportFrame result{.Width = Width, .Height = Height};
		result.RgbPixels.resize(static_cast<std::size_t>(Width) * Height * 3);
		for (std::size_t sourceIndex = 0, destinationIndex = 0;
			 sourceIndex < static_cast<std::size_t>(Width) * Height * 4;
			 sourceIndex += 4, destinationIndex += 3) {
			std::memcpy(result.RgbPixels.data() + destinationIndex, rgba + sourceIndex, 3);
		}
		SDL_UnmapGPUTransferBuffer(Gpu, DownloadBuffer);
		SDL_ReleaseGPUFence(Gpu, fence);
		return result;
	}
}

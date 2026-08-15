// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/RenderPass.hpp"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	std::unique_ptr<RenderPass> CreateOpaquePass(SDL_GPUDevice *GpuDevice, SDL_GPUTextureFormat SwapchainFormat);
	std::unique_ptr<RenderPass> CreateShadowPass(SDL_GPUDevice *GpuDevice, SDL_GPUTextureFormat SwapchainFormat);
	std::unique_ptr<RenderPass> CreateGuiPass(SDL_GPUDevice *GpuDevice, SDL_GPUTextureFormat SwapchainFormat);

	using RenderPassConstructor =
		std::function<std::unique_ptr<RenderPass>(SDL_GPUDevice *GpuDevice, SDL_GPUTextureFormat SwapchainFormat)>;
	extern const std::vector<RenderPassConstructor> RENDER_PASS_CONSTRUCTORS;

	class SDLRenderer final : public BaseRenderer {
	  public:
		explicit SDLRenderer(const Vector2 &ViewportSize);
		~SDLRenderer() override;

		void Draw(RenderSnapshotPtr Snapshot) override;
		void Resize(int WidthValue, int HeightValue) override;
		void Destroy() override;
		[[nodiscard]] std::string GetDriverName() const;
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height)};
		}

	  private:
		int Width = 0;
		int Height = 0;
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTextureFormat SwapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		bool WindowClaimed = false;

		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

		std::vector<std::unique_ptr<RenderPass>> RenderPasses;
		std::unique_ptr<GpuMeshCache> MeshResources;
	};
} // namespace gargantuan

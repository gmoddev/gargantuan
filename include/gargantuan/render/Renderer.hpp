#pragma once

#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/render/RenderPass.hpp"

#include <SDL3/SDL.h>
#include <functional>
#include <glm/glm.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace gargantuan {
	class BaseRenderer {
	  public:
		BaseRenderer(Vector2 &viewportSize) {};
		virtual ~BaseRenderer() = default;

		BaseRenderer(const BaseRenderer &) = delete;
		BaseRenderer &operator=(const BaseRenderer &) = delete;

		virtual void Draw(RenderSnapshotPtr snapshot) = 0;
		virtual void Resize(int width, int height) = 0;
		virtual void Destroy() = 0;
		[[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const = 0;
	};

	class HeadlessRenderer final : public BaseRenderer {
	  public:
		HeadlessRenderer(Vector2 &viewportSize)
			: BaseRenderer(viewportSize), Width(static_cast<std::uint32_t>(viewportSize.GetX())),
			  Height(static_cast<std::uint32_t>(viewportSize.GetY())) {};

		void Draw(RenderSnapshotPtr _snapshot) override {};
		void Resize(int width, int height) override {
			if (width < 1 || height < 1) return;
			Width = static_cast<std::uint32_t>(width);
			Height = static_cast<std::uint32_t>(height);
		};
		void Destroy() override {};
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {Width, Height};
		}

	  private:
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	std::unique_ptr<RenderPass> CreateOpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat);
	std::unique_ptr<RenderPass> CreateShadowPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat);
	std::unique_ptr<RenderPass> CreateGuiPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat);

	typedef std::function<std::unique_ptr<RenderPass>(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat)>
		RenderPassConstructor;
	extern const std::vector<RenderPassConstructor> RENDER_PASS_CONSTRUCTORS;

	class SDLRenderer final : public BaseRenderer {
	  public:
		SDLRenderer(Vector2 &viewportSize);
		~SDLRenderer() override;

		int Width = 0;
		int Height = 0;
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTextureFormat SwapchainFormat;

		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

		std::vector<std::unique_ptr<RenderPass>> RenderPasses;
		std::unique_ptr<GpuMeshCache> MeshResources;

		void Draw(RenderSnapshotPtr snapshot) override;
		void Resize(int width, int height) override;
		void Destroy() override;
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height)};
		}
	};
} // namespace gargantuan

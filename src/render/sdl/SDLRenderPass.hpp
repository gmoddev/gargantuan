#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "render/sdl/SDLShader.hpp"

#include <SDL3/SDL_gpu.h>

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace gargantuan {
	class SDLMeshCache;
	class SDLTextureCache;
	struct SDLRendererMetrics;

	struct SDLFrameContext {
		SDLFrameContext(const RenderProjection &RenderState, SDLMeshCache &MeshResourcesValue)
			: Projection(RenderState), MeshResources(MeshResourcesValue) {}

		const RenderProjection &Projection;
		SDLMeshCache &MeshResources;
		SDLTextureCache *TextureResources = nullptr;
		SDLRendererMetrics *Metrics = nullptr;
		SDL_GPUCommandBuffer *Commands = nullptr;
		SDL_GPUTexture *SwapchainTexture = nullptr;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		glm::mat4 ShadowMatrix{1.0f};
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	class SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "SDLRenderPass";
		SDLFileShader Shader;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;

		virtual ~SDLRenderPass() = default;
		virtual SDL_GPURenderPass *Draw(SDL_GPUDevice *Gpu, SDLFrameContext &Context) = 0;
		virtual void Resize(SDL_GPUDevice *, std::uint32_t, std::uint32_t) {}
		virtual void Destroy(SDL_GPUDevice *Gpu);
	};

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat);
	std::unique_ptr<SDLRenderPass> CreateShadowPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat);
	std::unique_ptr<SDLRenderPass> CreateSkyPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat);
	std::unique_ptr<SDLRenderPass> CreateGuiPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat);
	using SDLRenderPassConstructor =
		std::function<std::unique_ptr<SDLRenderPass>(SDL_GPUDevice *, SDL_GPUTextureFormat)>;
	[[nodiscard]] const std::vector<SDLRenderPassConstructor> &GetSDLRenderPassConstructors();
}

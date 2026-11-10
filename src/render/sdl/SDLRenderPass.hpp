#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "render/sdl/SDLImageBasedLighting.hpp"
#include "render/sdl/SDLShader.hpp"

#include <SDL3/SDL_gpu.h>

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	class SDLDirectionalShadowCache;
	class SDLMeshCache;
	class SDLSkinPaletteCache;
	class SDLTextureCache;
	class SDLMaterialSamplers;
	struct SDLRendererMetrics;

	struct SDLFrameContext {
		bool EnableOpaqueGrouping = true;
		SDLFrameContext(
			const RenderProjection &RenderState,
			SDLMeshCache &MeshResourcesValue,
			SDLSkinPaletteCache &SkinPalettesValue,
			SDLDirectionalShadowCache &ShadowCacheValue,
			std::unordered_map<ObjectId, std::uint64_t> &ShadowPoseRevisionsValue
		) : Projection(RenderState), MeshResources(MeshResourcesValue), SkinPalettes(SkinPalettesValue),
			ShadowCache(ShadowCacheValue), ShadowPoseRevisions(ShadowPoseRevisionsValue) {}

		const RenderProjection &Projection;
		SDLMeshCache &MeshResources;
		SDLSkinPaletteCache &SkinPalettes;
		SDLTextureCache *TextureResources = nullptr;
		SDLMaterialSamplers *MaterialSamplers = nullptr;
		SDLImageBasedLightingBindings EnvironmentLighting;
		SDLRendererMetrics *Metrics = nullptr;
		SDL_GPUCommandBuffer *Commands = nullptr;
		SDL_GPUTexture *SwapchainTexture = nullptr;
		SDL_GPUTexture *HdrSceneTexture = nullptr;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		glm::mat4 ShadowMatrix{1.0f};
		SDLDirectionalShadowCache &ShadowCache;
		std::unordered_map<ObjectId, std::uint64_t> &ShadowPoseRevisions;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;

		[[nodiscard]] SDL_GPUTexture *GetSceneColorTexture() const {
			return HdrSceneTexture ? HdrSceneTexture : SwapchainTexture;
		}
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

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics);
	std::unique_ptr<SDLRenderPass> CreateShadowPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics);
	std::unique_ptr<SDLRenderPass> CreateSkyPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics);
	std::unique_ptr<SDLRenderPass> CreateGuiPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics);
	std::unique_ptr<SDLRenderPass> CreateSceneResolvePass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics);
	using SDLRenderPassConstructor =
		std::function<std::unique_ptr<SDLRenderPass>(SDL_GPUDevice *, SDL_GPUTextureFormat, SDLRendererMetrics *)>;
	enum class SDLPassTarget { Scene, Output, Resolve };
	struct SDLRenderPassDefinition {
		SDLRenderPassConstructor Construct;
		SDLPassTarget Target;
	};
	[[nodiscard]] const std::vector<SDLRenderPassDefinition> &GetSDLRenderPassConstructors();
}

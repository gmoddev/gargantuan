#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"
#include "gargantuan/render/Shader.hpp"

#include <SDL3/SDL.h>

#include <string_view>

namespace gargantuan {
	class GpuMeshCache;

	struct FrameContext {
		FrameContext(const RenderSnapshot &snapshot, GpuMeshCache &meshResources)
			: Snapshot(snapshot), MeshResources(meshResources) {}

		const RenderSnapshot &Snapshot;
		GpuMeshCache &MeshResources;
		SDL_GPUCommandBuffer *Commands = nullptr;

		SDL_GPUTexture *SwapchainTexture = nullptr;
		SDL_GPUTexture *DepthTexture = nullptr;

		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		glm::mat4 ShadowMatrix{1.0f};

		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	class RenderPass {
	  public:
		static constexpr std::string_view LABEL = "RenderPass";

		Shader Shader;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;

		virtual ~RenderPass() = default;
		virtual SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, FrameContext &context) = 0;
		virtual void Resize(SDL_GPUDevice *gpu, uint32_t width, uint32_t height) {};
		virtual void Destroy(SDL_GPUDevice *gpu);
	};
} // namespace gargantuan

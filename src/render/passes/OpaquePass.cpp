#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"

#include <SDL3/SDL.h>
#include <memory>
#include <string>

namespace gargantuan {
	static const glm::mat4 SHADOW_BIAS_MATRIX{
		//
		0.5f,
		0.0f,
		0.0f,
		0.0f,
		//
		0.0f,
		-0.5f,
		0.0f,
		0.0f,
		//
		0.0f,
		0.0f,
		1.0f,
		0.0f,
		//
		0.5f,
		0.5f,
		0.0f,
		1.0f
	};

	class OpaquePass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Opaque";

		struct alignas(16) WorldUniforms {
			glm::mat4 ViewMatrix;
			glm::mat4 ProjectionMatrix;
			glm::mat4 ShadowBiasMatrix;
			glm::vec4 LightDirection;
		};

		struct alignas(16) PartUniforms {
			glm::mat4 ModelMatrix;
			glm::vec4 Color;
		};

		OpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
			Shader.VertexFilepath = GetSDLShaderPath("opaque.vert");
			Shader.VertexUniformBufferCount = 2;
			Shader.FragmentFilepath = GetSDLShaderPath("opaque.frag");
			Shader.FragmentUniformBufferCount = 1;
			Shader.FragmentSamplerCount = 1;
			Shader.Init(gpu);
			Pipeline = SDLPipelineBuilder()
						   .SetVertexShader(Shader.VertexShader)
						   .SetFragmentShader(Shader.FragmentShader)
						   .SetColorEnabled(true)
						   .SetColorFormat(swapchainFormat)
						   .SetBlendingEnabled(true)
						   .SetDepthEnabled(true)
						   .SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D16_UNORM)
						   .Build(gpu);
		};

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			SDL_GPUColorTargetInfo colorTarget = {
				.texture = context.SwapchainTexture,
				.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f},
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE,
			};

			SDL_GPUDepthStencilTargetInfo depthTarget = {
				.texture = context.DepthTexture,
				.clear_depth = 1.0f,
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_DONT_CARE,
				.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
				.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			};

			SDL_GPUTextureSamplerBinding shadowBinding{
				.texture = context.ShadowMapTexture,
				.sampler = context.ShadowSampler,
			};

			auto pass = SDL_BeginGPURenderPass(context.Commands, &colorTarget, 1, &depthTarget);
			SDL_BindGPUGraphicsPipeline(pass, Pipeline);
			if (context.Metrics) ++context.Metrics->PipelineSwitches;
			SDL_BindGPUFragmentSamplers(pass, 0, &shadowBinding, 1);

			WorldUniforms worldUniforms{
				.ViewMatrix = context.Projection.GetFrame().Camera.ViewMatrix,
				.ProjectionMatrix = context.Projection.GetFrame().Camera.ProjectionMatrix,
				.ShadowBiasMatrix = SHADOW_BIAS_MATRIX * context.ShadowMatrix,
				.LightDirection = glm::vec4(context.Projection.GetFrame().LightDirection, 0.0f),
			};
			SDL_PushGPUVertexUniformData(context.Commands, 0, &worldUniforms, sizeof(WorldUniforms));
			SDL_PushGPUFragmentUniformData(context.Commands, 0, &worldUniforms, sizeof(WorldUniforms));

			for (const auto &[Object, Projected] : context.Projection.GetObjects()) {
				(void)Object;
				if (!Projected.Visible) continue;
				const auto &item = Projected.Item;
				const auto *mesh = Projected.Mesh
					? context.MeshResources.Find(*Projected.Mesh) : context.MeshResources.Find(item.Geometry);
				if (!mesh || !mesh->VertexBuffer || !mesh->IndexBuffer) {
					LOG_WARN(
						App,
						"RenderPublication %llu skipped ObjectId %u:%u because its GPU resource is unavailable",
						static_cast<unsigned long long>(context.Projection.GetLastPublicationId()),
						item.Object.Slot,
						item.Object.Generation
					);
					continue;
				}

				PartUniforms uniforms{
					.ModelMatrix = item.ModelMatrix,
					.Color = item.Color,
				};
				SDL_PushGPUVertexUniformData(context.Commands, 1, &uniforms, sizeof(PartUniforms));

				SDL_GPUBufferBinding vertexBinding{.buffer = mesh->VertexBuffer, .offset = 0};
				SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

				SDL_GPUBufferBinding indexBinding{.buffer = mesh->IndexBuffer, .offset = 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				SDL_DrawGPUIndexedPrimitives(pass, mesh->IndexCount, 1, 0, 0, 0);
				if (context.Metrics) ++context.Metrics->DrawCalls;
			}

			return pass;
		};
	};

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
		return std::make_unique<OpaquePass>(gpu, swapchainFormat);
	}
} // namespace gargantuan

#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <memory>

namespace gargantuan {

	class ShadowPass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Shadow";

		struct alignas(16) Uniforms {
			glm::mat4 ShadowMatrix;
			glm::mat4 PartMatrix;
		};

		ShadowPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
			(void)swapchainFormat;
			Shader.VertexFilepath = GetSDLShaderPath("shadow.vert");
			Shader.VertexUniformBufferCount = 1;
			Shader.FragmentFilepath = GetSDLShaderPath("shadow.frag");
			Shader.Init(gpu);
			Pipeline = SDLPipelineBuilder()
						   .SetVertexShader(Shader.VertexShader)
						   .SetFragmentShader(Shader.FragmentShader)
						   .SetColorEnabled(false)
						   .SetDepthEnabled(true)
						   .SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT)
						   .Build(gpu);
		};

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			(void)gpu;
			glm::mat4 shadowProjection = glm::ortho<float>(-30.0f, 30.0f, -30.0f, 30.0f, -50.0f, 150.0f);
			glm::vec3 lightPosition = glm::normalize(context.Snapshot.LightDirection) * 40.0f;
			glm::mat4 shadowView = glm::lookAt(lightPosition, glm::vec3(0), glm::vec3(0, 1, 0));
			glm::mat4 shadowMatrix = shadowProjection * shadowView;
			context.ShadowMatrix = shadowMatrix;

			SDL_GPUDepthStencilTargetInfo depthTarget{
				.texture = context.ShadowMapTexture,
				.clear_depth = 1.0f,
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE,
				.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
				.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			};

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(context.Commands, nullptr, 0, &depthTarget);
			SDL_BindGPUGraphicsPipeline(pass, Pipeline);

			for (const auto &item : context.Snapshot.Items) {
				if (!item.CastShadow) {
					continue;
				}

				const auto *mesh = context.MeshResources.Find(item.Geometry);
				if (!mesh || !mesh->VertexBuffer || !mesh->IndexBuffer) {
					LOG_WARN(
						App,
						"RenderSnapshot %llu shadow skipped ObjectId %u:%u because its primitive GPU resource is unavailable",
						static_cast<unsigned long long>(context.Snapshot.Id),
						item.Object.Slot,
						item.Object.Generation
					);
					continue;
				}

				Uniforms uniforms{.ShadowMatrix = shadowMatrix, .PartMatrix = item.ModelMatrix};
				SDL_PushGPUVertexUniformData(context.Commands, 0, &uniforms, sizeof(Uniforms));

				SDL_GPUBufferBinding vertexBinding{.buffer = mesh->VertexBuffer, .offset = 0};
				SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

				SDL_GPUBufferBinding indexBinding{.buffer = mesh->IndexBuffer, .offset = 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

				SDL_DrawGPUIndexedPrimitives(pass, mesh->IndexCount, 1, 0, 0, 0);
			}

			return pass;
		};
	};

	std::unique_ptr<SDLRenderPass> CreateShadowPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
		return std::make_unique<ShadowPass>(gpu, swapchainFormat);
	}

} // namespace gargantuan

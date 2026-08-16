#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLRenderPass.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <memory>

namespace gargantuan {

	class GuiPass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Gui";

		GuiPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
			(void)gpu;
			(void)swapchainFormat;
			Shader.VertexFilepath = GetSDLShaderPath("gui.vert");
			Shader.FragmentFilepath = GetSDLShaderPath("gui.frag");
			// Shader.Init(gpu);

			// Pipeline = PipelineBuilder()
			// 			   .SetVertexShader(Shader.VertexShader)
			// 			   .SetFragmentShader(Shader.FragmentShader)
			// 			   .SetColorEnabled(false)
			// 			   .SetDepthEnabled(true)
			// 			   .SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT)
			// 			   .Build(gpu);
		};

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			(void)gpu;
			// glm::mat4 shadowProjection = glm::ortho<float>(-30.0f, 30.0f, -30.0f, 30.0f, -50.0f, 150.0f);
			// glm::vec3 lightPosition = glm::normalize(context.LightDirection) * 40.0f;
			// glm::mat4 shadowView = glm::lookAt(lightPosition, glm::vec3(0), glm::vec3(0, 1, 0));
			// glm::mat4 shadowMatrix = shadowProjection * shadowView;
			// context.ShadowMatrix = shadowMatrix;

			// SDL_GPUDepthStencilTargetInfo depthTarget{
			// 	.texture = context.ShadowMapTexture,
			// 	.clear_depth = 1.0f,
			// 	.load_op = SDL_GPU_LOADOP_CLEAR,
			// 	.store_op = SDL_GPU_STOREOP_STORE,
			// 	.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
			// 	.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			// };

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(context.Commands, nullptr, 0, nullptr);
			// SDL_BindGPUGraphicsPipeline(pass, Pipeline);

			return pass;
		};
	};

	std::unique_ptr<SDLRenderPass> CreateGuiPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat) {
		return std::make_unique<GuiPass>(gpu, swapchainFormat);
	}

} // namespace gargantuan

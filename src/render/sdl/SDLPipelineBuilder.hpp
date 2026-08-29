#pragma once

#include <SDL3/SDL_gpu.h>

namespace gargantuan {
	struct SDLRendererMetrics;
	struct SDLPipelineBuilder final {
		SDL_GPUShader *VertexShader = nullptr;
		SDL_GPUShader *FragmentShader = nullptr;
		SDL_GPUTextureFormat ColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		bool ColorEnabled = false;
		bool BlendingEnabled = false;
		SDL_GPUTextureFormat DepthFormat = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
		bool DepthEnabled = false;

		SDLPipelineBuilder &SetVertexShader(SDL_GPUShader *Shader);
		SDLPipelineBuilder &SetFragmentShader(SDL_GPUShader *Shader);
		SDLPipelineBuilder &SetColorFormat(SDL_GPUTextureFormat Format);
		SDLPipelineBuilder &SetColorEnabled(bool Enabled);
		SDLPipelineBuilder &SetBlendingEnabled(bool Enabled);
		SDLPipelineBuilder &SetDepthFormat(SDL_GPUTextureFormat Format);
		SDLPipelineBuilder &SetDepthEnabled(bool Enabled);
		SDL_GPUGraphicsPipelineCreateInfo BuildInfo();
		SDL_GPUGraphicsPipeline *Build(SDL_GPUDevice *Gpu, SDLRendererMetrics *Metrics = nullptr);

	  private:
		SDL_GPUColorTargetDescription ColorTarget{};
	};
}

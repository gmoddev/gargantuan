#include "render/sdl/SDLRenderPass.hpp"

#include <SDL3/SDL.h>

namespace gargantuan {
	void SDLRenderPass::Destroy(SDL_GPUDevice *gpu) {
		if (Pipeline) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, Pipeline);
			Pipeline = nullptr;
		}

		Shader.Destroy(gpu);
	}
} // namespace gargantuan

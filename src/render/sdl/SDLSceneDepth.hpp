#pragma once

#include <SDL3/SDL_gpu.h>
#include <stdexcept>

namespace gargantuan {
	// Shared by runtime/editor target recreation and both opaque pipeline variants.
	// D16 loses separated surfaces in the supported editor camera range (KI-008).
	// D32 is already required by the independent shadow path; never silently
	// downgrade scene depth. Conventional Z, LESS and clear=1 remain unchanged.
	inline constexpr SDL_GPUTextureFormat SDLSceneDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

	inline SDL_GPUTextureCreateInfo GetSDLSceneDepthTargetInfo(SDL_GPUDevice *Device, Uint32 Width, Uint32 Height) {
		if (!SDL_GPUTextureSupportsFormat(Device, SDLSceneDepthFormat, SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			throw std::runtime_error("[Render:Depth] D32_FLOAT scene depth attachment is unsupported");
		return {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDLSceneDepthFormat,
			.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, .width = Width, .height = Height,
			.layer_count_or_depth = 1, .num_levels = 1};
	}
}

#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace gargantuan {
	inline constexpr auto SDLHdrSceneFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
	inline constexpr auto SDLHdrSceneUsage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;

	inline bool SupportsSDLHdrScene(SDL_GPUDevice *Gpu) {
		return SDL_GPUTextureSupportsFormat(Gpu, SDLHdrSceneFormat, SDL_GPU_TEXTURETYPE_2D, SDLHdrSceneUsage);
	}

	inline SDL_GPUTexture *CreateSDLHdrScene(SDL_GPUDevice *Gpu, std::uint32_t Width, std::uint32_t Height) {
		const SDL_GPUTextureCreateInfo Info{
			.type = SDL_GPU_TEXTURETYPE_2D, .format = SDLHdrSceneFormat, .usage = SDLHdrSceneUsage,
			.width = Width, .height = Height, .layer_count_or_depth = 1, .num_levels = 1,
		};
		return SDL_CreateGPUTexture(Gpu, &Info);
	}

	inline bool SDLTargetEncodesSrgb(SDL_GPUTextureFormat Format) {
		return Format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
			Format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
	}
}

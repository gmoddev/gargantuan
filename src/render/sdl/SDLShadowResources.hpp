// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "render/sdl/SDLDirectionalCascadePlan.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace gargantuan {
	// Each spotlight owns one complete depth layer. This avoids partial-layer
	// clears, fragmentation, and stale neighbors when only one light updates.
	inline constexpr std::uint32_t MaximumShadowedSpotlights = 8;
	inline constexpr std::uint32_t SpotlightShadowResolution = 1024;

	class SDLShadowResources final {
	  public:
		[[nodiscard]] bool EnsureDirectional(SDL_GPUDevice *Gpu, std::uint32_t Resolution) {
			if (Directional && DirectionalResolution == Resolution) return true;
			if (Resolution < 256 || Resolution > 4096) return false;
			constexpr auto Usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			if (!SDL_GPUTextureSupportsFormat(Gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				SDL_GPU_TEXTURETYPE_2D_ARRAY, Usage)) return false;
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
				.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				.usage = Usage,
				.width = Resolution,
				.height = Resolution,
				.layer_count_or_depth = MaximumDirectionalCascades,
				.num_levels = 1,
			};
			auto *Replacement = SDL_CreateGPUTexture(Gpu, &Info);
			if (!Replacement) return false;
			if (Directional) SDL_ReleaseGPUTexture(Gpu, Directional);
			Directional = Replacement;
			DirectionalResolution = Resolution;
			return true;
		}

		[[nodiscard]] bool EnsureLocal(SDL_GPUDevice *Gpu) {
			if (Local) return true;
			constexpr auto Usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			if (!SDL_GPUTextureSupportsFormat(Gpu, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				SDL_GPU_TEXTURETYPE_2D_ARRAY, Usage)) return false;
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_2D_ARRAY,
				.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				.usage = Usage,
				.width = SpotlightShadowResolution,
				.height = SpotlightShadowResolution,
				.layer_count_or_depth = MaximumShadowedSpotlights,
				.num_levels = 1,
			};
			Local = SDL_CreateGPUTexture(Gpu, &Info);
			return Local != nullptr;
		}

		void Destroy(SDL_GPUDevice *Gpu) {
			if (Directional) SDL_ReleaseGPUTexture(Gpu, Directional);
			if (Local) SDL_ReleaseGPUTexture(Gpu, Local);
			Directional = nullptr;
			Local = nullptr;
			DirectionalResolution = 0;
		}

		[[nodiscard]] SDL_GPUTexture *GetDirectional() const { return Directional; }
		[[nodiscard]] SDL_GPUTexture *GetLocal() const { return Local; }
		[[nodiscard]] std::uint64_t LogicalDirectionalBytes() const {
			return std::uint64_t(DirectionalResolution) * DirectionalResolution * MaximumDirectionalCascades * 4;
		}
		[[nodiscard]] static constexpr std::uint64_t LogicalLocalBytes() {
			return std::uint64_t(SpotlightShadowResolution) * SpotlightShadowResolution * MaximumShadowedSpotlights * 4;
		}

	  private:
		SDL_GPUTexture *Directional = nullptr;
		SDL_GPUTexture *Local = nullptr;
		std::uint32_t DirectionalResolution = 0;
	};
}

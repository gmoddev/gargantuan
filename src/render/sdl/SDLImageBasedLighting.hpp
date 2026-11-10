// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderEnvironment.hpp"
#include "render/sdl/SDLShader.hpp"

#include <array>
#include <optional>
#include <utility>

namespace gargantuan {
	class SDLTextureCache;
	struct SDLRendererMetrics;

	struct SDLImageBasedLightingBindings {
		SDL_GPUTextureSamplerBinding Irradiance{};
		SDL_GPUTextureSamplerBinding Reflections{};
		SDL_GPUTextureSamplerBinding BrdfLut{};
		bool Enabled = false;
		float ReflectionMaxLod = 0.0f;
	};

	class SDLImageBasedLighting final {
	  public:
		explicit SDLImageBasedLighting(SDL_GPUDevice *Device) : Gpu(Device) {}
		~SDLImageBasedLighting();
		SDLImageBasedLighting(const SDLImageBasedLighting &) = delete;
		SDLImageBasedLighting &operator=(const SDLImageBasedLighting &) = delete;

		[[nodiscard]] SDLImageBasedLightingBindings Prepare(
			const std::optional<RenderSkyState> &Sky, SDLTextureCache &Textures, SDLRendererMetrics *Metrics
		);
		void Destroy();

	  private:
		struct SkyKey {
			std::uint64_t ResidencyGeneration = 0;
			std::uint32_t Dimension = 0;
			std::array<std::pair<RenderTextureIdentity, std::uint64_t>, 6> Faces;
			bool operator==(const SkyKey &) const = default;
		};

		void EnsureFallback(SDLRendererMetrics *Metrics);
		bool EnsurePipelines(SDLRendererMetrics *Metrics);
		bool CanBuildSky();
		void BuildSky(const SkyKey &Key, SDLTextureCache &Textures, SDLRendererMetrics *Metrics);
		void ReleaseSky(SDLRendererMetrics *Metrics);
		void Release(SDL_GPUTexture *&Texture, SDLRendererMetrics *Metrics);

		SDL_GPUDevice *Gpu;
		SDL_GPUSampler *Sampler = nullptr;
		SDL_GPUTexture *FallbackCube = nullptr;
		SDL_GPUTexture *Irradiance = nullptr;
		SDL_GPUTexture *Reflections = nullptr;
		SDL_GPUTexture *BrdfLut = nullptr;
		SDL_GPUFence *PreparationFence = nullptr;
		std::array<SDLFileShader, 3> Shaders;
		std::array<SDL_GPUGraphicsPipeline *, 3> Pipelines{};
		std::optional<SkyKey> ActiveKey;
		std::optional<SkyKey> RejectedKey;
		bool PreprocessingUnavailable = false;
	};
}

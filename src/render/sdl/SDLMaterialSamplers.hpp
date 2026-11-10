#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <stdexcept>

namespace gargantuan {
	// Two min and mag filters, three wrap modes per axis, and three mip modes.
	// Nine extra entries cover anisotropic filtering with automatic min and mag.
	class SDLMaterialSamplers final {
	  public:
		static bool UsesNearestMinFilter(RenderTextureMinFilter Filter) {
			using Min = RenderTextureMinFilter;
			return Filter == Min::Nearest || Filter == Min::NearestMipmapNearest || Filter == Min::NearestMipmapLinear;
		}

		SDL_GPUSampler *Get(SDL_GPUDevice *Gpu, const RenderSamplerState &State, bool HasMipmaps) {
			using Min = RenderTextureMinFilter;
			const bool NearMin = UsesNearestMinFilter(State.MinFilter);
			const bool NearMag = State.MagFilter == RenderTextureMagFilter::Nearest;
			const bool EnableMipmaps = HasMipmaps && State.MinFilter != Min::Nearest && State.MinFilter != Min::Linear;
			const bool BlendLevels = EnableMipmaps && (State.MinFilter == Min::Default ||
				State.MinFilter == Min::NearestMipmapLinear || State.MinFilter == Min::LinearMipmapLinear);
			const auto Mip = EnableMipmaps ? (BlendLevels ? 2u : 1u) : 0u;
			const bool Anisotropic = EnableMipmaps && State.MinFilter == Min::Default &&
				State.MagFilter == RenderTextureMagFilter::Default;
			const auto WrapKey = static_cast<unsigned>(State.WrapU) * 3u + static_cast<unsigned>(State.WrapV);
			const auto Key = (Anisotropic ? 108u : ((Mip * 2u + NearMin) * 2u + NearMag) * 9u) + WrapKey;
			auto &Sampler = Cache.at(Key);
			if (Sampler) return Sampler;
			const auto Address = [](RenderTextureWrap Wrap) {
				switch (Wrap) {
					case RenderTextureWrap::MirroredRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
					case RenderTextureWrap::ClampToEdge: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
					default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
				}
			};
			SDL_GPUSamplerCreateInfo Info{
				.min_filter = NearMin ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR,
				.mag_filter = NearMag ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR,
				.mipmap_mode = BlendLevels ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
				.address_mode_u = Address(State.WrapU), .address_mode_v = Address(State.WrapV),
				.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.max_anisotropy = Anisotropic ? 4.0f : 1.0f,
				// Keep minification active while nearest mip selection stays at level zero.
				.max_lod = EnableMipmaps ? 16.0f : 0.25f,
				.enable_anisotropy = Anisotropic,
			};
			Sampler = SDL_CreateGPUSampler(Gpu, &Info);
			if (!Sampler && Anisotropic) {
				Info.enable_anisotropy = false;
				Info.max_anisotropy = 1.0f;
				Sampler = SDL_CreateGPUSampler(Gpu, &Info);
			}
			if (!Sampler) throw std::runtime_error("[Renderer:Material] Could not create a bounded material sampler");
			return Sampler;
		}

		void Destroy(SDL_GPUDevice *Gpu) {
			for (auto *&Sampler : Cache) {
				if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
				Sampler = nullptr;
			}
		}

	  private:
		std::array<SDL_GPUSampler *, 117> Cache{};
	};
}

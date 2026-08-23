#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <unordered_map>

namespace gargantuan {
	struct SDLRendererMetrics;

	class SDLTextureCache final {
	  public:
		explicit SDLTextureCache(SDL_GPUDevice *GpuDevice, SDLRendererMetrics *MetricsValue = nullptr)
			: Gpu(GpuDevice), Metrics(MetricsValue) {}

		SDLTextureCache(const SDLTextureCache &) = delete;
		SDLTextureCache &operator=(const SDLTextureCache &) = delete;

		void ApplyPublication(const RenderPublication &Publication);
		void Destroy();
		[[nodiscard]] SDL_GPUTexture *Find(RenderTextureIdentity Texture) const;

	  private:
		struct Entry {
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTransferBuffer *TransferBuffer = nullptr;
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
		};

		void DestroyEntry(Entry &Value);
		void Upload(
			Entry &Value,
			SDL_GPUCopyPass *CopyPass,
			std::uint32_t X,
			std::uint32_t Y,
			std::uint32_t Width,
			std::uint32_t Height,
			const std::vector<std::uint8_t> &Pixels,
			bool Initial
		);

		SDL_GPUDevice *Gpu = nullptr;
		SDLRendererMetrics *Metrics = nullptr;
		std::unordered_map<RenderTextureIdentity, Entry, RenderTextureIdentityHash> Textures;
	};
}

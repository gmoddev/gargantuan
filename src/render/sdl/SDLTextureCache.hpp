#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	struct SDLRendererMetrics;
	struct SDLMaterialTextureBinding {
		SDL_GPUTexture *Texture = nullptr;
		bool NativeSrgb = false;
		bool HasMipmaps = false;
	};

	class SDLTextureCache final {
	  public:
		explicit SDLTextureCache(SDL_GPUDevice *GpuDevice, SDLRendererMetrics *MetricsValue = nullptr)
			: Gpu(GpuDevice), Metrics(MetricsValue) {}

		SDLTextureCache(const SDLTextureCache &) = delete;
		SDLTextureCache &operator=(const SDLTextureCache &) = delete;

		void ApplyPublication(const RenderPublication &Publication);
		void Destroy();
		[[nodiscard]] SDL_GPUTexture *Find(RenderTextureIdentity Texture) const;
		[[nodiscard]] SDLMaterialTextureBinding FindMaterialTexture(RenderTextureIdentity Texture, bool Srgb) const;
		[[nodiscard]] SDL_GPUTexture *GetWhiteTexture();
		[[nodiscard]] std::uint64_t GetResidencyGeneration() const { return ResidencyGeneration; }
		[[nodiscard]] std::uint64_t GetMaterialViewBytes() const { return MaterialViewBytes; }
		[[nodiscard]] std::uint64_t GetMaterialSourceBytes() const { return MaterialSourceBytes; }
		[[nodiscard]] std::uint64_t GetMaterialViewFallbacks() const { return MaterialViewFallbacks; }

	  private:
		struct MaterialView {
			SDL_GPUTexture *Texture = nullptr;
			std::uint64_t AttemptedRevision = 0;
			std::uint32_t Bytes = 0;
			std::uint32_t Levels = 0;
		};

		struct Entry {
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTransferBuffer *TransferBuffer = nullptr;
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			std::shared_ptr<const std::vector<std::uint8_t>> Source;
			std::unique_ptr<std::vector<std::uint8_t>> PatchedSource;
			std::array<MaterialView, 2> Views;
			std::uint8_t RequestedViews = 0;
			std::uint8_t PendingViews = 0;
			[[nodiscard]] const std::vector<std::uint8_t> *GetSource() const {
				return PatchedSource ? PatchedSource.get() : Source.get();
			}
		};

		Entry CreateEntry(std::uint32_t Width, std::uint32_t Height, std::uint64_t Revision);
		void DestroyView(MaterialView &View);
		void ForgetSource(Entry &Value);
		void RetainSource(Entry &Value, const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels);
		void UpdateSource(Entry &Value, const RenderTextureUpdate &Update);
		void QueueMaterialViews(Entry &Value, std::uint8_t Requested);
		void QueueMaterial(const RenderMaterialState &Material);
		void UploadMaterialView(Entry &Value, unsigned Index, SDL_GPUCopyPass *CopyPass);
		void DestroyEntry(Entry &Value);
		void DestroyResident();
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
		std::uint64_t ResidencyGeneration = 0;
		std::uint64_t MaterialViewBytes = 0;
		std::uint64_t MaterialSourceBytes = 0;
		std::uint64_t MaterialViewFallbacks = 0;
		std::vector<Entry *> PendingMaterialEntries;
		std::vector<SDL_GPUTransferBuffer *> MaterialStaging;
		Entry White;
	};
}

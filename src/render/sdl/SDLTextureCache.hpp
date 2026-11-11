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
		explicit SDLTextureCache(SDL_GPUDevice *GpuDevice, SDLRendererMetrics *MetricsValue = nullptr,
			std::uint64_t MaterialBudget = 128ull * 1024 * 1024)
			: Gpu(GpuDevice), Metrics(MetricsValue),
			  MaterialViewBudget(MaterialBudget <= 128ull * 1024 * 1024 ? MaterialBudget : 128ull * 1024 * 1024) {}

		SDLTextureCache(const SDLTextureCache &) = delete;
		SDLTextureCache &operator=(const SDLTextureCache &) = delete;

		void ApplyPublication(const RenderPublication &Publication);
		void Destroy();
		[[nodiscard]] SDL_GPUTexture *Find(RenderTextureIdentity Texture) const;
		[[nodiscard]] RenderTextureFormat GetFormat(RenderTextureIdentity Texture) const;
		[[nodiscard]] SDLMaterialTextureBinding FindMaterialTexture(RenderTextureIdentity Texture, bool Srgb);
		[[nodiscard]] SDL_GPUTexture *GetWhiteTexture();
		[[nodiscard]] std::uint64_t GetResidencyGeneration() const { return ResidencyGeneration; }
		[[nodiscard]] std::uint64_t GetMaterialViewBytes() const { return MaterialViewBytes; }
		[[nodiscard]] std::uint64_t GetMaterialSourceBytes() const { return MaterialSourceBytes; }
		[[nodiscard]] std::uint64_t GetMaterialViewFallbacks() const { return MaterialViewFallbacks; }
		[[nodiscard]] std::uint64_t GetMaterialViewEvictions() const { return MaterialViewEvictions; }
		[[nodiscard]] std::uint64_t GetDeferredMaterialViewUploads() const { return DeferredMaterialViewUploads; }
		[[nodiscard]] std::uint64_t GetMaterialViewBudget() const { return MaterialViewBudget; }
		[[nodiscard]] std::uint32_t GetMaterialUploadStagingBytes() const { return MaterialUploadCapacity; }
		[[nodiscard]] std::uint64_t GetBaseUploadStagingBytes() const { return BaseUploadStagingBytes; }
		[[nodiscard]] std::uint64_t GetBaseUploadStagingEvictions() const { return BaseUploadStagingEvictions; }

	  private:
		struct MaterialView {
			SDL_GPUTexture *Texture = nullptr;
			std::uint64_t AttemptedRevision = 0;
			std::uint32_t Bytes = 0;
			std::uint32_t Levels = 0;
			std::uint64_t LastUsedEpoch = 0;
		};

		struct Entry {
			SDL_GPUTexture *Texture = nullptr;
			SDL_GPUTransferBuffer *TransferBuffer = nullptr;
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			RenderTextureFormat Format = RenderTextureFormat::Rgba8Unorm;
			std::shared_ptr<const std::vector<std::uint8_t>> Source;
			std::unique_ptr<std::vector<std::uint8_t>> PatchedSource;
			std::array<MaterialView, 2> Views;
			std::uint8_t RequestedViews = 0;
			std::uint8_t PendingViews = 0;
			std::uint8_t DeferredViews = 0;
			std::uint64_t LastDemandEpoch = 0;
			std::uint64_t LastUploadSerial = 0;
			[[nodiscard]] const std::vector<std::uint8_t> *GetSource() const {
				return PatchedSource ? PatchedSource.get() : Source.get();
			}
		};

		Entry CreateEntry(std::uint32_t Width, std::uint32_t Height, std::uint64_t Revision,
			RenderTextureFormat Format = RenderTextureFormat::Rgba8Unorm);
		void DestroyView(MaterialView &View);
		void ForgetSource(Entry &Value);
		void RetainSource(Entry &Value, const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels);
		void UpdateSource(Entry &Value, const RenderTextureUpdate &Update);
		void QueueMaterialViews(Entry &Value, std::uint8_t Requested);
		void QueueMaterial(const RenderMaterialState &Material);
		void QueueDeferredMaterialViews();
		bool EvictMaterialViews(std::uint32_t RequiredBytes, Entry &Except);
		[[nodiscard]] SDL_GPUTransferBuffer *GetMaterialTransfer(std::uint32_t Bytes, bool Reuse);
		void UploadMaterialView(Entry &Value, unsigned Index, SDL_GPUCopyPass *CopyPass, bool &UsedReusableTransfer);
		void DestroyEntry(Entry &Value);
		void DestroyResident();
		void TrimBaseUploadStaging();
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
		std::uint64_t MaterialViewEvictions = 0;
		std::uint64_t DeferredMaterialViewUploads = 0;
		std::uint64_t MaterialViewBudget = 128ull * 1024 * 1024;
		std::uint64_t MaterialEpoch = 0;
		std::vector<Entry *> PendingMaterialEntries;
		std::vector<SDL_GPUTransferBuffer *> MaterialStaging;
		SDL_GPUTransferBuffer *MaterialUploadBuffer = nullptr;
		std::uint32_t MaterialUploadCapacity = 0;
		std::uint64_t BaseUploadStagingBytes = 0;
		std::uint64_t BaseUploadStagingEvictions = 0;
		std::uint64_t BaseUploadSerial = 0;
		Entry White;
	};
}

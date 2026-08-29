#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	struct SDLRendererMetrics;

	struct SDLSkinPaletteBinding {
		SDL_GPUBuffer *Buffer = nullptr;
		std::uint64_t PoseRevision = 0;
		bool GpuSkinned = false;
	};

	class SDLSkinPaletteCache final {
	  public:
		explicit SDLSkinPaletteCache(
			SDL_GPUDevice *GpuDevice,
			SDLRendererMetrics *MetricsValue = nullptr,
			std::uint32_t *InjectedUploadFailuresValue = nullptr
		) : Gpu(GpuDevice), Metrics(MetricsValue), InjectedUploadFailures(InjectedUploadFailuresValue) {}

		SDLSkinPaletteCache(const SDLSkinPaletteCache &) = delete;
		SDLSkinPaletteCache &operator=(const SDLSkinPaletteCache &) = delete;

		void ApplyPublication(const RenderPublication &Publication);
		void Destroy();
		[[nodiscard]] SDLSkinPaletteBinding Find(
			ObjectId Object,
			const RenderAnimationPoseUpdate *Pose
		) const;
		[[nodiscard]] std::size_t GetGpuRigCount() const { return Rigs.size(); }

	  private:
		struct Entry {
			SDL_GPUBuffer *Buffer = nullptr;
			SDL_GPUTransferBuffer *TransferBuffer = nullptr;
			std::uint32_t Bytes = 0;
			std::uint64_t PoseRevision = 0;
			RenderSkeletonIdentity Skeleton;
			bool Identity = false;
		};
		struct PendingUpload {
			Entry *Destination = nullptr;
			const RenderSkinPaletteEntry *Entries = nullptr;
			bool Cycle = false;
		};

		SDL_GPUDevice *Gpu = nullptr;
		SDLRendererMetrics *Metrics = nullptr;
		std::uint32_t *InjectedUploadFailures = nullptr;
		std::optional<Entry> Identity;
		std::unordered_map<ObjectId, Entry> Rigs;
		std::vector<PendingUpload> PendingUploads;

		Entry CreateEntry(std::uint32_t Bytes, RenderSkeletonIdentity Skeleton, bool IsIdentity);
		void Release(Entry &Value);
		void DestroyRigs();
	};
}

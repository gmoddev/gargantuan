#pragma once

#include "render/sdl/SDLInstanceRecordCache.hpp"

#include <SDL3/SDL_gpu.h>

namespace gargantuan {
	struct SDLRendererMetrics;

	// Optional GPU backing for publication-derived instance values. The caller
	// records Upload before beginning a render pass, then acknowledges only an
	// actually submitted command buffer.
	class SDLInstanceGpuCache final {
	  public:
		SDLInstanceGpuCache(SDL_GPUDevice *GpuValue, float OutputMode);
		SDLInstanceGpuCache(const SDLInstanceGpuCache &) = delete;
		SDLInstanceGpuCache &operator=(const SDLInstanceGpuCache &) = delete;
		~SDLInstanceGpuCache();

		[[nodiscard]] bool Apply(const RenderPublication &Publication);
		void Upload(SDL_GPUCommandBuffer *Commands, SDLRendererMetrics *Metrics);
		void OnFrameSubmitted();
		void OnFrameAborted();
		void Destroy();
		[[nodiscard]] SDL_GPUBuffer *GetBuffer() const { return Buffer; }
		[[nodiscard]] std::optional<std::uint32_t> Find(ObjectId Object) const { return Records.Find(Object); }
		[[nodiscard]] const SDLInstanceRecordCache &GetRecords() const { return Records; }

	  private:
		SDL_GPUDevice *Gpu = nullptr;
		SDLInstanceRecordCache Records;
		SDL_GPUBuffer *Buffer = nullptr;
		SDL_GPUTransferBuffer *Transfer = nullptr;
		std::uint32_t Capacity = 0;
		bool PendingUpload = false;
	};

	// Compact visible-slot indirection. A static camera/list reuses the submitted
	// buffer while sparse record changes touch only SDLInstanceGpuCache storage.
	class SDLVisibleInstanceBuffer final {
	  public:
		explicit SDLVisibleInstanceBuffer(SDL_GPUDevice *GpuValue) : Gpu(GpuValue) {}
		SDLVisibleInstanceBuffer(const SDLVisibleInstanceBuffer &) = delete;
		SDLVisibleInstanceBuffer &operator=(const SDLVisibleInstanceBuffer &) = delete;
		~SDLVisibleInstanceBuffer();

		void BeginFrame() { Slots.clear(); }
		[[nodiscard]] std::uint32_t Append(std::uint32_t Slot) {
			const auto Index = static_cast<std::uint32_t>(Slots.size());
			Slots.push_back(Slot);
			return Index;
		}
		[[nodiscard]] std::size_t Size() const { return Slots.size(); }
		[[nodiscard]] SDL_GPUBuffer *GetBuffer() const { return Buffer; }
		void Upload(SDL_GPUCommandBuffer *Commands, SDLRendererMetrics *Metrics);
		void OnFrameSubmitted();
		void OnFrameAborted();
		void Destroy();

	  private:
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUBuffer *Buffer = nullptr;
		SDL_GPUTransferBuffer *Transfer = nullptr;
		std::uint32_t Capacity = 0;
		std::vector<std::uint32_t> Slots;
		std::vector<std::uint32_t> SubmittedSlots;
		bool PendingUpload = false;
		bool Valid = false;
	};
} // namespace gargantuan

#include "render/sdl/SDLInstanceGpuCache.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	SDLInstanceGpuCache::SDLInstanceGpuCache(SDL_GPUDevice *GpuValue, float OutputMode)
		: Gpu(GpuValue), Records(OutputMode) {}

	SDLInstanceGpuCache::~SDLInstanceGpuCache() { Destroy(); }

	bool SDLInstanceGpuCache::Apply(const RenderPublication &Publication) {
		return Records.Apply(Publication);
	}

	void SDLInstanceGpuCache::Destroy() {
		if (Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
		if (Buffer) SDL_ReleaseGPUBuffer(Gpu, Buffer);
		Transfer = nullptr;
		Buffer = nullptr;
		Capacity = 0;
		PendingUpload = false;
		Records.Clear();
	}

	void SDLInstanceGpuCache::Upload(SDL_GPUCommandBuffer *Commands, SDLRendererMetrics *Metrics) {
		if (!Records.IsAvailable() || Records.SlotCount() == 0) return;
		if (PendingUpload) throw std::logic_error("[Renderer:Instances] Previous upload has no submission outcome");
		const auto Required = Records.SlotCount() * sizeof(SDLInstanceRecord);
		if (Required > std::numeric_limits<std::uint32_t>::max())
			throw std::length_error("[Renderer:Instances] Instance bytes exceed SDL buffer limit");
		if (Required > Capacity) {
			const auto ReplacementCapacity = std::bit_ceil(static_cast<std::uint32_t>(Required));
			const SDL_GPUBufferCreateInfo BufferInfo{
				.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
				.size = ReplacementCapacity,
			};
			const SDL_GPUTransferBufferCreateInfo TransferInfo{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
				.size = ReplacementCapacity,
			};
			auto *NewBuffer = SDL_CreateGPUBuffer(Gpu, &BufferInfo);
			auto *NewTransfer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
			if (!NewBuffer || !NewTransfer) {
				if (NewBuffer) SDL_ReleaseGPUBuffer(Gpu, NewBuffer);
				if (NewTransfer) SDL_ReleaseGPUTransferBuffer(Gpu, NewTransfer);
				throw std::runtime_error(std::format("[Renderer:Instances] Could not allocate instance buffers: {}", SDL_GetError()));
			}
			if (Buffer) SDL_ReleaseGPUBuffer(Gpu, Buffer);
			if (Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
			Buffer = NewBuffer;
			Transfer = NewTransfer;
			Capacity = ReplacementCapacity;
			Records.MarkFullUpload();
			if (Metrics) {
				++Metrics->BufferReallocations;
				++Metrics->TransferBufferCreations;
			}
		}
		const auto Plan = Records.PlanUpload();
		if (Plan.Bytes == 0) return;
		auto *Mapped = static_cast<std::byte *>(SDL_MapGPUTransferBuffer(Gpu, Transfer, true));
		if (!Mapped) throw std::runtime_error(std::format("[Renderer:Instances] Could not map instance transfer: {}", SDL_GetError()));
		const SDLInstanceRecord Zero{};
		for (const auto &Range : Plan.Ranges) for (std::uint32_t Slot = Range.First; Slot < Range.First + Range.Count; ++Slot) {
			const auto *Record = Records.Get(Slot);
			std::memcpy(Mapped + static_cast<std::size_t>(Slot) * sizeof(SDLInstanceRecord),
				Record ? Record : &Zero, sizeof(SDLInstanceRecord));
		}
		SDL_UnmapGPUTransferBuffer(Gpu, Transfer);
		auto *Pass = SDL_BeginGPUCopyPass(Commands);
		if (!Pass) throw std::runtime_error(std::format("[Renderer:Instances] Could not begin instance upload: {}", SDL_GetError()));
		for (const auto &Range : Plan.Ranges) {
			const auto Offset = Range.First * static_cast<std::uint32_t>(sizeof(SDLInstanceRecord));
			const auto Bytes = Range.Count * static_cast<std::uint32_t>(sizeof(SDLInstanceRecord));
			const SDL_GPUTransferBufferLocation Source{.transfer_buffer = Transfer, .offset = Offset};
			const SDL_GPUBufferRegion Destination{.buffer = Buffer, .offset = Offset, .size = Bytes};
			// Preserve untouched slots. Cycling a partially updated device buffer
			// would leave those slots unspecified on the new backing.
			SDL_UploadToGPUBuffer(Pass, &Source, &Destination, false);
			if (Metrics) {
				++Metrics->UploadOperations;
				Metrics->UploadedBytes += Bytes;
				Metrics->InstanceUploadBytes += Bytes;
			}
		}
		SDL_EndGPUCopyPass(Pass);
		PendingUpload = true;
	}

	void SDLInstanceGpuCache::OnFrameSubmitted() {
		if (PendingUpload) Records.MarkUploaded();
		PendingUpload = false;
	}

	void SDLInstanceGpuCache::OnFrameAborted() {
		if (PendingUpload) Records.MarkFullUpload();
		PendingUpload = false;
	}

	SDLVisibleInstanceBuffer::~SDLVisibleInstanceBuffer() { Destroy(); }

	void SDLVisibleInstanceBuffer::Destroy() {
		if (Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
		if (Buffer) SDL_ReleaseGPUBuffer(Gpu, Buffer);
		Transfer = nullptr;
		Buffer = nullptr;
		Capacity = 0;
		Slots.clear();
		SubmittedSlots.clear();
		PendingUpload = false;
		Valid = false;
	}

	void SDLVisibleInstanceBuffer::Upload(SDL_GPUCommandBuffer *Commands, SDLRendererMetrics *Metrics) {
		if (PendingUpload) throw std::logic_error("[Renderer:Instances] Visible upload has no submission outcome");
		if (Slots.empty()) return;
		if (Slots.size() > SDLInstanceRecordCache::MaximumRecords)
			throw std::length_error("[Renderer:Instances] Visible instance count exceeds the bounded cache");
		const auto Required = static_cast<std::uint32_t>(Slots.size() * sizeof(std::uint32_t));
		if (Required > Capacity) {
			const auto ReplacementCapacity = std::bit_ceil(Required);
			const SDL_GPUBufferCreateInfo BufferInfo{
				.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = ReplacementCapacity};
			const SDL_GPUTransferBufferCreateInfo TransferInfo{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = ReplacementCapacity};
			auto *NewBuffer = SDL_CreateGPUBuffer(Gpu, &BufferInfo);
			auto *NewTransfer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
			if (!NewBuffer || !NewTransfer) {
				if (NewBuffer) SDL_ReleaseGPUBuffer(Gpu, NewBuffer);
				if (NewTransfer) SDL_ReleaseGPUTransferBuffer(Gpu, NewTransfer);
				throw std::runtime_error(std::format("[Renderer:Instances] Could not allocate visible buffer: {}", SDL_GetError()));
			}
			if (Buffer) SDL_ReleaseGPUBuffer(Gpu, Buffer);
			if (Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
			Buffer = NewBuffer;
			Transfer = NewTransfer;
			Capacity = ReplacementCapacity;
			Valid = false;
			if (Metrics) {
				++Metrics->BufferReallocations;
				++Metrics->TransferBufferCreations;
			}
		}
		if (Valid && Slots == SubmittedSlots) return;
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Transfer, true);
		if (!Mapped) throw std::runtime_error(std::format("[Renderer:Instances] Could not map visible transfer: {}", SDL_GetError()));
		std::memcpy(Mapped, Slots.data(), Required);
		SDL_UnmapGPUTransferBuffer(Gpu, Transfer);
		auto *Pass = SDL_BeginGPUCopyPass(Commands);
		if (!Pass) throw std::runtime_error(std::format("[Renderer:Instances] Could not begin visible upload: {}", SDL_GetError()));
		const SDL_GPUTransferBufferLocation Source{.transfer_buffer = Transfer};
		const SDL_GPUBufferRegion Destination{.buffer = Buffer, .size = Required};
		SDL_UploadToGPUBuffer(Pass, &Source, &Destination, false);
		SDL_EndGPUCopyPass(Pass);
		PendingUpload = true;
		if (Metrics) {
			++Metrics->UploadOperations;
			Metrics->UploadedBytes += Required;
			Metrics->VisibleIndexUploadBytes += Required;
		}
	}

	void SDLVisibleInstanceBuffer::OnFrameSubmitted() {
		if (PendingUpload) {
			SubmittedSlots = Slots;
			Valid = true;
		}
		PendingUpload = false;
	}

	void SDLVisibleInstanceBuffer::OnFrameAborted() {
		PendingUpload = false;
		Valid = false;
	}
} // namespace gargantuan

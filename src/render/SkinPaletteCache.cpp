#include "render/sdl/SDLSkinPaletteCache.hpp"

#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gargantuan {
	SDLSkinPaletteCache::Entry SDLSkinPaletteCache::CreateEntry(
		std::uint32_t Bytes,
		RenderSkeletonIdentity Skeleton,
		bool IsIdentity
	) {
		Entry Result{.Bytes = Bytes, .Skeleton = Skeleton, .Identity = IsIdentity};
		SDL_GPUBufferCreateInfo BufferInfo{
			.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
			.size = Bytes,
		};
		Result.Buffer = SDL_CreateGPUBuffer(Gpu, &BufferInfo);
		SDL_GPUTransferBufferCreateInfo TransferInfo{
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = Bytes,
		};
		Result.TransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
		if (!Result.Buffer || !Result.TransferBuffer) {
			Release(Result);
			throw std::runtime_error(std::format("Failed to create skin palette resources: {}", SDL_GetError()));
		}
		if (Metrics && !IsIdentity) {
			++Metrics->PaletteBufferCreations;
			++Metrics->PaletteTransferBufferCreations;
		}
		return Result;
	}

	void SDLSkinPaletteCache::Release(Entry &Value) {
		if (!Gpu) return;
		if (Value.TransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, Value.TransferBuffer);
		if (Value.Buffer) SDL_ReleaseGPUBuffer(Gpu, Value.Buffer);
		if (Metrics && !Value.Identity && (Value.TransferBuffer || Value.Buffer))
			++Metrics->PaletteResourceReleases;
		Value = {};
	}

	void SDLSkinPaletteCache::DestroyRigs() {
		for (auto &[Object, Value] : Rigs) {
			(void)Object;
			Release(Value);
		}
		Rigs.clear();
	}

	void SDLSkinPaletteCache::Destroy() {
		DestroyRigs();
		if (Identity) {
			Release(*Identity);
			Identity.reset();
		}
		PendingUploads.clear();
	}

	void SDLSkinPaletteCache::ApplyPublication(const RenderPublication &Publication) {
		if (!Gpu) throw std::logic_error("Cannot apply skin palettes without an SDL GPU device");
		if (Publication.FullResync) DestroyRigs();
		for (const auto &Remove : Publication.AnimationPoseRemoves) {
			const auto Existing = Rigs.find(Remove.Object);
			if (Existing == Rigs.end()) continue;
			Release(Existing->second);
			Rigs.erase(Existing);
		}

		PendingUploads.clear();
		const auto RequiredUploads = Publication.AnimationPoseUpdates.size() + (Identity ? 0u : 1u);
		if (PendingUploads.capacity() < RequiredUploads) {
			PendingUploads.reserve(RequiredUploads);
			if (Metrics) ++Metrics->PaletteScratchAllocations;
		}
		Rigs.reserve(Rigs.size() + Publication.AnimationPoseUpdates.size());

		const RenderSkinPaletteEntry IdentityEntry{};
		if (!Identity) {
			Identity.emplace(CreateEntry(sizeof(RenderSkinPaletteEntry), {}, true));
			PendingUploads.push_back({&*Identity, &IdentityEntry, false});
		}

		for (const auto &Update : Publication.AnimationPoseUpdates) {
			if (Update.Mode == RenderAnimationSkinningMode::CpuFallback) {
				const auto Existing = Rigs.find(Update.Object);
				if (Existing != Rigs.end()) {
					Release(Existing->second);
					Rigs.erase(Existing);
					if (Metrics) ++Metrics->FallbackTransitions;
				}
				continue;
			}
			const auto Bytes64 = Update.Palette.Entries->size() * sizeof(RenderSkinPaletteEntry);
			if (Bytes64 > std::numeric_limits<std::uint32_t>::max())
				throw std::length_error("Skin palette exceeds the SDL GPU buffer size limit");
			const auto Bytes = static_cast<std::uint32_t>(Bytes64);
			auto Existing = Rigs.find(Update.Object);
			if (Existing == Rigs.end()) {
				Existing = Rigs.emplace(Update.Object,
					CreateEntry(Bytes, Update.Palette.Skeleton, false)).first;
			} else if (Existing->second.Bytes != Bytes ||
				Existing->second.Skeleton != Update.Palette.Skeleton) {
				throw std::logic_error("Skin palette topology changed without a full resync");
			}
			const bool Cycle = Existing->second.PoseRevision != 0;
			Existing->second.PoseRevision = Update.PoseRevision;
			PendingUploads.push_back({Existing->second.Buffer ? &Existing->second : nullptr,
				Update.Palette.Entries->data(), Cycle});
		}

		if (PendingUploads.empty()) return;
		if (InjectedUploadFailures && *InjectedUploadFailures > 0) {
			--*InjectedUploadFailures;
			Destroy();
			throw std::runtime_error("[Render:Skinning] injected palette upload failure");
		}
		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) {
			Destroy();
			throw std::runtime_error(std::format("Failed to acquire skin palette commands: {}", SDL_GetError()));
		}
		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		if (!CopyPass) {
			SDL_CancelGPUCommandBuffer(Commands);
			Destroy();
			throw std::runtime_error(std::format("Failed to begin skin palette upload: {}", SDL_GetError()));
		}
		bool CopyPassOpen = true;
		bool SubmitAttempted = false;
		try {
			std::uint64_t AnimationUploadCount = 0;
			std::uint64_t AnimationUploadBytes = 0;
			std::uint64_t TotalUploadBytes = 0;
			for (const auto &Upload : PendingUploads) {
				if (!Upload.Destination || !Upload.Destination->Buffer || !Upload.Destination->TransferBuffer)
					throw std::logic_error("Skin palette upload references an incomplete resource");
				auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Upload.Destination->TransferBuffer, Upload.Cycle);
				if (!Mapped)
					throw std::runtime_error(std::format("Failed to map skin palette transfer buffer: {}", SDL_GetError()));
				std::memcpy(Mapped, Upload.Entries, Upload.Destination->Bytes);
				SDL_UnmapGPUTransferBuffer(Gpu, Upload.Destination->TransferBuffer);
				SDL_GPUTransferBufferLocation Source{.transfer_buffer = Upload.Destination->TransferBuffer};
				SDL_GPUBufferRegion Destination{
					.buffer = Upload.Destination->Buffer,
					.size = Upload.Destination->Bytes,
				};
				SDL_UploadToGPUBuffer(CopyPass, &Source, &Destination, Upload.Cycle);
				TotalUploadBytes += Upload.Destination->Bytes;
				if (!Upload.Destination->Identity) {
					++AnimationUploadCount;
					AnimationUploadBytes += Upload.Destination->Bytes;
				}
			}
			SDL_EndGPUCopyPass(CopyPass);
			CopyPassOpen = false;
			SubmitAttempted = true;
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to submit skin palette upload: {}", SDL_GetError()));
			if (Metrics) {
				Metrics->PaletteUploads += AnimationUploadCount;
				Metrics->PaletteUploadBytes += AnimationUploadBytes;
				Metrics->UploadOperations += PendingUploads.size();
				Metrics->UploadedBytes += TotalUploadBytes;
			}
		} catch (...) {
			if (CopyPassOpen) SDL_EndGPUCopyPass(CopyPass);
			if (!SubmitAttempted) SDL_CancelGPUCommandBuffer(Commands);
			Destroy();
			throw;
		}
	}

	SDLSkinPaletteBinding SDLSkinPaletteCache::Find(
		ObjectId Object,
		const RenderAnimationPoseUpdate *Pose
	) const {
		if (!Identity || !Identity->Buffer)
			throw std::logic_error("SDL skin palette identity resource is unavailable");
		if (!Pose || Pose->Mode == RenderAnimationSkinningMode::CpuFallback)
			return {Identity->Buffer, 0, false};
		const auto Existing = Rigs.find(Object);
		if (Existing == Rigs.end() || !Existing->second.Buffer ||
			Existing->second.PoseRevision != Pose->PoseRevision ||
			Existing->second.Skeleton != Pose->Palette.Skeleton)
			throw std::logic_error("SDL skin palette residency is incoherent with the render projection");
		return {Existing->second.Buffer, Existing->second.PoseRevision, true};
	}
}

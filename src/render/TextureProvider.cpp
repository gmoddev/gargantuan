#include "render/sdl/SDLTextureCache.hpp"

#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	void SDLTextureCache::DestroyEntry(Entry &Value) {
		if (Value.Texture) {
			SDL_ReleaseGPUTexture(Gpu, Value.Texture);
			Value.Texture = nullptr;
			if (Metrics) ++Metrics->TextureReleases;
		}
		if (Value.TransferBuffer) {
			SDL_ReleaseGPUTransferBuffer(Gpu, Value.TransferBuffer);
			Value.TransferBuffer = nullptr;
		}
	}

	void SDLTextureCache::Destroy() {
		if (!Gpu) return;
		for (auto &[Texture, Value] : Textures) {
			(void)Texture;
			DestroyEntry(Value);
		}
		Textures.clear();
	}

	void SDLTextureCache::Upload(
		Entry &Value,
		SDL_GPUCopyPass *CopyPass,
		std::uint32_t X,
		std::uint32_t Y,
		std::uint32_t Width,
		std::uint32_t Height,
		const std::vector<std::uint8_t> &Pixels,
		bool Initial
	) {
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Value.TransferBuffer, !Initial);
		if (!Mapped) throw std::runtime_error(std::format("Failed to map texture transfer buffer: {}", SDL_GetError()));
		std::memcpy(Mapped, Pixels.data(), Pixels.size());
		SDL_UnmapGPUTransferBuffer(Gpu, Value.TransferBuffer);

		SDL_GPUTextureTransferInfo Source{
			.transfer_buffer = Value.TransferBuffer,
			.offset = 0,
			.pixels_per_row = Width,
			.rows_per_layer = Height,
		};
		SDL_GPUTextureRegion Destination{
			.texture = Value.Texture,
			.x = X,
			.y = Y,
			.w = Width,
			.h = Height,
			.d = 1,
		};
		const bool FullReplacement = !Initial && X == 0 && Y == 0 && Width == Value.Width && Height == Value.Height;
		SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, FullReplacement);
		if (Metrics) {
			++Metrics->UploadOperations;
			Metrics->UploadedBytes += Pixels.size();
			if (!Initial) {
				++Metrics->TextureUpdates;
				++Metrics->BufferCycleRequests;
				if (FullReplacement) ++Metrics->BufferCycleRequests;
			}
		}
	}

	void SDLTextureCache::ApplyPublication(const RenderPublication &Publication) {
		if (!Gpu) throw std::logic_error("Cannot apply textures without an SDL GPU device");
		if (Publication.FullResync) Destroy();
		for (const auto &Remove : Publication.TextureRemoves) {
			auto Found = Textures.find(Remove.Texture);
			if (Found == Textures.end()) continue;
			DestroyEntry(Found->second);
			Textures.erase(Found);
		}
		if (Publication.TextureCreates.empty() && Publication.TextureUpdates.empty()) return;

		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) throw std::runtime_error(std::format("Failed to acquire texture upload commands: {}", SDL_GetError()));
		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		if (!CopyPass) {
			SDL_CancelGPUCommandBuffer(Commands);
			throw std::runtime_error(std::format("Failed to begin texture upload: {}", SDL_GetError()));
		}
		bool CopyPassOpen = true;
		bool SubmitAttempted = false;
		try {
			for (const auto &Create : Publication.TextureCreates) {
				const auto ByteCount = static_cast<std::uint64_t>(Create.Width) * Create.Height * 4u;
				if (ByteCount > std::numeric_limits<std::uint32_t>::max())
					throw std::length_error("Texture exceeds the SDL GPU transfer-buffer size limit");
				SDL_GPUTextureCreateInfo TextureInfo{
					.type = SDL_GPU_TEXTURETYPE_2D,
					.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
					.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
					.width = Create.Width,
					.height = Create.Height,
					.layer_count_or_depth = 1,
					.num_levels = 1,
				};
				Entry Value{
					.Texture = SDL_CreateGPUTexture(Gpu, &TextureInfo),
					.Revision = Create.Revision,
					.Width = Create.Width,
					.Height = Create.Height,
				};
				if (!Value.Texture)
					throw std::runtime_error(std::format("Failed to create resident texture: {}", SDL_GetError()));
				if (Metrics) ++Metrics->TextureCreations;
				SDL_GPUTransferBufferCreateInfo TransferInfo{
					.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
					.size = static_cast<std::uint32_t>(ByteCount),
				};
				Value.TransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
				if (!Value.TransferBuffer) {
					DestroyEntry(Value);
					throw std::runtime_error(std::format("Failed to create texture transfer buffer: {}", SDL_GetError()));
				}
				if (Metrics) ++Metrics->TransferBufferCreations;
				try {
					Upload(Value, CopyPass, 0, 0, Create.Width, Create.Height, *Create.Pixels, true);
					const auto [Position, Inserted] = Textures.emplace(Create.Texture, Value);
					(void)Position;
					if (!Inserted) throw std::logic_error("Texture publication attempted duplicate SDL residency");
				} catch (...) {
					DestroyEntry(Value);
					throw;
				}
			}
			for (const auto &Update : Publication.TextureUpdates) {
				auto &Value = Textures.at(Update.Texture);
				Upload(Value, CopyPass, Update.X, Update.Y, Update.Width, Update.Height, *Update.Pixels, false);
				Value.Revision = Update.Revision;
			}
			SDL_EndGPUCopyPass(CopyPass);
			CopyPassOpen = false;
			SubmitAttempted = true;
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to submit texture upload: {}", SDL_GetError()));
		} catch (...) {
			if (CopyPassOpen) SDL_EndGPUCopyPass(CopyPass);
			if (!SubmitAttempted) SDL_CancelGPUCommandBuffer(Commands);
			Destroy();
			throw;
		}
	}

	SDL_GPUTexture *SDLTextureCache::Find(RenderTextureIdentity Texture) const {
		const auto Found = Textures.find(Texture);
		return Found == Textures.end() ? nullptr : Found->second.Texture;
	}
}

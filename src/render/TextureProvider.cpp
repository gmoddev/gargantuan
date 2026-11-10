#include "render/sdl/SDLTextureCache.hpp"

#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	SDLTextureCache::Entry SDLTextureCache::CreateEntry(
		std::uint32_t Width, std::uint32_t Height, std::uint64_t Revision
	) {
		const auto PixelCount = static_cast<std::uint64_t>(Width) * Height;
		if (PixelCount > std::numeric_limits<std::uint32_t>::max() / 4u)
			throw std::length_error("Texture exceeds the SDL GPU transfer-buffer size limit");
		const SDL_GPUTextureCreateInfo TextureInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = Width,
			.height = Height,
			.layer_count_or_depth = 1,
			.num_levels = 1,
		};
		Entry Value{
			.Texture = SDL_CreateGPUTexture(Gpu, &TextureInfo),
			.Revision = Revision,
			.Width = Width,
			.Height = Height,
		};
		if (!Value.Texture)
			throw std::runtime_error(std::format("Failed to create resident texture: {}", SDL_GetError()));
		if (Metrics) ++Metrics->TextureCreations;
		const SDL_GPUTransferBufferCreateInfo TransferInfo{
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = static_cast<std::uint32_t>(PixelCount * 4u),
		};
		Value.TransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
		if (!Value.TransferBuffer) {
			DestroyEntry(Value);
			throw std::runtime_error(std::format("Failed to create texture transfer buffer: {}", SDL_GetError()));
		}
		if (Metrics) ++Metrics->TransferBufferCreations;
		return Value;
	}

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
		DestroyResident();
		DestroyEntry(White);
	}

	void SDLTextureCache::DestroyResident() {
		for (auto &[Texture, Value] : Textures) {
			(void)Texture;
			DestroyEntry(Value);
		}
		Textures.clear();
		++ResidencyGeneration;
	}

	SDL_GPUTexture *SDLTextureCache::GetWhiteTexture() {
		if (White.Texture) return White.Texture;
		static const std::vector<std::uint8_t> Pixels{255, 255, 255, 255};
		auto Value = CreateEntry(1, 1, 0);
		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) {
			DestroyEntry(Value);
			throw std::runtime_error(std::format("Failed to acquire white texture commands: {}", SDL_GetError()));
		}
		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		try {
			if (!CopyPass)
				throw std::runtime_error(std::format("Failed to begin white texture upload: {}", SDL_GetError()));
			Upload(Value, CopyPass, 0, 0, 1, 1, Pixels, true);
		} catch (...) {
			if (CopyPass) SDL_EndGPUCopyPass(CopyPass);
			SDL_CancelGPUCommandBuffer(Commands);
			DestroyEntry(Value);
			throw;
		}
		SDL_EndGPUCopyPass(CopyPass);
		if (!SDL_SubmitGPUCommandBuffer(Commands)) {
			DestroyEntry(Value);
			throw std::runtime_error(std::format("Failed to submit white texture upload: {}", SDL_GetError()));
		}
		White = Value;
		return White.Texture;
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
		if (Publication.FullResync) DestroyResident();
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
				auto Value = CreateEntry(Create.Width, Create.Height, Create.Revision);
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
			DestroyResident();
			throw;
		}
	}

	SDL_GPUTexture *SDLTextureCache::Find(RenderTextureIdentity Texture) const {
		const auto Found = Textures.find(Texture);
		return Found == Textures.end() ? nullptr : Found->second.Texture;
	}
}

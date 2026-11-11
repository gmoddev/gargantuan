#include "render/sdl/SDLTextureCache.hpp"
#include "render/sdl/SDLMaterialMips.hpp"

#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		constexpr std::uint64_t MATERIAL_SOURCE_BUDGET = 128ull * 1024 * 1024;
		constexpr std::uint32_t MATERIAL_CHAIN_LIMIT = 8u * 1024 * 1024;
		constexpr std::uint8_t LINEAR_VIEW = 1u;
		constexpr std::uint8_t SRGB_VIEW = 2u;
	}
	SDLTextureCache::Entry SDLTextureCache::CreateEntry(
		std::uint32_t Width, std::uint32_t Height, std::uint64_t Revision, RenderTextureFormat Format
	) {
		const auto PixelCount = static_cast<std::uint64_t>(Width) * Height;
		const auto PixelBytes = GetRenderTexturePixelBytes(Format);
		if (PixelBytes == 0 || PixelCount > std::numeric_limits<std::uint32_t>::max() / PixelBytes)
			throw std::length_error("Texture exceeds the SDL GPU transfer-buffer size limit");
		const auto NativeFormat = Format == RenderTextureFormat::Rgba32Float
			? SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		if (!SDL_GPUTextureSupportsFormat(Gpu, NativeFormat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
			throw std::runtime_error("[Render:Texture] Required source texture format is unavailable");
		const SDL_GPUTextureCreateInfo TextureInfo{
			.type = SDL_GPU_TEXTURETYPE_2D,
			.format = NativeFormat,
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
			.Format = Format,
		};
		if (!Value.Texture)
			throw std::runtime_error(std::format("Failed to create resident texture: {}", SDL_GetError()));
		if (Metrics) ++Metrics->TextureCreations;
		const SDL_GPUTransferBufferCreateInfo TransferInfo{
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = static_cast<std::uint32_t>(PixelCount * PixelBytes),
		};
		Value.TransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
		if (!Value.TransferBuffer) {
			DestroyEntry(Value);
			throw std::runtime_error(std::format("Failed to create texture transfer buffer: {}", SDL_GetError()));
		}
		BaseUploadStagingBytes += static_cast<std::uint32_t>(PixelCount * PixelBytes);
		if (Metrics) ++Metrics->TransferBufferCreations;
		return Value;
	}

	void SDLTextureCache::DestroyView(MaterialView &View) {
		if (View.Texture) {
			SDL_ReleaseGPUTexture(Gpu, View.Texture);
			MaterialViewBytes -= View.Bytes;
			if (Metrics) ++Metrics->TextureReleases;
		}
		View = {};
	}

	void SDLTextureCache::ForgetSource(Entry &Value) {
		if (const auto *Source = Value.GetSource()) MaterialSourceBytes -= Source->size();
		Value.Source.reset();
		Value.PatchedSource.reset();
	}

	void SDLTextureCache::RetainSource(Entry &Value, const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels) {
		if (Value.Format != RenderTextureFormat::Rgba8Unorm) return;
		if (Pixels->size() > MATERIAL_CHAIN_LIMIT || Pixels->size() > MATERIAL_SOURCE_BUDGET - MaterialSourceBytes)
			return;
		Value.Source = Pixels;
		MaterialSourceBytes += Pixels->size();
	}

	void SDLTextureCache::UpdateSource(Entry &Value, const RenderTextureUpdate &Update) {
		if (Update.X == 0 && Update.Y == 0 && Update.Width == Value.Width && Update.Height == Value.Height) {
			ForgetSource(Value);
			RetainSource(Value, Update.Pixels);
			return;
		}
		if (!Value.GetSource()) return;
		if (!Value.PatchedSource) {
			try {
				Value.PatchedSource = std::make_unique<std::vector<std::uint8_t>>(*Value.Source);
			} catch (const std::bad_alloc &) {
				ForgetSource(Value);
				return;
			}
			Value.Source.reset();
		}
		const auto PatchStride = static_cast<std::size_t>(Update.Width) * 4;
		for (std::size_t Y = 0; Y < Update.Height; ++Y) {
			const auto Offset = ((Update.Y + Y) * Value.Width + Update.X) * 4;
			std::memcpy(Value.PatchedSource->data() + Offset, Update.Pixels->data() + Y * PatchStride, PatchStride);
		}
	}

	void SDLTextureCache::QueueMaterialViews(Entry &Value, std::uint8_t Requested) {
		if (Value.Format != RenderTextureFormat::Rgba8Unorm) return;
		Value.RequestedViews |= Requested;
		for (unsigned Index = 0; Index < Value.Views.size(); ++Index) {
			const auto Flag = static_cast<std::uint8_t>(1u << Index);
			if (!(Requested & Flag) || (Value.PendingViews & Flag)) continue;
			auto &View = Value.Views[Index];
			if (View.AttemptedRevision == Value.Revision || (Index == 0 && Value.Width == 1 && Value.Height == 1)) continue;
			const auto Fallback = [&] {
				DestroyView(View);
				View.AttemptedRevision = Value.Revision;
				++MaterialViewFallbacks;
			};
			if (!Value.GetSource()) {
				Fallback();
				continue;
			}
			if (!Value.PendingViews) {
				try {
					PendingMaterialEntries.push_back(&Value);
				} catch (const std::bad_alloc &) {
					Fallback();
					continue;
				}
			}
			Value.PendingViews |= Flag;
		}
	}

	void SDLTextureCache::QueueMaterial(const RenderMaterialState &Material) {
		const auto Queue = [&](const auto &Texture, std::uint8_t View) {
			if (!Texture) return;
			const auto Found = Textures.find(*Texture);
			if (Found != Textures.end()) QueueMaterialViews(Found->second, View);
		};
		const auto QueueData = [&](const auto &Texture, RenderMaterialTextureSlot Slot) {
			const auto Filter = Material.TextureSamplers[static_cast<std::size_t>(Slot)].MinFilter;
			if (Filter != RenderTextureMinFilter::Nearest && Filter != RenderTextureMinFilter::Linear)
				Queue(Texture, LINEAR_VIEW);
		};
		if (Material.ShadingModel != RenderShadingModel::MetallicRoughness) {
			if (Material.OpacityMode != RenderOpacityMode::Masked) QueueData(Material.BaseColorTexture, RenderMaterialTextureSlot::BaseColor);
			return;
		}
		Queue(Material.BaseColorTexture, SRGB_VIEW);
		Queue(Material.EmissiveTexture, SRGB_VIEW);
		QueueData(Material.NormalTexture, RenderMaterialTextureSlot::Normal);
		QueueData(Material.MetallicRoughnessTexture, RenderMaterialTextureSlot::MetallicRoughness);
		QueueData(Material.OcclusionTexture, RenderMaterialTextureSlot::Occlusion);
	}

	void SDLTextureCache::QueueDeferredMaterialViews() {
		struct Demand {
			RenderTextureIdentity Identity;
			Entry *Value;
		};
		std::vector<Demand> Ordered;
		try {
			Ordered.reserve(Textures.size());
			for (auto &[Identity, Value] : Textures)
				if (Value.DeferredViews) Ordered.push_back({Identity, &Value});
		} catch (const std::bad_alloc &) {
			++MaterialViewFallbacks;
			return;
		}
		std::ranges::sort(Ordered, [](const Demand &Left, const Demand &Right) {
			if (Left.Value->LastDemandEpoch != Right.Value->LastDemandEpoch)
				return Left.Value->LastDemandEpoch > Right.Value->LastDemandEpoch;
			return Left.Identity < Right.Identity;
		});
		for (const auto &DemandValue : Ordered) {
			const auto Requested = std::exchange(DemandValue.Value->DeferredViews, 0);
			QueueMaterialViews(*DemandValue.Value, Requested);
		}
	}

	bool SDLTextureCache::EvictMaterialViews(std::uint32_t RequiredBytes, Entry &Except) {
		if (RequiredBytes > MaterialViewBudget) return false;
		while (MaterialViewBytes > MaterialViewBudget - RequiredBytes) {
			MaterialView *Oldest = nullptr;
			RenderTextureIdentity OldestId{};
			for (auto &[Identity, Value] : Textures) {
				if (&Value == &Except) continue;
				for (unsigned Index = 0; Index < Value.Views.size(); ++Index) {
					auto &View = Value.Views[Index];
					if (!View.Texture || (Value.PendingViews & (1u << Index)) ||
						MaterialEpoch <= 1 || View.LastUsedEpoch >= MaterialEpoch - 1) continue;
					if (!Oldest || View.LastUsedEpoch < Oldest->LastUsedEpoch ||
						(View.LastUsedEpoch == Oldest->LastUsedEpoch && Identity < OldestId)) {
						Oldest = &View;
						OldestId = Identity;
					}
				}
			}
			if (!Oldest) return false;
			DestroyView(*Oldest);
			++MaterialViewEvictions;
		}
		return true;
	}

	SDL_GPUTransferBuffer *SDLTextureCache::GetMaterialTransfer(std::uint32_t Bytes, bool Reuse) {
		if (Reuse && MaterialUploadCapacity >= Bytes) return MaterialUploadBuffer;
		const auto Capacity = Reuse ? std::bit_ceil(Bytes) : Bytes;
		const SDL_GPUTransferBufferCreateInfo Info{
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = Capacity,
		};
		auto *Transfer = SDL_CreateGPUTransferBuffer(Gpu, &Info);
		if (!Transfer) return nullptr;
		if (Metrics) ++Metrics->TransferBufferCreations;
		if (Reuse) {
			if (MaterialUploadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, MaterialUploadBuffer);
			MaterialUploadBuffer = Transfer;
			MaterialUploadCapacity = Capacity;
		} else {
			try {
				MaterialStaging.push_back(Transfer);
			} catch (const std::bad_alloc &) {
				SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
				return nullptr;
			}
		}
		return Transfer;
	}

	void SDLTextureCache::UploadMaterialView(Entry &Value, unsigned Index, SDL_GPUCopyPass *CopyPass,
		bool &UsedReusableTransfer) {
		auto &View = Value.Views[Index];
		const auto Fallback = [&] {
			DestroyView(View);
			View.AttemptedRevision = Value.Revision;
			++MaterialViewFallbacks;
		};
		const auto Layout = GetMaterialMipLayout(Value.Width, Value.Height, MATERIAL_CHAIN_LIMIT);
		if (!Layout || Layout->Bytes > MaterialViewBudget) {
			Fallback();
			return;
		}
		const auto Needed = Layout->Bytes > View.Bytes ? Layout->Bytes - View.Bytes : 0u;
		if (!EvictMaterialViews(Needed, Value)) {
			DestroyView(View);
			View.AttemptedRevision = 0;
			++MaterialViewFallbacks;
			return;
		}
		const bool Srgb = Index != 0;
		const bool Refresh = View.Texture != nullptr;
		if (!Refresh) {
			const auto Format = Srgb ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			if (!SDL_GPUTextureSupportsFormat(Gpu, Format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
				Fallback();
				return;
			}
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_2D, .format = Format, .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = Value.Width, .height = Value.Height, .layer_count_or_depth = 1, .num_levels = Layout->Count,
			};
			View.Texture = SDL_CreateGPUTexture(Gpu, &Info);
			if (!View.Texture) {
				Fallback();
				return;
			}
			View.Bytes = Layout->Bytes;
			View.Levels = Layout->Count;
			MaterialViewBytes += View.Bytes;
			if (Metrics) ++Metrics->TextureCreations;
		}
		// Limit retained cycling backings to one refresh per publication. First
		// admission and bulk updates release their temporary staging after submission.
		const bool Reuse = Refresh && !UsedReusableTransfer;
		auto *Transfer = GetMaterialTransfer(Layout->Bytes, Reuse);
		if (!Transfer) {
			Fallback();
			return;
		}
		UsedReusableTransfer |= Reuse;
		auto *Mapped = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, Transfer, Reuse));
		if (!Mapped) {
			Fallback();
			return;
		}
		const bool Built = BuildMaterialMipPixels({Mapped, Layout->Bytes}, *Value.GetSource(), *Layout, Srgb);
		SDL_UnmapGPUTransferBuffer(Gpu, Transfer);
		if (!Built) {
			Fallback();
			return;
		}
		for (std::uint32_t Level = 0; Level < Layout->Count; ++Level) {
			const auto &Mip = Layout->Levels[Level];
			const SDL_GPUTextureTransferInfo Source{.transfer_buffer = Transfer, .offset = Mip.Offset,
				.pixels_per_row = Mip.Width, .rows_per_layer = Mip.Height};
			const SDL_GPUTextureRegion Destination{.texture = View.Texture, .mip_level = Level,
				.w = Mip.Width, .h = Mip.Height, .d = 1};
			// Cycle once for a complete replacement. Later levels populate that same backing.
			SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, Refresh && Level == 0);
		}
		View.AttemptedRevision = Value.Revision;
		if (Metrics) {
			Metrics->UploadOperations += Layout->Count;
			Metrics->UploadedBytes += Layout->Bytes;
			if (Refresh) {
				++Metrics->TextureUpdates;
				++Metrics->BufferCycleRequests;
			}
		}
	}

	void SDLTextureCache::DestroyEntry(Entry &Value) {
		for (auto &View : Value.Views) DestroyView(View);
		ForgetSource(Value);
		if (Value.Texture) {
			SDL_ReleaseGPUTexture(Gpu, Value.Texture);
			Value.Texture = nullptr;
			if (Metrics) ++Metrics->TextureReleases;
		}
		if (Value.TransferBuffer) {
			SDL_ReleaseGPUTransferBuffer(Gpu, Value.TransferBuffer);
			Value.TransferBuffer = nullptr;
			BaseUploadStagingBytes -= static_cast<std::uint64_t>(Value.Width) * Value.Height *
				GetRenderTexturePixelBytes(Value.Format);
		}
	}

	void SDLTextureCache::TrimBaseUploadStaging() {
		constexpr std::uint64_t Budget = 16ull * 1024 * 1024;
		while (BaseUploadStagingBytes > Budget) {
			Entry *Oldest = nullptr;
			for (auto &[Identity, Value] : Textures) {
				(void)Identity;
				if (Value.TransferBuffer && (!Oldest || Value.LastUploadSerial < Oldest->LastUploadSerial))
					Oldest = &Value;
			}
			if (!Oldest) break;
			SDL_ReleaseGPUTransferBuffer(Gpu, std::exchange(Oldest->TransferBuffer, nullptr));
			BaseUploadStagingBytes -= static_cast<std::uint64_t>(Oldest->Width) * Oldest->Height *
				GetRenderTexturePixelBytes(Oldest->Format);
			++BaseUploadStagingEvictions;
		}
	}

	void SDLTextureCache::Destroy() {
		if (!Gpu) return;
		DestroyResident();
		DestroyEntry(White);
		if (MaterialUploadBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, MaterialUploadBuffer);
		MaterialUploadBuffer = nullptr;
		MaterialUploadCapacity = 0;
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
		White = std::move(Value);
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
		bool NewBuffer = false;
		if (!Value.TransferBuffer) {
			const auto Size = static_cast<std::uint32_t>(static_cast<std::uint64_t>(Value.Width) * Value.Height *
				GetRenderTexturePixelBytes(Value.Format));
			const SDL_GPUTransferBufferCreateInfo Info{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = Size};
			Value.TransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &Info);
			if (!Value.TransferBuffer)
				throw std::runtime_error(std::format("Failed to create texture upload staging: {}", SDL_GetError()));
			BaseUploadStagingBytes += Size;
			NewBuffer = true;
			if (Metrics) ++Metrics->TransferBufferCreations;
		}
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Value.TransferBuffer, !Initial && !NewBuffer);
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
		SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, FullReplacement && !NewBuffer);
		Value.LastUploadSerial = ++BaseUploadSerial;
		if (Metrics) {
			++Metrics->UploadOperations;
			Metrics->UploadedBytes += Pixels.size();
			if (!Initial) {
				++Metrics->TextureUpdates;
				if (!NewBuffer) {
					++Metrics->BufferCycleRequests;
					if (FullReplacement) ++Metrics->BufferCycleRequests;
				}
			}
		}
	}

	void SDLTextureCache::ApplyPublication(const RenderPublication &Publication) {
		if (!Gpu) throw std::logic_error("Cannot apply textures without an SDL GPU device");
		++MaterialEpoch;
		if (Publication.FullResync) DestroyResident();
		for (const auto &Remove : Publication.TextureRemoves) {
			auto Found = Textures.find(Remove.Texture);
			if (Found == Textures.end()) continue;
			DestroyEntry(Found->second);
			Textures.erase(Found);
		}
		const bool BaseUpload = !Publication.TextureCreates.empty() || !Publication.TextureUpdates.empty();
		SDL_GPUCommandBuffer *Commands = nullptr;
		SDL_GPUCopyPass *CopyPass = nullptr;
		const auto BeginCopy = [&] {
			if (CopyPass) return true;
			Commands = SDL_AcquireGPUCommandBuffer(Gpu);
			if (!Commands) return false;
			CopyPass = SDL_BeginGPUCopyPass(Commands);
			if (CopyPass) return true;
			SDL_CancelGPUCommandBuffer(Commands);
			Commands = nullptr;
			return false;
		};
		const auto ReleaseStaging = [&] {
			for (auto *Transfer : MaterialStaging) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
			MaterialStaging.clear();
		};
		const auto FinishPending = [&](bool Failed) {
			for (auto *Value : PendingMaterialEntries) {
				if (Failed) for (unsigned Index = 0; Index < Value->Views.size(); ++Index) {
					if (!(Value->PendingViews & (1u << Index))) continue;
					auto &View = Value->Views[Index];
					if (View.Texture || View.AttemptedRevision != Value->Revision) ++MaterialViewFallbacks;
					DestroyView(View);
					View.AttemptedRevision = Value->Revision;
				}
				Value->PendingViews = 0;
			}
			PendingMaterialEntries.clear();
		};
		try {
			if (BaseUpload && !BeginCopy())
				throw std::runtime_error(std::format("Failed to begin texture upload: {}", SDL_GetError()));
			for (const auto &Create : Publication.TextureCreates) {
				auto Value = CreateEntry(Create.Width, Create.Height, Create.Revision, Create.Format);
				try {
					Upload(Value, CopyPass, 0, 0, Create.Width, Create.Height, *Create.Pixels, true);
					const auto [Position, Inserted] = Textures.try_emplace(Create.Texture, std::move(Value));
					if (!Inserted) throw std::logic_error("Texture publication attempted duplicate SDL residency");
					RetainSource(Position->second, Create.Pixels);
				} catch (...) {
					DestroyEntry(Value);
					throw;
				}
			}
			for (const auto &Update : Publication.TextureUpdates) {
				auto &Value = Textures.at(Update.Texture);
				Upload(Value, CopyPass, Update.X, Update.Y, Update.Width, Update.Height, *Update.Pixels, false);
				Value.Revision = Update.Revision;
				UpdateSource(Value, Update);
				QueueMaterialViews(Value, Value.RequestedViews);
			}
			const auto QueueObject = [&](const auto &Object) {
				if (!Object.Visible) return;
				if (Object.Primitives) {
					for (const auto &Primitive : *Object.Primitives) QueueMaterial(Primitive.Material);
				} else QueueMaterial(Object.Material);
			};
			// Previously visible demand is admitted before newly published objects.
			QueueDeferredMaterialViews();
			if (!Textures.empty()) {
				for (const auto &Object : Publication.Creates) QueueObject(Object);
				for (const auto &Object : Publication.Updates) QueueObject(Object);
			}
			if (!PendingMaterialEntries.empty() && BeginCopy()) {
				bool UsedReusableTransfer = false;
				std::uint32_t Uploads = 0;
				std::uint64_t UploadBytes = 0;
				for (auto *Value : PendingMaterialEntries) {
					for (unsigned Index = 0; Index < Value->Views.size(); ++Index) {
						const auto Flag = static_cast<std::uint8_t>(1u << Index);
						if (!(Value->PendingViews & Flag)) continue;
						const auto Layout = GetMaterialMipLayout(Value->Width, Value->Height, MATERIAL_CHAIN_LIMIT);
						if (Layout && (Uploads >= 32 || UploadBytes + Layout->Bytes > 32ull * 1024 * 1024)) {
							Value->PendingViews &= ~Flag;
							Value->DeferredViews |= Flag;
							++DeferredMaterialViewUploads;
							continue;
						}
						UploadMaterialView(*Value, Index, CopyPass, UsedReusableTransfer);
						++Uploads;
						if (Layout) UploadBytes += Layout->Bytes;
					}
				}
			}
			bool OptionalFailure = !CopyPass && !PendingMaterialEntries.empty();
			if (CopyPass) {
				SDL_EndGPUCopyPass(CopyPass);
				CopyPass = nullptr;
				const bool Submitted = SDL_SubmitGPUCommandBuffer(std::exchange(Commands, nullptr));
				if (!Submitted && BaseUpload)
					throw std::runtime_error(std::format("Failed to submit texture upload: {}", SDL_GetError()));
				OptionalFailure = !Submitted;
			}
			FinishPending(OptionalFailure);
			ReleaseStaging();
			TrimBaseUploadStaging();
		} catch (...) {
			if (CopyPass) SDL_EndGPUCopyPass(CopyPass);
			if (Commands) SDL_CancelGPUCommandBuffer(Commands);
			PendingMaterialEntries.clear();
			ReleaseStaging();
			DestroyResident();
			throw;
		}
	}

	SDL_GPUTexture *SDLTextureCache::Find(RenderTextureIdentity Texture) const {
		const auto Found = Textures.find(Texture);
		return Found == Textures.end() ? nullptr : Found->second.Texture;
	}

	RenderTextureFormat SDLTextureCache::GetFormat(RenderTextureIdentity Texture) const {
		const auto Found = Textures.find(Texture);
		if (Found == Textures.end()) throw std::out_of_range("[Render:Texture] Texture has no SDL residency");
		return Found->second.Format;
	}

	SDLMaterialTextureBinding SDLTextureCache::FindMaterialTexture(RenderTextureIdentity Texture, bool Srgb) {
		const auto Found = Textures.find(Texture);
		if (Found == Textures.end()) return {};
		auto &Value = Found->second;
		if (Value.Format == RenderTextureFormat::Rgba32Float) return {Value.Texture, Srgb, false};
		auto &View = Value.Views[Srgb ? 1 : 0];
		if (!View.Texture || View.AttemptedRevision != Value.Revision) {
			if (Value.GetSource() && View.AttemptedRevision != Value.Revision) {
				Value.DeferredViews |= Srgb ? SRGB_VIEW : LINEAR_VIEW;
				Value.LastDemandEpoch = MaterialEpoch;
			}
			return {Value.Texture, false, false};
		}
		View.LastUsedEpoch = MaterialEpoch;
		return {View.Texture, Srgb, View.Levels > 1};
	}
}

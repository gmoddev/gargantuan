#include "render/sdl/SDLReflectionProbes.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLTextureCache.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace gargantuan {
	void SDLReflectionProbes::EnsureFallback(SDLFrameContext &Frame) {
		if (!Sampler) {
			const SDL_GPUSamplerCreateInfo Info{
				.min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
				.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
				.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.max_lod = 6.0f,
			};
			Sampler = SDL_CreateGPUSampler(Gpu, &Info);
			if (!Sampler) throw std::runtime_error("[Renderer:Probe] Could not create the array sampler");
		}
		if (!Fallback) {
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
				.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
				.width = 1, .height = 1, .layer_count_or_depth = 6, .num_levels = 1,
			};
			Fallback = SDL_CreateGPUTexture(Gpu, &Info);
			if (!Fallback) throw std::runtime_error("[Renderer:Probe] Could not create the cube array fallback");
		}
		if (!Buffer) {
			const SDL_GPUBufferCreateInfo Info{
				.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
				.size = sizeof(SDLReflectionProbeRecord) * MaximumResidentReflectionProbes,
			};
			Buffer = SDL_CreateGPUBuffer(Gpu, &Info);
			if (!Buffer) throw std::runtime_error("[Renderer:Probe] Could not create the record buffer");
		}
		if (!Transfer) {
			const SDL_GPUTransferBufferCreateInfo Info{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
				.size = sizeof(SDLReflectionProbeRecord) * MaximumResidentReflectionProbes,
			};
			Transfer = SDL_CreateGPUTransferBuffer(Gpu, &Info);
			if (!Transfer) throw std::runtime_error("[Renderer:Probe] Could not create the record staging");
		}
		if (!FallbackInitialized) {
			for (std::uint32_t Face = 0; Face < 6; ++Face) {
				const SDL_GPUColorTargetInfo Target{
					.texture = Fallback, .layer_or_depth_plane = Face, .clear_color = {0, 0, 0, 1},
					.load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE,
				};
				auto *Pass = SDL_BeginGPURenderPass(Frame.Commands, &Target, 1, nullptr);
				if (!Pass) throw std::runtime_error("[Renderer:Probe] Could not clear the array fallback");
				SDL_EndGPURenderPass(Pass);
			}
			PendingFallback = true;
		}
	}

	bool SDLReflectionProbes::EnsureArrays() {
		if (Irradiance && Reflections) return true;
		if (ArraysUnavailable) return false;
		if (PrepareSequence < RetryArraysAfter) return false;
		constexpr auto Usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		if (!SDL_GPUTextureSupportsFormat(Gpu, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
			SDL_GPU_TEXTURETYPE_CUBE_ARRAY, Usage)) { ArraysUnavailable = true; return false; }
		const auto Make = [&](std::uint32_t Size, std::uint32_t Levels) {
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_CUBE_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
				.usage = Usage, .width = Size, .height = Size,
				.layer_count_or_depth = 6 * MaximumResidentReflectionProbes, .num_levels = Levels,
			};
			return SDL_CreateGPUTexture(Gpu, &Info);
		};
		Irradiance = Make(16, 1); Reflections = Make(64, 7);
		if (!Irradiance || !Reflections) {
			if (Irradiance) SDL_ReleaseGPUTexture(Gpu, Irradiance);
			if (Reflections) SDL_ReleaseGPUTexture(Gpu, Reflections);
			Irradiance = nullptr; Reflections = nullptr;
			RetryArraysAfter = PrepareSequence + 120; return false;
		}
		return true;
	}

	void SDLReflectionProbes::CopyCube(SDLFrameContext &Frame, SDL_GPUTexture *Source, SDL_GPUTexture *Target,
		std::uint32_t Slot, std::uint32_t Dimension, std::uint32_t Levels) {
		for (std::uint32_t Level = 0; Level < Levels; ++Level) {
			const auto Size = std::max(1u, Dimension >> Level);
			for (std::uint32_t Face = 0; Face < 6; ++Face) {
				const SDL_GPUBlitInfo Info{
					.source = {.texture = Source, .mip_level = Level, .layer_or_depth_plane = Face, .w = Size, .h = Size},
					.destination = {.texture = Target, .mip_level = Level, .layer_or_depth_plane = Slot * 6 + Face, .w = Size, .h = Size},
					.load_op = SDL_GPU_LOADOP_DONT_CARE, .filter = SDL_GPU_FILTER_NEAREST,
				};
				SDL_BlitGPUTexture(Frame.Commands, &Info);
			}
		}
	}

	void SDLReflectionProbes::UploadRecords(SDLFrameContext &Frame) {
		const bool Equal = Records.size() == CommittedRecords.size() &&
			(Records.empty() || std::memcmp(Records.data(), CommittedRecords.data(), Records.size() * sizeof(Records[0])) == 0);
		if (Equal && RecordsValid && FallbackInitialized) return;
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Transfer, true);
		if (!Mapped) throw std::runtime_error("[Renderer:Probe] Could not map the record staging");
		const SDLReflectionProbeRecord Empty{};
		const auto Bytes = static_cast<std::uint32_t>(std::max<std::size_t>(1, Records.size()) * sizeof(Records[0]));
		std::memcpy(Mapped, Records.empty() ? &Empty : Records.data(), Bytes);
		SDL_UnmapGPUTransferBuffer(Gpu, Transfer);
		auto *Pass = SDL_BeginGPUCopyPass(Frame.Commands);
		if (!Pass) throw std::runtime_error("[Renderer:Probe] Could not begin the record upload");
		const SDL_GPUTransferBufferLocation Source{.transfer_buffer = Transfer};
		const SDL_GPUBufferRegion Destination{.buffer = Buffer, .size = Bytes};
		SDL_UploadToGPUBuffer(Pass, &Source, &Destination, true);
		SDL_EndGPUCopyPass(Pass);
		PendingRecords = true;
		if (Frame.Metrics) Frame.Metrics->ProbeUploadBytes += Bytes;
	}

	SDLReflectionProbeBindings SDLReflectionProbes::Prepare(SDLFrameContext &Frame, bool AllowPreprocessing) {
		const auto Start = std::chrono::steady_clock::now();
		if (!SupportsArrays) SupportsArrays = SDL_GPUTextureSupportsFormat(Gpu,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTURETYPE_CUBE_ARRAY,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
		if (!*SupportsArrays) return {};
		++PrepareSequence;
		PendingFallback = false; PendingRecords = false;
		for (auto &Slot : Slots) Slot.PendingSource.reset();
		EnsureFallback(Frame);
		Candidates.clear(); Records.clear();
		const auto &State = Frame.Projection.GetFrame();
		if (State.Environment.ReflectionProbes) {
			for (const auto &Probe : *State.Environment.ReflectionProbes) Candidates.push_back(&Probe);
			const auto Distance = [&](const auto *Probe) {
				const auto Local = glm::transpose(Probe->Rotation) * (State.Camera.Position - Probe->Position);
				const auto Delta = glm::max(glm::abs(Local) - Probe->HalfSize, glm::vec3(0));
				return glm::dot(glm::dvec3(Delta), glm::dvec3(Delta));
			};
			std::ranges::sort(Candidates, [&](const auto *Left, const auto *Right) {
				const auto A = Distance(Left), B = Distance(Right);
				if (A != B) return A < B;
				if (Left->Priority != Right->Priority) return Left->Priority > Right->Priority;
				return Left->Object < Right->Object;
			});
			if (Candidates.size() > MaximumResidentReflectionProbes) Candidates.resize(MaximumResidentReflectionProbes);
			std::ranges::sort(Candidates, {}, [](const auto *Probe) { return Probe->Object; });
		}
		std::array<bool, MaximumResidentReflectionProbes> Keep{};
		if (PreparingSlot) {
			auto &Preparing = Slots[*PreparingSlot];
			if (Preparing.Lighting && !Preparing.Lighting->PollPreparation()) {
				// Retain an evicted in-flight prefilter until its fence completes;
				// the next selected probe cannot exceed the single-preparation budget.
				Keep[*PreparingSlot] = true;
			} else PreparingSlot.reset();
		}
		if (Candidates.empty()) {
			if (Irradiance) SDL_ReleaseGPUTexture(Gpu, Irradiance);
			if (Reflections) SDL_ReleaseGPUTexture(Gpu, Reflections);
			Irradiance = nullptr; Reflections = nullptr; ArraysUnavailable = false;
		}
		for (const auto *Probe : Candidates)
			for (std::size_t Index = 0; Index < Slots.size(); ++Index)
				if (Slots[Index].Object == Probe->Object) Keep[Index] = true;
		for (std::size_t Index = 0; Index < Slots.size(); ++Index) if (!Keep[Index]) {
			Slots[Index] = {};
			if (PreparingSlot == Index) PreparingSlot.reset();
		}
		const auto Generation = Frame.TextureResources->GetResidencyGeneration();
		SDL_GPUTextureSamplerBinding BrdfLut{};
		for (const auto *Probe : Candidates) {
			auto Found = std::ranges::find(Slots, Probe->Object, &Slot::Object);
			if (Found == Slots.end()) {
				Found = std::ranges::find_if(Slots, [](const auto &Slot) { return !Slot.Object.IsValid(); });
				if (Found == Slots.end()) continue;
				Found->Object = Probe->Object;
				Found->Lighting = std::make_unique<SDLImageBasedLighting>(Gpu);
			}
			auto &Slot = *Found;
			const auto Index = static_cast<std::size_t>(Found - Slots.begin());
			const bool Changed = Slot.PreparedSource != Probe->Source || Slot.PreparedGeneration != Generation;
			const bool Pending = !Slot.Lighting->PollPreparation();
			if (PreparingSlot == Index && !Pending) PreparingSlot.reset();
			if (Changed && !Pending && !PreparingSlot && AllowPreprocessing) {
				const auto Builds = Frame.Metrics ? Frame.Metrics->IblBuilds : 0;
				Slot.Bindings = Slot.Lighting->Prepare(Probe->Source, *Frame.TextureResources, Frame.Metrics);
				if (Frame.Metrics) Frame.Metrics->ProbePreprocessBuilds += Frame.Metrics->IblBuilds - Builds;
				Slot.PreparedSource = Probe->Source; Slot.PreparedGeneration = Generation;
				if (Slot.Lighting->IsPreparationPending()) PreparingSlot = Index;
				else PreparingSlot.reset();
				if (Changed) AllowPreprocessing = false;
			}
			if (Slot.PreparedSource != Probe->Source || Slot.PreparedGeneration != Generation || !Slot.Bindings.Enabled) continue;
			if (!EnsureArrays()) continue;
			BrdfLut = Slot.Bindings.BrdfLut;
			if (Slot.CopiedSource != Probe->Source || Slot.CopiedGeneration != Generation) {
				CopyCube(Frame, Slot.Bindings.Irradiance.texture, Irradiance, static_cast<std::uint32_t>(Index), 16, 1);
				CopyCube(Frame, Slot.Bindings.Reflections.texture, Reflections, static_cast<std::uint32_t>(Index), 64, 7);
				Slot.PendingSource = Probe->Source; Slot.PendingGeneration = Generation;
				if (Frame.Metrics) ++Frame.Metrics->ProbeUpdates;
			}
			const auto Inverse = glm::transpose(Probe->Rotation);
			Records.push_back({glm::vec4(Probe->Position, Probe->Intensity),
				{glm::vec4(Inverse[0][0], Inverse[1][0], Inverse[2][0], 0),
				 glm::vec4(Inverse[0][1], Inverse[1][1], Inverse[2][1], 0),
				 glm::vec4(Inverse[0][2], Inverse[1][2], Inverse[2][2], 0)},
				glm::vec4(Probe->HalfSize, Probe->BlendDistance), glm::vec4(Probe->CaptureOffset, Probe->BoxProjection ? 1.0f : 0.0f),
				glm::vec4(static_cast<float>(Probe->Priority), static_cast<float>(Index), 0, 0)});
		}
		UploadRecords(Frame);
		if (Frame.Metrics) {
			Frame.Metrics->ResidentReflectionProbes = Records.size();
			Frame.Metrics->ProbeArrayBytes = Irradiance ? (16u * 16u * 6u + 32766u) * MaximumResidentReflectionProbes * 8u : 0;
			Frame.Metrics->LastProbePreparationNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count());
		}
		return {{Irradiance ? Irradiance : Fallback, Sampler}, {Reflections ? Reflections : Fallback, Sampler},
			BrdfLut, Buffer, static_cast<std::uint32_t>(Records.size())};
	}

	void SDLReflectionProbes::OnFrameSubmitted() {
		if (PendingFallback) FallbackInitialized = true;
		if (PendingRecords) { CommittedRecords = Records; RecordsValid = true; }
		for (auto &Slot : Slots) if (Slot.PendingSource) {
			Slot.CopiedSource = Slot.PendingSource; Slot.CopiedGeneration = Slot.PendingGeneration;
			Slot.PendingSource.reset();
		}
	}
	void SDLReflectionProbes::OnFrameAborted() {
		// SDL may select a new backing when recording a cycled buffer upload.
		// Canceling that command does not restore the previous active backing.
		// Even a return to the last committed values therefore needs an upload.
		RecordsValid = false;
		PendingRecords = false;
		PendingFallback = false;
		for (auto &Slot : Slots) Slot.PendingSource.reset();
	}
	void SDLReflectionProbes::Destroy() {
		for (auto &Slot : Slots) Slot = {};
		if (Irradiance) SDL_ReleaseGPUTexture(Gpu, Irradiance);
		if (Reflections) SDL_ReleaseGPUTexture(Gpu, Reflections);
		if (Fallback) SDL_ReleaseGPUTexture(Gpu, Fallback);
		if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
		if (Buffer) SDL_ReleaseGPUBuffer(Gpu, Buffer);
		if (Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Transfer);
		Irradiance = nullptr; Reflections = nullptr; Fallback = nullptr;
		Sampler = nullptr; Buffer = nullptr; Transfer = nullptr;
		FallbackInitialized = false; PendingFallback = false; PendingRecords = false; RecordsValid = false;
		ArraysUnavailable = false;
		SupportsArrays.reset();
		PrepareSequence = 0; RetryArraysAfter = 0;
		CommittedRecords.clear(); Records.clear(); PreparingSlot.reset();
	}
}

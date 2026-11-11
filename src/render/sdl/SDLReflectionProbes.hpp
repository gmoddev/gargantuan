#pragma once

#include "gargantuan/render/RenderEnvironment.hpp"
#include "render/sdl/SDLImageBasedLighting.hpp"
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace gargantuan {
	struct SDLFrameContext;
	struct alignas(16) SDLReflectionProbeRecord {
		glm::vec4 PositionIntensity;
		std::array<glm::vec4, 3> InverseRotation;
		glm::vec4 HalfSizeBlend;
		glm::vec4 CaptureOffsetBox;
		glm::vec4 PriorityLayer;
	};
	static_assert(sizeof(SDLReflectionProbeRecord) == 112);
	struct SDLReflectionProbeBindings {
		SDL_GPUTextureSamplerBinding Irradiance{};
		SDL_GPUTextureSamplerBinding Reflections{};
		SDL_GPUTextureSamplerBinding BrdfLut{};
		SDL_GPUBuffer *Records = nullptr;
		std::uint32_t Count = 0;
	};

	class SDLReflectionProbes final {
	  public:
		explicit SDLReflectionProbes(SDL_GPUDevice *Device) : Gpu(Device) {
			Candidates.reserve(MaximumRenderReflectionProbes);
			Records.reserve(MaximumResidentReflectionProbes);
			CommittedRecords.reserve(MaximumResidentReflectionProbes);
		}
		~SDLReflectionProbes() { Destroy(); }
		[[nodiscard]] SDLReflectionProbeBindings Prepare(SDLFrameContext &Frame, bool AllowPreprocessing);
		void OnFrameSubmitted();
		void Destroy();
	  private:
		struct Slot {
			ObjectId Object;
			std::unique_ptr<SDLImageBasedLighting> Lighting;
			std::optional<RenderSkyState> PreparedSource;
			std::uint64_t PreparedGeneration = 0;
			SDLImageBasedLightingBindings Bindings;
			std::optional<RenderSkyState> CopiedSource;
			std::uint64_t CopiedGeneration = 0;
			std::optional<RenderSkyState> PendingSource;
			std::uint64_t PendingGeneration = 0;
		};
		void EnsureFallback(SDLFrameContext &Frame);
		[[nodiscard]] bool EnsureArrays();
		void UploadRecords(SDLFrameContext &Frame);
		void CopyCube(SDLFrameContext &Frame, SDL_GPUTexture *Source, SDL_GPUTexture *Target,
			std::uint32_t Slot, std::uint32_t Dimension, std::uint32_t Levels);
		SDL_GPUDevice *Gpu;
		SDL_GPUTexture *Fallback = nullptr;
		SDL_GPUTexture *Irradiance = nullptr;
		SDL_GPUTexture *Reflections = nullptr;
		SDL_GPUSampler *Sampler = nullptr;
		SDL_GPUBuffer *Buffer = nullptr;
		SDL_GPUTransferBuffer *Transfer = nullptr;
		bool FallbackInitialized = false;
		bool PendingFallback = false;
		bool PendingRecords = false;
		bool ArraysUnavailable = false;
		std::optional<bool> SupportsArrays;
		std::uint64_t PrepareSequence = 0;
		std::uint64_t RetryArraysAfter = 0;
		std::optional<std::size_t> PreparingSlot;
		std::array<Slot, MaximumResidentReflectionProbes> Slots;
		std::vector<const RenderReflectionProbeState *> Candidates;
		std::vector<SDLReflectionProbeRecord> Records;
		std::vector<SDLReflectionProbeRecord> CommittedRecords;
	};
}

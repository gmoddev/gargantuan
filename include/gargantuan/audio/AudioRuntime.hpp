#pragma once

#include "gargantuan/audio/AudioBackend.hpp"
#include "gargantuan/datatypes/CFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gargantuan {
	class AssetService;
	class SemanticSpatialResolver;
	class Sound;

	struct AudioRuntimeMetrics {
		std::size_t ActiveVoices = 0;
		std::uint64_t VoiceAdmissions = 0;
		std::uint64_t VoiceRejections = 0;
		std::uint64_t MixedFrames = 0;
		std::uint64_t MixCpuNanoseconds = 0;
		std::uint64_t SemanticSourceResolutions = 0;
		std::uint64_t SemanticSourceCpuNanoseconds = 0;
		AudioBackendMetrics Backend;
	};

	class AudioRuntime final {
	  public:
		using DiagnosticCallback = std::function<void(std::string Code, std::string Message)>;

		static constexpr std::size_t MaximumVoices = 256;
		static constexpr std::size_t MixBlockFrames = 256;
		static constexpr std::size_t TargetQueuedFrames = 1024;
		static constexpr std::size_t MaximumBlocksPerStep = 4;

		AudioRuntime(
			std::shared_ptr<AssetService> Assets,
			std::unique_ptr<IAudioBackend> Backend,
			DiagnosticCallback Diagnostic = {},
			std::shared_ptr<SemanticSpatialResolver> Spatial = {}
		);
		~AudioRuntime();
		AudioRuntime(const AudioRuntime &) = delete;
		AudioRuntime &operator=(const AudioRuntime &) = delete;

		void RegisterSound(const std::shared_ptr<Sound> &SoundValue);
		void Step(const CFrame &Listener);
		void Shutdown();
		[[nodiscard]] bool IsAvailable() const;
		[[nodiscard]] AudioRuntimeMetrics GetMetrics() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}

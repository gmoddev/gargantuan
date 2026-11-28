#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace gargantuan {
	struct AudioBackendMetrics {
		std::uint64_t SubmittedFrames = 0;
		std::uint64_t SubmissionFailures = 0;
		std::uint64_t ObservedEmptyQueueEvents = 0;
	};

	class IAudioBackend {
	  public:
		virtual ~IAudioBackend() = default;
		[[nodiscard]] virtual bool IsAvailable() const = 0;
		[[nodiscard]] virtual std::uint32_t GetSampleRate() const = 0;
		[[nodiscard]] virtual std::size_t GetQueuedFrames() = 0;
		[[nodiscard]] virtual bool Submit(std::span<const float> InterleavedStereo) = 0;
		virtual void Clear() = 0;
		// Retain already mixed samples when the output supports a per-stream hold.
		// Other backends drain at most the runtime's bounded queue.
		virtual void SetPlaybackHeld(bool) {}
		virtual void Shutdown() = 0;
		[[nodiscard]] virtual std::string GetDiagnostic() const = 0;
		[[nodiscard]] virtual AudioBackendMetrics GetMetrics() const = 0;
	};

	class IAudioOutput {
	  public:
		virtual ~IAudioOutput() = default;
		[[nodiscard]] virtual std::unique_ptr<IAudioBackend> OpenStream() = 0;
	};
	[[nodiscard]] std::shared_ptr<IAudioOutput> CreateSdlAudioOutput();
	[[nodiscard]] std::unique_ptr<IAudioBackend> CreateSdlAudioBackend();
}

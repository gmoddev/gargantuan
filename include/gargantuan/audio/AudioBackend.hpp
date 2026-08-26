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
		virtual void Shutdown() = 0;
		[[nodiscard]] virtual std::string GetDiagnostic() const = 0;
		[[nodiscard]] virtual AudioBackendMetrics GetMetrics() const = 0;
	};

	[[nodiscard]] std::unique_ptr<IAudioBackend> CreateSdlAudioBackend();
}

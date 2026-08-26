#include "gargantuan/audio/AudioBackend.hpp"

#include <SDL3/SDL.h>

#include <limits>

namespace gargantuan {
	namespace {
		class SdlAudioBackend final : public IAudioBackend {
		  public:
			SdlAudioBackend() {
				if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
					Diagnostic = SDL_GetError();
					return;
				}
				OwnsAudioSubsystem = true;
				const SDL_AudioSpec Specification{
					.format = SDL_AUDIO_F32,
					.channels = 2,
					.freq = static_cast<int>(SampleRate),
				};
				Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Specification, nullptr, nullptr);
				if (!Stream || !SDL_ResumeAudioStreamDevice(Stream)) {
					Diagnostic = SDL_GetError();
					if (Stream) SDL_DestroyAudioStream(Stream);
					Stream = nullptr;
					return;
				}
				Available = true;
			}

			~SdlAudioBackend() override {
				Shutdown();
			}

			bool IsAvailable() const override {
				return Available;
			}
			std::uint32_t GetSampleRate() const override {
				return SampleRate;
			}

			std::size_t GetQueuedFrames() override {
				if (!Available || !Stream) return 0;
				const auto QueuedBytes = SDL_GetAudioStreamQueued(Stream);
				if (QueuedBytes < 0) {
					Fail(SDL_GetError());
					return 0;
				}
				if (SubmittedOnce && QueuedBytes == 0) ++Metrics.ObservedEmptyQueueEvents;
				return static_cast<std::size_t>(QueuedBytes) / (sizeof(float) * 2);
			}

			bool Submit(std::span<const float> InterleavedStereo) override {
				if (!Available || !Stream || InterleavedStereo.empty() || (InterleavedStereo.size() & 1u) != 0)
					return false;
				const auto ByteCount = InterleavedStereo.size_bytes();
				if (ByteCount > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
					!SDL_PutAudioStreamData(Stream, InterleavedStereo.data(), static_cast<int>(ByteCount))) {
					++Metrics.SubmissionFailures;
					Fail(SDL_GetError());
					return false;
				}
				SubmittedOnce = true;
				Metrics.SubmittedFrames += InterleavedStereo.size() / 2;
				return true;
			}

			void Clear() override {
				if (Stream) (void)SDL_ClearAudioStream(Stream);
			}

			void Shutdown() override {
				Available = false;
				if (Stream) {
					SDL_DestroyAudioStream(Stream);
					Stream = nullptr;
				}
				if (OwnsAudioSubsystem) {
					SDL_QuitSubSystem(SDL_INIT_AUDIO);
					OwnsAudioSubsystem = false;
				}
			}

			std::string GetDiagnostic() const override {
				return Diagnostic;
			}
			AudioBackendMetrics GetMetrics() const override {
				return Metrics;
			}

		  private:
			void Fail(const char *Message) {
				Available = false;
				Diagnostic = Message ? Message : "SDL audio device became unavailable";
			}

			static constexpr std::uint32_t SampleRate = 48'000;
			SDL_AudioStream *Stream = nullptr;
			bool OwnsAudioSubsystem = false;
			bool Available = false;
			bool SubmittedOnce = false;
			std::string Diagnostic;
			AudioBackendMetrics Metrics;
		};
	}

	std::unique_ptr<IAudioBackend> CreateSdlAudioBackend() {
		return std::make_unique<SdlAudioBackend>();
	}
}

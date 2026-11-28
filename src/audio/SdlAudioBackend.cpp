#include "gargantuan/audio/AudioBackend.hpp"

#include <SDL3/SDL.h>

#include <limits>

namespace gargantuan {
	namespace {
		constexpr SDL_AudioSpec Specification{SDL_AUDIO_F32, 2, 48'000};
		class SdlAudioOutput final : public IAudioOutput, public std::enable_shared_from_this<SdlAudioOutput> {
		  public:
			explicit SdlAudioOutput(SDL_AudioDeviceID Borrowed) : Device(Borrowed) {}
			SdlAudioOutput() {
				if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
					Diagnostic = SDL_GetError();
					return;
				}
				OwnsAudioSubsystem = true;
				Device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Specification);
				if (!Device) Diagnostic = SDL_GetError();
			}
			~SdlAudioOutput() override {
				if (OwnsAudioSubsystem && Device) SDL_CloseAudioDevice(Device);
				if (OwnsAudioSubsystem) SDL_QuitSubSystem(SDL_INIT_AUDIO);
			}
			std::unique_ptr<IAudioBackend> OpenStream() override;
			SDL_AudioDeviceID Device{};
			std::string Diagnostic;
		  private:
			bool OwnsAudioSubsystem = false;
		};
		class SdlAudioBackend final : public IAudioBackend {
		  public:
			explicit SdlAudioBackend(std::shared_ptr<SdlAudioOutput> DeviceOutput) : Output(std::move(DeviceOutput)) {
				if (!Output->Device) { Diagnostic = Output->Diagnostic; return; }
				Stream = SDL_CreateAudioStream(&Specification, &Specification);
				if (!Stream || !SDL_BindAudioStream(Output->Device, Stream)) {
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

			void SetPlaybackHeld(bool Held) override {
				if (!Stream || !Available) return;
				if (Held) SDL_UnbindAudioStream(Stream);
				else if (!SDL_BindAudioStream(Output->Device, Stream)) Fail(SDL_GetError());
			}

			void Shutdown() override {
				Available = false;
				if (Stream) {
					SDL_DestroyAudioStream(Stream);
					Stream = nullptr;
				}
				Output.reset();
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
			std::shared_ptr<SdlAudioOutput> Output;
			bool Available = false;
			bool SubmittedOnce = false;
			std::string Diagnostic;
			AudioBackendMetrics Metrics;
		};
		std::unique_ptr<IAudioBackend> SdlAudioOutput::OpenStream() {
			return std::make_unique<SdlAudioBackend>(shared_from_this());
		}
	}

	std::shared_ptr<IAudioOutput> CreateSdlAudioOutput() { return std::make_shared<SdlAudioOutput>(); }
	std::shared_ptr<IAudioOutput> CreateSdlAudioOutput(std::uint32_t Device) {
		return std::make_shared<SdlAudioOutput>(Device);
	}

	std::unique_ptr<IAudioBackend> CreateSdlAudioBackend() {
		return CreateSdlAudioOutput()->OpenStream();
	}
}

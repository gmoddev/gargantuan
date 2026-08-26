#include "gargantuan/audio/AudioRuntime.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		constexpr float Pi = 3.14159265358979323846f;

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		std::optional<glm::vec3> ResolveSoundPosition(const std::shared_ptr<Sound> &SoundValue) {
			CFrame LocalOffset;
			auto Current = SoundValue ? SoundValue->GetParent() : std::nullopt;
			while (Current) {
				auto Object = *Current;
				if (!Object || Object->GetDestroyed() || Object->IsDestroying()) return std::nullopt;
				if (auto AttachmentValue = std::dynamic_pointer_cast<Attachment>(Object))
					LocalOffset = AttachmentValue->GetCFrame() * LocalOffset;
				else if (auto Part = std::dynamic_pointer_cast<BasePart>(Object)) {
					const auto Position = (Part->GetCFrame() * LocalOffset).Position;
					return IsFinite(Position) ? std::optional(Position) : std::nullopt;
				}
				Current = Object->GetParent();
			}
			return std::nullopt;
		}
	}

	struct AudioRuntime::Impl {
		struct Voice {
			AssetAudioResource Resource;
			double FramePosition = 0.0;
			std::string Reference;
		};

		struct TrackedSound {
			std::weak_ptr<Sound> Value;
			std::uint64_t ObservedPlayGeneration = 0;
			std::uint64_t ObservedSeekGeneration = 0;
			std::optional<Voice> ActiveVoice;
		};

		std::shared_ptr<AssetService> Assets;
		std::unique_ptr<IAudioBackend> Backend;
		DiagnosticCallback Diagnostic;
		std::map<ObjectId, TrackedSound> Sounds;
		std::set<std::pair<ObjectId, std::string>> EmittedDiagnostics;
		std::array<float, AudioRuntime::MixBlockFrames * 2> MixBuffer{};
		std::vector<ObjectId> Completed;
		AudioRuntimeMetrics Metrics;
		bool ShutDown = false;

		Impl(
			std::shared_ptr<AssetService> AssetsValue,
			std::unique_ptr<IAudioBackend> BackendValue,
			DiagnosticCallback DiagnosticValue
		)
			: Assets(std::move(AssetsValue)), Backend(std::move(BackendValue)), Diagnostic(std::move(DiagnosticValue)) {
			Completed.reserve(AudioRuntime::MaximumVoices);
			if (Backend && !Backend->IsAvailable() && Diagnostic)
				Diagnostic(
					"DeviceUnavailable",
					Backend->GetDiagnostic().empty()
						? "Audio device initialization failed; game execution continues silently"
						: Backend->GetDiagnostic()
				);
		}

		void Emit(ObjectId Id, std::string Code, std::string Message) {
			if (EmittedDiagnostics.size() >= AudioRuntime::MaximumVoices ||
				!EmittedDiagnostics.emplace(Id, Code).second || !Diagnostic)
				return;
			Diagnostic(std::move(Code), std::move(Message));
		}

		std::size_t ActiveVoiceCount() const {
			return std::ranges::count_if(Sounds, [](const auto &Entry) {
				return Entry.second.ActiveVoice.has_value();
			});
		}

		void Admit(ObjectId Id, TrackedSound &Tracked, const std::shared_ptr<Sound> &SoundValue) {
			Tracked.ActiveVoice.reset();
			if (!Backend || !Backend->IsAvailable()) {
				++Metrics.VoiceRejections;
				SoundValue->RejectRuntimePlayback();
				Emit(Id, "DeviceUnavailable", "Audio device is unavailable; playback remains fail-open");
				return;
			}
			const auto Reference = SoundValue->GetSoundId();
			auto Resource = Assets && !Reference.empty() ? Assets->ResolveAudio(Reference) : std::nullopt;
			if (!Resource) {
				++Metrics.VoiceRejections;
				SoundValue->RejectRuntimePlayback();
				Emit(Id, "AssetUnavailable", "SoundId does not resolve to an available Audio asset");
				return;
			}
			if (ActiveVoiceCount() >= AudioRuntime::MaximumVoices) {
				++Metrics.VoiceRejections;
				SoundValue->RejectRuntimePlayback();
				Emit(Id, "VoiceLimit", "Audio voice limit reached; the newest voice was rejected");
				return;
			}
			const auto MaximumPosition = static_cast<double>(Resource->Value.FrameCount);
			const auto RequestedPosition = static_cast<double>(SoundValue->GetTimePosition()) *
										   Resource->Value.SampleRate;
			Tracked.ActiveVoice = Voice{*Resource, std::clamp(RequestedPosition, 0.0, MaximumPosition), Reference};
			SoundValue->SetRuntimeTimePosition(
				static_cast<float>(Tracked.ActiveVoice->FramePosition / Resource->Value.SampleRate), true
			);
			++Metrics.VoiceAdmissions;
		}

		void Reconcile() {
			for (auto Iterator = Sounds.begin(); Iterator != Sounds.end();) {
				auto SoundValue = Iterator->second.Value.lock();
				if (!SoundValue || SoundValue->GetDestroyed() || SoundValue->IsDestroying()) {
					Iterator = Sounds.erase(Iterator);
					continue;
				}
				auto &Tracked = Iterator->second;
				if (SoundValue->GetPlaybackState() == Enums::SoundPlaybackState::Stopped) {
					Tracked.ActiveVoice.reset();
				} else if (Tracked.ObservedPlayGeneration != SoundValue->PlayGeneration) {
					Tracked.ObservedPlayGeneration = SoundValue->PlayGeneration;
					Tracked.ObservedSeekGeneration = SoundValue->SeekGeneration;
					Admit(Iterator->first, Tracked, SoundValue);
				} else if (Tracked.ActiveVoice && Tracked.ObservedSeekGeneration != SoundValue->SeekGeneration) {
					Tracked.ObservedSeekGeneration = SoundValue->SeekGeneration;
					const auto &Audio = Tracked.ActiveVoice->Resource.Value;
					Tracked.ActiveVoice->FramePosition = std::clamp(
						static_cast<double>(SoundValue->GetTimePosition()) * Audio.SampleRate,
						0.0,
						static_cast<double>(Audio.FrameCount)
					);
				}

				if (Tracked.ActiveVoice) {
					auto Latest = Assets->ResolveAudio(Tracked.ActiveVoice->Reference);
					if (!Latest) {
						Tracked.ActiveVoice.reset();
						SoundValue->RejectRuntimePlayback();
						Emit(Iterator->first, "AssetUnavailable", "Playing Audio asset became unavailable");
					} else if (Latest->ContentRevision != Tracked.ActiveVoice->Resource.ContentRevision) {
						const auto Seconds = Tracked.ActiveVoice->FramePosition /
											 Tracked.ActiveVoice->Resource.Value.SampleRate;
						Tracked.ActiveVoice->Resource = *Latest;
						Tracked.ActiveVoice->FramePosition = std::min(
							Seconds * Latest->Value.SampleRate, static_cast<double>(Latest->Value.FrameCount)
						);
					}
				}
				++Iterator;
			}
		}

		std::pair<float, float> ReadFrame(const ImportedAudio &Audio, double Position, bool Looped) const {
			const auto Frame0 = std::min<std::uint32_t>(static_cast<std::uint32_t>(Position), Audio.FrameCount - 1);
			auto Frame1 = Frame0 + 1;
			if (Frame1 >= Audio.FrameCount) Frame1 = Looped ? 0 : Frame0;
			const auto Fraction = static_cast<float>(Position - std::floor(Position));
			const auto Sample = [&](std::uint32_t Frame, std::uint8_t Channel) {
				return static_cast<float>((*Audio.Pcm16)[static_cast<std::size_t>(Frame) * Audio.Channels + Channel]) /
					   32768.0f;
			};
			const auto Left0 = Sample(Frame0, 0);
			const auto Left1 = Sample(Frame1, 0);
			const auto Left = std::lerp(Left0, Left1, Fraction);
			if (Audio.Channels == 1) return {Left, Left};
			const auto Right0 = Sample(Frame0, 1);
			const auto Right1 = Sample(Frame1, 1);
			return {Left, std::lerp(Right0, Right1, Fraction)};
		}

		void Mix(const CFrame &Listener) {
			const auto Started = std::chrono::steady_clock::now();
			MixBuffer.fill(0.0f);
			Completed.clear();
			const auto ListenerPosition = Listener.Position;
			const auto ListenerRight = Listener.GetRightVector();
			const auto OutputRate = static_cast<double>(Backend->GetSampleRate());

			for (auto &[Id, Tracked] : Sounds) {
				if (!Tracked.ActiveVoice) continue;
				auto SoundValue = Tracked.Value.lock();
				if (!SoundValue || SoundValue->GetPlaybackState() != Enums::SoundPlaybackState::Playing) continue;
				auto &VoiceValue = *Tracked.ActiveVoice;
				const auto &Audio = VoiceValue.Resource.Value;
				const auto Step = static_cast<double>(Audio.SampleRate) / OutputRate * SoundValue->GetPlaybackSpeed();
				float LeftGain = SoundValue->GetVolume();
				float RightGain = SoundValue->GetVolume();
				const auto Position = ResolveSoundPosition(SoundValue);
				if (Position) {
					const auto Offset = *Position - ListenerPosition;
					const auto Distance = glm::length(Offset);
					const auto Minimum = SoundValue->GetRollOffMinDistance();
					const auto Maximum = SoundValue->GetRollOffMaxDistance();
					const auto DistanceGain = Distance <= Minimum	? 1.0f
											  : Distance >= Maximum ? 0.0f
																	: 1.0f - (Distance - Minimum) / (Maximum - Minimum);
					const auto Pan = Distance > 1.0e-6f
										 ? std::clamp(glm::dot(Offset / Distance, ListenerRight), -1.0f, 1.0f)
										 : 0.0f;
					LeftGain *= DistanceGain * std::cos((Pan + 1.0f) * Pi * 0.25f);
					RightGain *= DistanceGain * std::sin((Pan + 1.0f) * Pi * 0.25f);
				}

				bool Ended = false;
				for (std::size_t Frame = 0; Frame < AudioRuntime::MixBlockFrames; ++Frame) {
					if (VoiceValue.FramePosition >= Audio.FrameCount) {
						if (SoundValue->GetLooped())
							VoiceValue.FramePosition = std::fmod(VoiceValue.FramePosition, Audio.FrameCount);
						else {
							Ended = true;
							break;
						}
					}
					auto [Left, Right] = ReadFrame(Audio, VoiceValue.FramePosition, SoundValue->GetLooped());
					if (Position) {
						const auto Mono = (Left + Right) * 0.5f;
						Left = Mono;
						Right = Mono;
					}
					MixBuffer[Frame * 2] += Left * LeftGain;
					MixBuffer[Frame * 2 + 1] += Right * RightGain;
					VoiceValue.FramePosition += Step;
				}
				if (Ended)
					Completed.push_back(Id);
				else
					SoundValue->SetRuntimeTimePosition(
						static_cast<float>(VoiceValue.FramePosition / Audio.SampleRate), false
					);
			}

			for (auto &Sample : MixBuffer)
				Sample = std::clamp(Sample, -1.0f, 1.0f);
			for (const auto Id : Completed) {
				auto Existing = Sounds.find(Id);
				if (Existing == Sounds.end()) continue;
				auto SoundValue = Existing->second.Value.lock();
				Existing->second.ActiveVoice.reset();
				if (SoundValue) SoundValue->CompleteRuntimePlayback();
			}
			Metrics.MixedFrames += AudioRuntime::MixBlockFrames;
			Metrics.MixCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			);
		}

		void FailVoices() {
			for (auto &[Id, Tracked] : Sounds) {
				if (!Tracked.ActiveVoice) continue;
				Tracked.ActiveVoice.reset();
				if (auto SoundValue = Tracked.Value.lock()) SoundValue->RejectRuntimePlayback();
				Emit(Id, "DeviceLost", "Audio device submission failed; game execution continues silently");
			}
		}
	};

	AudioRuntime::AudioRuntime(
		std::shared_ptr<AssetService> Assets, std::unique_ptr<IAudioBackend> Backend, DiagnosticCallback Diagnostic
	)
		: State(std::make_unique<Impl>(std::move(Assets), std::move(Backend), std::move(Diagnostic))) {}

	AudioRuntime::~AudioRuntime() {
		Shutdown();
	}

	void AudioRuntime::RegisterSound(const std::shared_ptr<Sound> &SoundValue) {
		if (!State || State->ShutDown || !SoundValue || SoundValue->GetDestroyed() || SoundValue->IsDestroying())
			return;
		State->Sounds.insert_or_assign(SoundValue->GetObjectId(), Impl::TrackedSound{SoundValue});
	}

	void AudioRuntime::Step(const CFrame &Listener) {
		if (!State || State->ShutDown) return;
		State->Reconcile();
		if (!State->Backend || !State->Backend->IsAvailable() || State->ActiveVoiceCount() == 0) {
			State->Metrics.ActiveVoices = State->ActiveVoiceCount();
			if (State->Backend) State->Metrics.Backend = State->Backend->GetMetrics();
			return;
		}
		auto QueuedFrames = State->Backend->GetQueuedFrames();
		for (std::size_t Block = 0; Block < MaximumBlocksPerStep && QueuedFrames < TargetQueuedFrames; ++Block) {
			State->Mix(Listener);
			if (!State->Backend->Submit(State->MixBuffer)) {
				State->FailVoices();
				break;
			}
			QueuedFrames += MixBlockFrames;
		}
		State->Metrics.ActiveVoices = State->ActiveVoiceCount();
		State->Metrics.Backend = State->Backend->GetMetrics();
	}

	void AudioRuntime::Shutdown() {
		if (!State || State->ShutDown) return;
		State->ShutDown = true;
		for (auto &[Id, Tracked] : State->Sounds) {
			(void)Id;
			if (auto SoundValue = Tracked.Value.lock()) SoundValue->RejectRuntimePlayback();
		}
		State->Sounds.clear();
		if (State->Backend) {
			State->Backend->Clear();
			State->Backend->Shutdown();
			State->Metrics.Backend = State->Backend->GetMetrics();
		}
		State->Metrics.ActiveVoices = 0;
	}

	bool AudioRuntime::IsAvailable() const {
		return State && !State->ShutDown && State->Backend && State->Backend->IsAvailable();
	}

	AudioRuntimeMetrics AudioRuntime::GetMetrics() const {
		return State ? State->Metrics : AudioRuntimeMetrics{};
	}
}

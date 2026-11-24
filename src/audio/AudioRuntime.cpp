#include "gargantuan/audio/AudioRuntime.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/AudioMixService.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		constexpr float Pi = 3.14159265358979323846f;

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		std::optional<glm::vec3> ResolveSoundPosition(
			const std::shared_ptr<Sound> &SoundValue,
			const std::shared_ptr<SemanticSpatialResolver> &Spatial
		) {
			auto Transform = Spatial ? Spatial->ResolveWorldTransform(SoundValue)
				: SemanticSpatialResolver::ResolveStaticWorldTransform(SoundValue);
			if (!Transform || !IsFinite(Transform->WorldCFrame.Position)) return std::nullopt;
			return Transform->WorldCFrame.Position;
		}
	}

	struct AudioRuntime::Impl {
		static constexpr AudioMixService::EffectiveGains UnityGains = [] {
			AudioMixService::EffectiveGains Gains;
			Gains.fill(1.0f);
			return Gains;
		}();

		struct Voice {
			AssetAudioResource Resource;
			double FramePosition = 0.0;
			std::string Reference;
			bool HasMixed = false;
		};

		struct TrackedSound {
			std::weak_ptr<Sound> Value;
			std::uint64_t ObservedPlayGeneration = 0;
			std::uint64_t ObservedSeekGeneration = 0;
			std::optional<Voice> ActiveVoice;
		};

		std::shared_ptr<AssetService> Assets;
		std::shared_ptr<SemanticSpatialResolver> Spatial;
		std::unique_ptr<IAudioBackend> Backend;
		std::weak_ptr<AudioMixService> MixService;
		DiagnosticCallback Diagnostic;
		std::map<ObjectId, TrackedSound> Sounds;
		std::set<std::pair<ObjectId, std::string>> EmittedDiagnostics;
		std::array<float, AudioRuntime::MixBlockFrames * 2> MixBuffer{};
		AudioMixService::EffectiveGains AppliedGains = UnityGains;
		std::array<std::array<float, AudioRuntime::MixBlockFrames>, AudioMixService::CategoryCount> GainRamps;
		std::vector<ObjectId> Completed;
		AudioRuntimeMetrics Metrics;
		std::uint64_t AssetChangeSequence = 1;
		bool ShutDown = false;

		Impl(
			std::shared_ptr<AssetService> AssetsValue,
			std::unique_ptr<IAudioBackend> BackendValue,
			DiagnosticCallback DiagnosticValue,
			std::shared_ptr<SemanticSpatialResolver> SpatialValue
		)
			: Assets(std::move(AssetsValue)),
			  Spatial(SpatialValue ? std::move(SpatialValue) : std::make_shared<SemanticSpatialResolver>(Assets)),
			  Backend(std::move(BackendValue)), Diagnostic(std::move(DiagnosticValue)) {
			Completed.reserve(AudioRuntime::MaximumVoices);
			if (Assets) AssetChangeSequence = Assets->ReadChanges(0).NextSequence;
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
			auto AssetChanges = Assets ? Assets->ReadChanges(AssetChangeSequence) : AssetChangeBatch{};
			AssetChangeSequence = AssetChanges.NextSequence;
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

				const bool AudioChanged = Tracked.ActiveVoice && (AssetChanges.RescanRequired ||
					std::ranges::any_of(AssetChanges.Changes, [&](const AssetChange &Change) {
						return Change.Kind == AssetKind::Audio && Change.Reference == Tracked.ActiveVoice->Reference;
					}));
				if (AudioChanged) {
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

		void Mix(const CFrame &Listener, const AudioMixService::EffectiveGains &TargetGains) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::AudioMix);
			const auto Started = std::chrono::steady_clock::now();
			MixBuffer.fill(0.0f);
			Completed.clear();
			for (std::size_t Index = 0; Index < TargetGains.size(); ++Index) {
				if (AppliedGains[Index] == TargetGains[Index]) continue;
				for (std::size_t Frame = 0; Frame < AudioRuntime::MixBlockFrames; ++Frame) {
					const float Fraction = static_cast<float>(Frame) / static_cast<float>(AudioRuntime::MixBlockFrames - 1);
					GainRamps[Index][Frame] = std::lerp(AppliedGains[Index], TargetGains[Index], Fraction);
				}
			}
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
				const auto Category = static_cast<std::size_t>(SoundValue->GetMixCategory());
				const bool RampExistingVoice =
					VoiceValue.HasMixed && AppliedGains[Category] != TargetGains[Category];
				const auto *Ramp = RampExistingVoice ? GainRamps[Category].data() : nullptr;
				VoiceValue.HasMixed = true;
				float LeftGain = SoundValue->GetVolume();
				if (!Ramp) LeftGain *= TargetGains[Category];
				float RightGain = LeftGain;
				const auto SemanticStarted = std::chrono::steady_clock::now();
				const auto Position = ResolveSoundPosition(SoundValue, Spatial);
				++Metrics.SemanticSourceResolutions;
				Metrics.SemanticSourceCpuNanoseconds += static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - SemanticStarted).count());
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

				const bool Looped = SoundValue->GetLooped();
				const bool Spatialized = Position.has_value();
				// Pick the gain path once per voice so a steady voice has no ramp check per sample.
				const auto MixFrames = [&]<bool ApplyRamp>() {
					for (std::size_t Frame = 0; Frame < AudioRuntime::MixBlockFrames; ++Frame) {
						if (VoiceValue.FramePosition >= Audio.FrameCount) {
							if (!Looped) return true;
							VoiceValue.FramePosition = std::fmod(VoiceValue.FramePosition, Audio.FrameCount);
						}
						auto [Left, Right] = ReadFrame(Audio, VoiceValue.FramePosition, Looped);
						if (Spatialized) {
							const auto Mono = (Left + Right) * 0.5f;
							Left = Mono;
							Right = Mono;
						}
						if constexpr (ApplyRamp) {
							Left *= Ramp[Frame];
							Right *= Ramp[Frame];
						}
						MixBuffer[Frame * 2] += Left * LeftGain;
						MixBuffer[Frame * 2 + 1] += Right * RightGain;
						VoiceValue.FramePosition += Step;
					}
					return false;
				};
				const bool Ended = Ramp ? MixFrames.operator()<true>() : MixFrames.operator()<false>();
				if (Ended)
					Completed.push_back(Id);
				else
					SoundValue->SetRuntimeTimePosition(
						static_cast<float>(VoiceValue.FramePosition / Audio.SampleRate), false
					);
			}

			AppliedGains = TargetGains;
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
		std::shared_ptr<AssetService> Assets,
		std::unique_ptr<IAudioBackend> Backend,
		DiagnosticCallback Diagnostic,
		std::shared_ptr<SemanticSpatialResolver> Spatial
	)
		: State(std::make_unique<Impl>(
			std::move(Assets), std::move(Backend), std::move(Diagnostic), std::move(Spatial))) {}

	AudioRuntime::~AudioRuntime() {
		Shutdown();
	}

	void AudioRuntime::AttachMixService(const std::shared_ptr<AudioMixService> &Service, RuntimeMode Mode) {
		AssertAuthoritativeMutation("[Audio:Mix] Attach local mix");
		if (Mode != RuntimeMode::Offline && Mode != RuntimeMode::NetworkClient)
			throw std::invalid_argument("[Audio:Mix] Runtime mode has no local presentation");
		if (!State || State->ShutDown || !State->MixService.expired())
			throw std::logic_error("[Audio:Mix] Runtime already has a mix or is stopped");
		const auto World = Service ? Service->GetDataModel() : nullptr;
		const auto Parent = Service ? Service->GetParent() : std::nullopt;
		if (!World || !Parent || *Parent != World || !State->Assets || State->Assets->GetDataModel() != World ||
			Service->GetDestroyed() || Service->IsDestroying())
			throw std::invalid_argument("[Audio:Mix] Mix service must belong to the runtime's DataModel");
		Service->AttachRuntime();
		State->MixService = Service;
	}

	void AudioRuntime::RegisterSound(const std::shared_ptr<Sound> &SoundValue) {
		if (!State || State->ShutDown || !SoundValue || SoundValue->GetDestroyed() || SoundValue->IsDestroying())
			return;
		State->Sounds.insert_or_assign(SoundValue->GetObjectId(), Impl::TrackedSound{SoundValue});
	}

	void AudioRuntime::Step(const CFrame &Listener) {
		if (!State || State->ShutDown) return;
		runtime_detail::MeasureWork(runtime_detail::WorkPhase::AudioReconcile, [&] { State->Reconcile(); });
		const auto Service = State->MixService.lock();
		const auto TargetGains = Service ? Service->CaptureEffectiveGains() : Impl::UnityGains;
		if (!State->Backend || !State->Backend->IsAvailable() || State->ActiveVoiceCount() == 0) {
			State->AppliedGains = TargetGains;
			State->Metrics.ActiveVoices = State->ActiveVoiceCount();
			if (State->Backend) State->Metrics.Backend = State->Backend->GetMetrics();
			return;
		}
		auto QueuedFrames = State->Backend->GetQueuedFrames();
		for (std::size_t Block = 0; Block < MaximumBlocksPerStep && QueuedFrames < TargetQueuedFrames; ++Block) {
			State->Mix(Listener, TargetGains);
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
		if (auto Service = State->MixService.lock()) Service->DetachRuntime();
		State->MixService.reset();
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

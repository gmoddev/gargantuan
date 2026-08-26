#include "gargantuan/classes/Sound.hpp"

#include "gargantuan/assets/AssetTypes.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	void Sound::AdvanceGeneration(std::uint64_t &Generation) {
		if (Generation == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Sound playback command generation is exhausted");
		++Generation;
	}

	std::string Sound::GetSoundId() const {
		return SoundId;
	}

	void Sound::SetSoundId(std::string Value) {
		AssertCanMutate();
		if (!Value.empty() && !AssetReference::Parse(Value))
			throw std::invalid_argument("[Audio:Sound] SoundId requires a strict asset:// or builtin:// reference");
		ValidatePropertyMutation("SoundId", Value);
		if (SoundId == Value) return;
		SoundId = std::move(Value);
		NotifyPropertyCommitted("SoundId");
		if (PlaybackState != Enums::SoundPlaybackState::Stopped) {
			SetRuntimeTimePosition(0.0f, true);
			AdvanceGeneration(PlayGeneration);
		}
	}

	float Sound::GetTimePosition() const {
		return TimePosition;
	}
	float Sound::GetRollOffMinDistance() const {
		return RollOffMinDistance;
	}
	float Sound::GetRollOffMaxDistance() const {
		return RollOffMaxDistance;
	}

	void Sound::SetTimePosition(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("TimePosition", Value);
		SetRuntimeTimePosition(std::clamp(Value, 0.0f, MaximumTimePosition), true);
		AdvanceGeneration(SeekGeneration);
	}

	void Sound::SetRollOffMinDistance(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("RollOffMinDistance", Value);
		if (!std::isfinite(Value) || Value < 0.0f || Value >= RollOffMaxDistance)
			throw std::invalid_argument(
				"[Audio:Spatial] RollOffMinDistance must be finite, nonnegative, and below RollOffMaxDistance"
			);
		if (RollOffMinDistance == Value) return;
		RollOffMinDistance = Value;
		NotifyPropertyCommitted("RollOffMinDistance");
	}

	void Sound::SetRollOffMaxDistance(float Value) {
		AssertCanMutate();
		ValidatePropertyMutation("RollOffMaxDistance", Value);
		if (!std::isfinite(Value) || Value <= RollOffMinDistance || Value > MaximumDistance)
			throw std::invalid_argument(
				"[Audio:Spatial] RollOffMaxDistance must be finite, above RollOffMinDistance, and bounded"
			);
		if (RollOffMaxDistance == Value) return;
		RollOffMaxDistance = Value;
		NotifyPropertyCommitted("RollOffMaxDistance");
	}

	void Sound::SetRuntimePlaybackState(Enums::SoundPlaybackState State) {
		if (PlaybackState == State) return;
		PlaybackState = State;
		GetPropertyChangedSignal("PlaybackState")->Fire({});
	}

	void Sound::SetRuntimeTimePosition(float Seconds, bool Notify) {
		Seconds = std::clamp(Seconds, 0.0f, MaximumTimePosition);
		if (TimePosition == Seconds) return;
		TimePosition = Seconds;
		if (Notify) GetPropertyChangedSignal("TimePosition")->Fire({});
	}

	void Sound::Play() {
		AssertCanMutate();
		if (PlaybackState != Enums::SoundPlaybackState::Stopped) SetRuntimeTimePosition(0.0f, true);
		AdvanceGeneration(PlayGeneration);
		SetRuntimePlaybackState(Enums::SoundPlaybackState::Playing);
	}

	void Sound::Pause() {
		AssertCanMutate();
		if (PlaybackState == Enums::SoundPlaybackState::Playing)
			SetRuntimePlaybackState(Enums::SoundPlaybackState::Paused);
	}

	void Sound::Resume() {
		AssertCanMutate();
		if (PlaybackState == Enums::SoundPlaybackState::Paused)
			SetRuntimePlaybackState(Enums::SoundPlaybackState::Playing);
	}

	void Sound::Stop() {
		AssertCanMutate();
		SetRuntimePlaybackState(Enums::SoundPlaybackState::Stopped);
		SetRuntimeTimePosition(0.0f, true);
	}

	void Sound::CompleteRuntimePlayback() {
		if (PlaybackState == Enums::SoundPlaybackState::Stopped) return;
		SetRuntimePlaybackState(Enums::SoundPlaybackState::Stopped);
		SetRuntimeTimePosition(0.0f, true);
		Ended->Fire({});
	}

	void Sound::RejectRuntimePlayback() {
		SetRuntimePlaybackState(Enums::SoundPlaybackState::Stopped);
		SetRuntimeTimePosition(0.0f, true);
	}
}

#include "gargantuan/animation/AnimationTrack.hpp"

#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/scripting/UserdataTag.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	G_USERDATA_IMPL(
		AnimationTrack,
		.Tag = UserdataTag::AnimationTrack,
		.Type = "AnimationTrack",
		.Properties = {
			{"Duration", Property::fromRead([](AnimationTrack *self) { return self->GetDuration(); })},
			{"Ended", Property::fromReadonlyMember<&AnimationTrack::Ended>()},
			{"Looped", Property::fromReadWrite<bool>(
				[](AnimationTrack *self) { return self->GetLooped(); },
				[](AnimationTrack *self, bool Value) { self->SetLooped(Value); })},
			{"PlaybackState", Property::fromRead([](AnimationTrack *self) { return self->GetPlaybackState(); })},
			{"Speed", Property::fromReadWrite<float>(
				[](AnimationTrack *self) { return self->GetSpeed(); },
				[](AnimationTrack *self, float Value) { self->SetSpeed(Value); })},
			{"TimePosition", Property::fromReadWrite<float>(
				[](AnimationTrack *self) { return self->GetTimePosition(); },
				[](AnimationTrack *self, float Value) { self->SetTimePosition(Value); })},
			{"Weight", Property::fromReadWrite<float>(
				[](AnimationTrack *self) { return self->GetWeight(); },
				[](AnimationTrack *self, float Value) { self->SetWeight(Value); })},
		},
		.Methods = {
			{"AdjustSpeed", Method::fromCheckedMember<&AnimationTrack::AdjustSpeed>()},
			{"AdjustWeight", Method::fromCheckedMember<&AnimationTrack::AdjustWeight>()},
			{"Pause", Method::fromMember<&AnimationTrack::Pause>()},
			{"Play", Method::fromMember<&AnimationTrack::Play>()},
			{"Resume", Method::fromMember<&AnimationTrack::Resume>()},
			{"Stop", Method::fromMember<&AnimationTrack::Stop>()},
		}
	);

	AnimationTrack::AnimationTrack(
		std::weak_ptr<Animator> OwnerValue,
		AssetAnimationResource ResourceValue,
		std::vector<std::int32_t> JointTrackIndicesValue,
		std::uint64_t CreationSequenceValue
	)
		: Owner(std::move(OwnerValue)), Resource(std::move(ResourceValue)),
		  JointTrackIndices(std::make_shared<const std::vector<std::int32_t>>(std::move(JointTrackIndicesValue))),
		  CreationSequence(CreationSequenceValue) {
		if (!Resource.Value.Tracks || Resource.Value.Tracks->empty() || Resource.Value.Duration <= 0.0f ||
			!JointTrackIndices || JointTrackIndices->empty() || CreationSequence == 0)
			throw std::invalid_argument("[Animation:Track] canonical track construction is invalid");
	}

	void AnimationTrack::MarkChanged(bool ControlChange) {
		if (Revision == std::numeric_limits<std::uint64_t>::max() ||
			(ControlChange && ControlRevision == std::numeric_limits<std::uint64_t>::max()))
			throw std::overflow_error("[Animation:Track] runtime revision is exhausted");
		++Revision;
		if (ControlChange) ++ControlRevision;
	}

	void AnimationTrack::SetLooped(bool Value) {
		if (Invalidated || Looped == Value) return;
		Looped = Value;
		MarkChanged();
	}

	void AnimationTrack::SetSpeed(float Value) {
		if (!std::isfinite(Value) || Value < 0.0f || Value > MaximumSpeed)
			throw std::invalid_argument("[Animation:Track] Speed must be finite and within 0..16");
		if (Invalidated || Speed == Value) return;
		Speed = Value;
		MarkChanged();
	}

	void AnimationTrack::SetTimePosition(float Value) {
		if (!std::isfinite(Value))
			throw std::invalid_argument("[Animation:Track] TimePosition must be finite");
		if (Invalidated) return;
		Value = std::clamp(Value, 0.0f, GetDuration());
		if (TimePosition == Value && !NaturalEndPose) return;
		TimePosition = Value;
		NaturalEndPose = false;
		PendingEnded = false;
		MarkChanged();
	}

	void AnimationTrack::SetWeight(float Value) {
		if (!std::isfinite(Value) || Value < 0.0f || Value > 1.0f)
			throw std::invalid_argument("[Animation:Track] Weight must be finite and within 0..1");
		if (Invalidated || Weight == Value) return;
		Weight = Value;
		MarkChanged();
	}

	void AnimationTrack::Play() {
		auto OwnerValue = Owner.lock();
		if (Invalidated || !OwnerValue || OwnerValue->GetDestroyed() || OwnerValue->IsDestroying()) return;
		TimePosition = 0.0f;
		NaturalEndPose = false;
		PendingEnded = false;
		PlaybackState = Enums::AnimationPlaybackState::Playing;
		MarkChanged();
	}

	void AnimationTrack::Pause() {
		if (Invalidated || PlaybackState != Enums::AnimationPlaybackState::Playing) return;
		PlaybackState = Enums::AnimationPlaybackState::Paused;
		MarkChanged();
	}

	void AnimationTrack::Resume() {
		auto OwnerValue = Owner.lock();
		if (Invalidated || !OwnerValue || OwnerValue->GetDestroyed() || OwnerValue->IsDestroying() ||
			PlaybackState != Enums::AnimationPlaybackState::Paused) return;
		PlaybackState = Enums::AnimationPlaybackState::Playing;
		MarkChanged();
	}

	void AnimationTrack::Stop() {
		if (Invalidated) return;
		if (PlaybackState == Enums::AnimationPlaybackState::Stopped && TimePosition == 0.0f && !NaturalEndPose) return;
		PlaybackState = Enums::AnimationPlaybackState::Stopped;
		TimePosition = 0.0f;
		NaturalEndPose = false;
		PendingEnded = false;
		MarkChanged();
	}

	bool AnimationTrack::AdvanceRuntime(float DeltaTime) {
		if (Invalidated || PlaybackState != Enums::AnimationPlaybackState::Playing ||
			!std::isfinite(DeltaTime) || DeltaTime <= 0.0f || Speed == 0.0f) return false;
		const auto Duration = static_cast<double>(GetDuration());
		const auto Advanced = static_cast<double>(TimePosition) + static_cast<double>(DeltaTime) * Speed;
		if (Looped) {
			TimePosition = static_cast<float>(std::fmod(Advanced, Duration));
			NaturalEndPose = false;
			MarkChanged(false);
			return true;
		}
		if (Advanced >= Duration) {
			TimePosition = GetDuration();
			PlaybackState = Enums::AnimationPlaybackState::Stopped;
			NaturalEndPose = true;
			PendingEnded = true;
			MarkChanged(false);
			return true;
		}
		TimePosition = static_cast<float>(Advanced);
		MarkChanged(false);
		return true;
	}

	void AnimationTrack::FirePendingEndedRuntime() {
		if (!PendingEnded || Invalidated) return;
		PendingEnded = false;
		Ended->Fire({});
	}

	void AnimationTrack::InvalidateRuntime() {
		if (Invalidated) return;
		Invalidated = true;
		PlaybackState = Enums::AnimationPlaybackState::Stopped;
		TimePosition = 0.0f;
		NaturalEndPose = false;
		PendingEnded = false;
		Owner.reset();
	}
}

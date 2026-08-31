#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/datatypes/Signal.hpp"
#include "gargantuan/reflection/Enums.hpp"
#include "gargantuan/scripting/Userdata.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace gargantuan {
	G_ENUM(AnimationPlaybackState, Stopped, Playing, Paused);

	class Animator;
	class AnimationRuntime;

	G_SHARED_USERDATA_DECL(
		AnimationTrack,
		friend class Animator;
		friend class AnimationRuntime;

	  private:
		std::weak_ptr<Animator> Owner;
		AssetAnimationResource Resource;
		std::shared_ptr<const std::vector<std::int32_t>> JointTrackIndices;
		std::uint64_t CreationSequence = 0;
		std::uint64_t Revision = 1;
		std::uint64_t ControlRevision = 1;
		float TimePosition = 0.0f;
		float Speed = 1.0f;
		float Weight = 1.0f;
		bool Looped = false;
		Enums::AnimationPlaybackState PlaybackState = Enums::AnimationPlaybackState::Stopped;
		bool NaturalEndPose = false;
		bool PendingEnded = false;
		bool Invalidated = false;

		void MarkChanged(bool ControlChange = true);
		[[nodiscard]] bool AdvanceRuntime(float DeltaTime);
		void FirePendingEndedRuntime();
		void InvalidateRuntime();

	  public:
		static constexpr float MaximumSpeed = 16.0f;

		AnimationTrack(
			std::weak_ptr<Animator> OwnerValue,
			AssetAnimationResource ResourceValue,
			std::vector<std::int32_t> JointTrackIndicesValue,
			std::uint64_t CreationSequenceValue
		);

		[[nodiscard]] float GetDuration() const { return Resource.Value.Duration; }
		[[nodiscard]] bool GetLooped() const { return Looped; }
		void SetLooped(bool Value);
		[[nodiscard]] Enums::AnimationPlaybackState GetPlaybackState() const { return PlaybackState; }
		[[nodiscard]] float GetSpeed() const { return Speed; }
		void SetSpeed(float Value);
		[[nodiscard]] float GetTimePosition() const { return TimePosition; }
		void SetTimePosition(float Value);
		[[nodiscard]] float GetWeight() const { return Weight; }
		void SetWeight(float Value);
		[[nodiscard]] bool HoldsNaturalEndPose() const { return NaturalEndPose; }

		void Play();
		void Pause();
		void Resume();
		void Stop();
		void AdjustSpeed(float Value) { SetSpeed(Value); }
		void AdjustWeight(float Value) { SetWeight(Value); }

		std::shared_ptr<Signal<std::monostate>> Ended = std::make_shared<Signal<std::monostate>>();
	);
}

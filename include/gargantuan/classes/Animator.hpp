#pragma once

#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/generated/Animator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace gargantuan {
	class AnimationRuntime;

	class Animator final : public Instance {
		I_Animator;

		friend class AnimationRuntime;
		friend class AnimationTrack;

		std::vector<std::shared_ptr<AnimationTrack>> Tracks;
		std::weak_ptr<AnimationTrack> RootMotionTrack;
		std::uint64_t NextCreationSequence = 1;

		void InvalidateTracks();
		void SetRootMotionSource(AnimationTrack &Track, bool Enabled);

	  public:
		static constexpr std::size_t MaximumTracks = 16;

		Animator();
		~Animator() override;
		[[nodiscard]] std::shared_ptr<AnimationTrack> CreateTrack(std::string_view AnimationReference);
	};
}

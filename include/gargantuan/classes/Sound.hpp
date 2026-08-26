#pragma once

#include "gargantuan/classes/generated/Sound.hpp"
#include "gargantuan/reflection/Enums.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace gargantuan {
	G_ENUM(SoundPlaybackState, Stopped, Playing, Paused);

	class AudioRuntime;

	class Sound : public Instance {
		I_Sound;

		friend class AudioRuntime;

		std::string SoundId;
		float TimePosition = 0.0f;
		float RollOffMinDistance = 10.0f;
		float RollOffMaxDistance = 100.0f;
		std::uint64_t PlayGeneration = 0;
		std::uint64_t SeekGeneration = 0;

		void AdvanceGeneration(std::uint64_t &Generation);
		void SetRuntimePlaybackState(Enums::SoundPlaybackState State);
		void SetRuntimeTimePosition(float Seconds, bool Notify);
		void CompleteRuntimePlayback();
		void RejectRuntimePlayback();

	  public:
		static constexpr float MaximumDistance = 100'000.0f;
		static constexpr float MaximumTimePosition = 30.0f;
	};
}

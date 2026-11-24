#pragma once

#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/services/generated/AudioMixService.hpp"

#include <array>
#include <cstddef>

namespace gargantuan {
	class AudioMixService final : public Instance {
		I_AudioMixService;

	  public:
		[[nodiscard]] double GetMasterGain() const;
		void SetMasterGain(double Gain);
		[[nodiscard]] bool GetMasterMuted() const;
		void SetMasterMuted(bool Muted);
		[[nodiscard]] double GetCategoryGain(Enums::SoundMixCategory Category) const;
		void SetCategoryGain(Enums::SoundMixCategory Category, double Gain);
		[[nodiscard]] bool GetCategoryMuted(Enums::SoundMixCategory Category) const;
		void SetCategoryMuted(Enums::SoundMixCategory Category, bool Muted);

	  private:
		friend class AudioRuntime;
		friend class ServiceProvider;

		static constexpr std::size_t CategoryCount = magic_enum::enum_count<Enums::SoundMixCategory>();
		using EffectiveGains = std::array<float, CategoryCount>;

		struct MixControl {
			double Gain = 1.0;
			bool Muted = false;
		};

		MixControl Master;
		std::array<MixControl, CategoryCount> Categories{};
		bool RuntimeAttached = false;

		[[nodiscard]] static std::size_t CategoryIndex(Enums::SoundMixCategory Category);
		[[nodiscard]] bool HasAttachedRuntime() const;
		void RequireAccess(ScriptCapability Capability) const;
		void AttachRuntime();
		void DetachRuntime() noexcept;
		[[nodiscard]] EffectiveGains CaptureEffectiveGains() const;
	};
}

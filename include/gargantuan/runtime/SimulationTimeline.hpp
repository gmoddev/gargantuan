// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <optional>

namespace gargantuan {
	// One timeline per session. The trusted host applies the aggregate gameplay
	// hold at a completed step boundary. Debugger parking is independent.
	class SimulationTimeline final {
	  public:
		using Clock = std::chrono::steady_clock;
		enum class Hold { Gameplay, Debugger };

		[[nodiscard]] bool IsHeld(Hold Reason) const noexcept {
			return Reason == Hold::Gameplay ? GameplayHeld : DebuggerHeld;
		}

		void SetHeld(Hold Reason, bool Held, Clock::time_point WallNow = Clock::now()) noexcept {
			auto &Current = Reason == Hold::Gameplay ? GameplayHeld : DebuggerHeld;
			if (Current == Held) return;
			Current = Held;
			if (GameplayHeld || DebuggerHeld) {
				if (!ParkedAt) ParkedAt = WallNow;
			} else {
				ParkedDuration += WallNow - *ParkedAt;
				ParkedAt.reset();
			}
		}

		[[nodiscard]] Clock::time_point Now(Clock::time_point WallNow = Clock::now()) const noexcept {
			return ParkedAt.value_or(WallNow) - ParkedDuration;
		}

	  private:
		bool GameplayHeld = false;
		bool DebuggerHeld = false;
		std::optional<Clock::time_point> ParkedAt;
		Clock::duration ParkedDuration{};
	};
}

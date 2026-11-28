// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/SimulationTimeline.hpp"

#include <iostream>

int main() {
	using namespace std::chrono_literals;
	using Timeline = gargantuan::SimulationTimeline;
	using Hold = Timeline::Hold;
	const Timeline::Clock::time_point Start{};
	for (const Hold First : {Hold::Gameplay, Hold::Debugger}) {
		const Hold Second = First == Hold::Gameplay ? Hold::Debugger : Hold::Gameplay;
		Timeline Session;
		Session.SetHeld(First, false, Start);
		Session.SetHeld(First, true, Start + 7ms);
		Session.SetHeld(First, true, Start + 1s);
		Session.SetHeld(Second, true, Start + 2s);
		Session.SetHeld(First, false, Start + 3s);
		if (Session.IsHeld(First) || !Session.IsHeld(Second) || Session.Now(Start + 48h) != Start + 7ms) {
			std::cerr << "[Lifecycle:Timeline] Overlapping holds advanced simulation\n";
			return 1;
		}
		Session.SetHeld(Second, false, Start + 48h);
		Session.SetHeld(Second, false, Start + 48h + 1ms);
		Session.SetHeld(First, true, Start + 48h + 2ms);
		Session.SetHeld(First, false, Start + 49h);
		if (Session.Now(Start + 49h + 4ms) != Start + 13ms) {
			std::cerr << "[Lifecycle:Timeline] Resume accumulated wall-clock debt\n";
			return 1;
		}
	}
	return 0;
}

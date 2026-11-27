#pragma once

#include <lua.h>
#include <chrono>

namespace gargantuan {
	struct GameplayResumeBudget {
		std::chrono::steady_clock::duration Remaining;
	};
	int ResumeGameplay(lua_State *Thread, lua_State *From, int ArgumentCount, GameplayResumeBudget *Retained = nullptr);
	int CallGameplay(lua_State *State, int ArgumentCount, int ResultCount);
}

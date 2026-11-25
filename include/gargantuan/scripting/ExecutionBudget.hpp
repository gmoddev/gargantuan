#pragma once

#include <lua.h>

namespace gargantuan {
	int ResumeGameplay(lua_State *Thread, lua_State *From, int ArgumentCount);
	int CallGameplay(lua_State *State, int ArgumentCount, int ResultCount);
}

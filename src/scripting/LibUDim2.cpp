// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/datatypes/UDim2.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <lualib.h>

namespace gargantuan {
	int LibUDim2_new(lua_State *L) {
		StackValue<UDim2>::Push(L, {
			static_cast<float>(luaL_optnumber(L, 1, 0.0f)), luaL_optinteger(L, 2, 0),
			static_cast<float>(luaL_optnumber(L, 3, 0.0f)), luaL_optinteger(L, 4, 0),
		});
		return 1;
	}

	int LibUDim2_fromScale(lua_State *L) {
		StackValue<UDim2>::Push(L, {static_cast<float>(luaL_checknumber(L, 1)), 0,
			static_cast<float>(luaL_checknumber(L, 2)), 0});
		return 1;
	}

	int LibUDim2_fromOffset(lua_State *L) {
		StackValue<UDim2>::Push(L, {0.0f, luaL_checkinteger(L, 1), 0.0f, luaL_checkinteger(L, 2)});
		return 1;
	}

	luaL_Reg LibUDim2[]{
		{"new", LibUDim2_new},
		{"fromScale", LibUDim2_fromScale},
		{"fromOffset", LibUDim2_fromOffset},
		{nullptr, nullptr},
	};

	int OpenLibUDim2(lua_State *L) {
		luaL_register(L, "UDim2", LibUDim2);
		return 0;
	}
}

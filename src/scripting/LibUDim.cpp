#include "gargantuan/datatypes/UDim.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <lualib.h>

namespace gargantuan {
	int LibUDim_new(lua_State *L) {
		float scale = luaL_optnumber(L, 1, 0.0f);
		int offset = luaL_optinteger(L, 2, 0);
		StackValue<UDim>::Push(L, {scale, offset});
		return 1;
	}

	int LibUDim_fromScale(lua_State *L) {
		StackValue<UDim>::Push(L, {static_cast<float>(luaL_checknumber(L, 1)), 0});
		return 1;
	}

	int LibUDim_fromOffset(lua_State *L) {
		StackValue<UDim>::Push(L, {0.0f, luaL_checkinteger(L, 1)});
		return 1;
	}

	luaL_Reg LibUDim[]{
		{"new", LibUDim_new},
		{"fromScale", LibUDim_fromScale},
		{"fromOffset", LibUDim_fromOffset},
		{nullptr, nullptr},
	};

	int OpenLibUDim(lua_State *L) {
		luaL_register(L, "UDim", LibUDim);
		return 0;
	}
} // namespace gargantuan

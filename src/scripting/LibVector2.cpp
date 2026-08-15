#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <lualib.h>

namespace gargantuan {
	int LibVector2_new(lua_State *L) {
		float x = luaL_optnumber(L, 1, 0.0f);
		float y = luaL_optnumber(L, 2, 0.0f);
		StackValue<Vector2>::Push(L, Vector2(x, y));
		return 1;
	}

	luaL_Reg LibVector2[] = {
		{"new", LibVector2_new},
		{nullptr, nullptr},
	};

	int OpenLibVector2(lua_State *L) {
		luaL_register(L, "Vector2", LibVector2);

		StackValue<Vector2>::Push(L, Vector2(0.0f, 0.0f));
		lua_setfield(L, -2, "zero");
		StackValue<Vector2>::Push(L, Vector2(1.0f, 1.0f));
		lua_setfield(L, -2, "one");
		StackValue<Vector2>::Push(L, Vector2(1.0f, 0.0f));
		lua_setfield(L, -2, "xAxis");
		StackValue<Vector2>::Push(L, Vector2(0.0f, 1.0f));
		lua_setfield(L, -2, "yAxis");

		lua_setreadonly(L, -1, true);
		lua_pop(L, 1);
		return 0;
	}
} // namespace gargantuan

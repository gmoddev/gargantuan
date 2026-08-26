#include "gargantuan/datatypes/RaycastParams.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	namespace {
		int LibRaycastParamsNew(lua_State *L) {
			StackValue<RaycastParams>::Push(L, RaycastParams{});
			return 1;
		}

		luaL_Reg LibRaycastParams[]{{"new", LibRaycastParamsNew}, {nullptr, nullptr}};
	}

	int OpenLibRaycastParams(lua_State *L) {
		luaL_register(L, "RaycastParams", LibRaycastParams);
		return 0;
	}
}

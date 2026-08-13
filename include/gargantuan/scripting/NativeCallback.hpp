#pragma once

#include <exception>
#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	template <typename Callback> int InvokeNativeCallback(lua_State *L, Callback &&callback) {
		try {
			return callback();
		} catch (const std::exception &exception) {
			lua_pushstring(L, exception.what());
		} catch (...) {
			lua_pushstring(L, "Unknown native error");
		}
		lua_error(L);
		return 0;
	}
}

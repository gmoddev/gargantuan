#pragma once

#include <lua.h>

namespace gargantuan {
	// A debugger turn is synchronous and can park only the selected root VM frame.
	// The owning Script keeps the coroutine rooted between turns.
	class RootScriptDebugTurn final {
	  public:
		RootScriptDebugTurn(lua_State *Thread, int PreviousLine);
		~RootScriptDebugTurn();
		RootScriptDebugTurn(const RootScriptDebugTurn &) = delete;
		RootScriptDebugTurn &operator=(const RootScriptDebugTurn &) = delete;

		[[nodiscard]] int GetLine() const { return Line; }

	  private:
		static void Step(lua_State *Thread, lua_Debug *Frame);
		static thread_local RootScriptDebugTurn *Active;
		lua_State *Thread;
		lua_Hook Prior;
		int PreviousLine;
		int Line = 0;
	};
}

#include "gargantuan/services/ProcessService.hpp"

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <iostream>
#include <lua.h>
#include <lualib.h>

#include <stdexcept>

namespace gargantuan {
	namespace {
		void RequireProcessCapability() {
			if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::ProcessControl))
				throw std::runtime_error("ProcessService requires ProcessControl");
		}
	}

	void ProcessService::MarkExit(int exitCode) {
		Alive = false;
		ExitCode = exitCode;
	}

	int ProcessService::ExitAsync(lua_State *L, Instance *self) {
		RequireProcessCapability();
		auto processService = dynamic_cast<ProcessService *>(self);
		int exitCode = luaL_checknumber(L, 2);
		if (processService->Alive) processService->MarkExit(exitCode);
		return lua_yield(L, 0);
	}

	int ProcessService::WriteToStdout(lua_State *L, Instance *self) {
		RequireProcessCapability();
		(void)self;
		auto numArguments = lua_gettop(L);
		for (int idx = 2; idx <= numArguments; idx++) {
			auto str = luaL_checkstring(L, idx);
			std::cout << str;
		}
		return 0;
	}

	void ProcessService::FlushStdout() {
		RequireProcessCapability();
		std::cout << std::flush;
	}
}

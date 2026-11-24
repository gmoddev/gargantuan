#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ThreadEngine.hpp"

#include <cmath>
#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	lua_State *GetThreadFromArgument(lua_State *L, int startIdx, int &argumentsToPass) {
		int argumentCount = lua_gettop(L);

		if (argumentCount < startIdx) {
			luaL_typeerror(L, startIdx, "function or thread");
			return 0;
		}

		startIdx = lua_absindex(L, startIdx);

		lua_State *targetThread = nullptr;
		argumentsToPass = argumentCount - startIdx;

		if (lua_isfunction(L, startIdx)) {
			targetThread = lua_newthread(L);
			lua_pushvalue(L, startIdx);
			lua_xmove(L, targetThread, 1);
		} else if (lua_isthread(L, startIdx)) {
			targetThread = lua_tothread(L, startIdx);
			lua_pushvalue(L, startIdx);
		} else {
			luaL_typeerror(L, startIdx, "function or thread");
			return nullptr;
		}

		return targetThread;
	}

	int LibTask_cancel(lua_State *L) {
		luaL_checktype(L, 1, LUA_TTHREAD);
		auto *Thread = lua_tothread(L, 1);
		lua_pushboolean(L, ScriptEngine::Get(L)->Threads.CancelPendingTasks(L, Thread));
		return 1;
	}

	int LibTask_spawn(lua_State *L) {
		int argumentsToPass;
		lua_State *thread = GetThreadFromArgument(L, 1, argumentsToPass);
		if (argumentsToPass > 0) {
			if (!lua_checkstack(thread, argumentsToPass) || !lua_checkstack(L, argumentsToPass))
				luaL_error(L, "too many arguments to spawn");
			for (int Index = 2; Index <= argumentsToPass + 1; ++Index) lua_pushvalue(L, Index);
			lua_xmove(L, thread, argumentsToPass);
		}

		auto scriptEngine = ScriptEngine::Get(L);
		scriptEngine->Threads.ResumeThread(thread, scriptEngine->Threads.TakeThreadReference(thread), argumentsToPass);

		return 1;
	}

	int LibTask_defer(lua_State *L) {
		int argumentsToPass;
		lua_State *thread = GetThreadFromArgument(L, 1, argumentsToPass);

		auto scriptEngine = ScriptEngine::Get(L);
		if (!scriptEngine->Threads.QueueDeferredTask(thread, {L, 2, argumentsToPass})) {
			luaL_error(L, "task queue limit exceeded");
			return 0;
		}

		return 1;
	}

	int LibTask_delay(lua_State *L) {
		double delaySeconds = luaL_checknumber(L, 1);
		if (std::isnan(delaySeconds)) luaL_error(L, "task.delay duration cannot be NaN");

		int argumentsToPass;
		lua_State *thread = GetThreadFromArgument(L, 2, argumentsToPass);

		auto scriptEngine = ScriptEngine::Get(L);
		if (!scriptEngine->Threads.QueueScheduledTask(
			thread, ThreadEngine::ScheduledTask::Type::Delay, delaySeconds, {L, 3, argumentsToPass}
		)) {
			luaL_error(L, "task queue limit exceeded");
			return 0;
		}

		return 1;
	}

	int LibTask_desynchronize(lua_State *L) {
		luaL_error(L, "not yet implemented");
	}

	int LibTask_synchronize(lua_State *L) {
		luaL_error(L, "not yet implemented");
	}

	int LibTask_wait(lua_State *L) {
		if (!GetCurrentScriptSecurityContext().AllowsTaskSchedulerYield) {
			luaL_error(L, "task.wait is unavailable in this handler context");
			return 0;
		}
		auto scriptEngine = ScriptEngine::Get(L);
		double delaySeconds = luaL_optnumber(L, 1, 0.0f);
		if (std::isnan(delaySeconds)) luaL_error(L, "task.wait duration cannot be NaN");
		lua_settop(L, 0);
		if (!scriptEngine->Threads.QueueScheduledTask(
			L, ThreadEngine::ScheduledTask::Type::Wait, delaySeconds, {}
		)) {
			luaL_error(L, "task queue limit exceeded");
			return 0;
		}
		return lua_yield(L, 0);
	}

	luaL_Reg LibTask[]{
		{"cancel", LibTask_cancel},
		{"defer", LibTask_defer},
		{"delay", LibTask_delay},
		{"desynchronize", LibTask_desynchronize},
		{"synchronize", LibTask_synchronize},
		{"spawn", LibTask_spawn},
		{"wait", LibTask_wait},
		{nullptr, nullptr}
	};

	int OpenLibTask(lua_State *L) {
		luaL_register(L, "task", LibTask);
		return 0;
	}
} // namespace gargantuan

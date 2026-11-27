#pragma once

#include "gargantuan/classes/generated/Script.hpp"

#include <lua.h>
#include <string>

namespace gargantuan {
	struct GameplayResumeBudget;
	struct ScriptSecurityContext;
	G_ENUM(RunContext, Client, Server, None);

	enum class ScriptStatus : int {
		Disabled,
		Idle,
		Running,
		Yielded,
		DebugPaused,
		Error,
		Finished,
	};

	class Script : public LuaSourceContainer {
		I_Script;

		Script();

		ScriptStatus Status = ScriptStatus::Idle;
		std::string ErrorMessage = "";
		lua_State *Thread = nullptr;
		int ThreadReference = LUA_NOREF;
		bool Resuming = false;
		bool CleanupAfterResume = false;

		void Cleanup();
		bool ShouldStep();
		ScriptStatus Step(lua_State *L, int *FailureLine = nullptr,
			GameplayResumeBudget *DebugBudget = nullptr, int *StepLine = nullptr);
		ScriptStatus ResumeDebug(lua_State *L, const ScriptSecurityContext &Security,
			GameplayResumeBudget &Budget, int *StepLine, int *FailureLine);

	  private:
		// Valid only while the owned debug coroutine retains its root frame.
		const void *DebugRootClosure = nullptr;
		ScriptStatus ResumeOwnedThread(lua_State *L, int *FailureLine,
			GameplayResumeBudget *DebugBudget, int *StepLine);
	};
}

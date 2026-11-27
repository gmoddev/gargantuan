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
		[[nodiscard]] bool IsCurrentExecution(std::uint64_t Generation) const {return ExecutionActive && Generation && Generation==ExecutionGeneration && GetEnabled() && !GetDestroyed() && !IsDestroying();}
		void RetireExecution() noexcept { ExecutionActive=false; }
		bool ShouldStep();
		ScriptStatus Step(lua_State *L, int *FailureLine = nullptr,
			GameplayResumeBudget *DebugBudget = nullptr, int *StepLine = nullptr);
		ScriptStatus ResumeDebug(lua_State *L, const ScriptSecurityContext &Security,
			GameplayResumeBudget &Budget, int *StepLine, int *FailureLine);

	  private:
		// Valid only while the owned debug coroutine retains its root frame.
		const void *DebugRootClosure = nullptr;
		std::uint64_t ExecutionGeneration{};
		bool ExecutionActive{};
		ScriptStatus ResumeOwnedThread(lua_State *L, int *FailureLine,
			GameplayResumeBudget *DebugBudget, int *StepLine);
	};
}

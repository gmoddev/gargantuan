#include "gargantuan/classes/Script.hpp"
#include "gargantuan/scripting/ExecutionBudget.hpp"
#include "gargantuan/scripting/RootScriptDebug.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <lua.h>
#include <stdexcept>

namespace gargantuan {
	namespace {
		ObjectId GetPublishedScriptId(const Script &Owner) {
			// Starting a detached bootstrap Script must not publish a DataModel identity.
			std::scoped_lock Lock(Owner.IdentityMutex);
			return Owner.Id;
		}
	}

	Script::Script() {
		Destroying->Connect([this](std::monostate _) { this->Cleanup(); });
	}

	bool Script::GetEnabled() const {
		return Status != ScriptStatus::Disabled;
	}

	void Script::SetEnabled(bool enabled) {
		AssertCanMutate();
		ValidatePropertyMutation("Enabled", enabled);
		if (GetEnabled() == enabled) return;
		if (Status == ScriptStatus::Disabled && enabled) {
			Status = ScriptStatus::Idle;
		} else if (Status != ScriptStatus::Disabled && !enabled) {
			Status = ScriptStatus::Disabled;
			RetireExecution();
			Cleanup();
		}
		NotifyPropertyCommitted("Enabled");
	}

	void Script::Cleanup() {
		if (Resuming) {
			CleanupAfterResume = true;
			return;
		}
		if (Thread && ThreadReference != LUA_NOREF && ThreadReference != LUA_REFNIL) {
			auto L = lua_mainthread(Thread);
			auto *Engine = ScriptEngine::Get(L);
			// A queued task owns its own reference to the main coroutine.
			// A deferred cleanup still retires this coroutine after a restart to Idle.
			if (CleanupAfterResume || Status == ScriptStatus::Disabled || GetDestroyed() || IsDestroying())
				(void)Engine->Threads.CancelPendingTasks(L, Thread);
			Engine->UntrackManagedScriptThread(Thread);
			lua_unref(L, ThreadReference);
		}
		ThreadReference = LUA_NOREF;
		Thread = nullptr;
		DebugRootClosure = nullptr;
		CleanupAfterResume = false;
	}

	bool Script::ShouldStep() {
		switch (Status) {
		case ScriptStatus::Error:
		case ScriptStatus::Disabled:
		case ScriptStatus::Yielded:
		case ScriptStatus::DebugPaused:
		case ScriptStatus::Finished:
			return false;

		default:
			return true;
		}
	}

	ScriptStatus Script::Step(lua_State *L, int *FailureLine, GameplayResumeBudget *DebugBudget, int *StepLine) {
		if (FailureLine) *FailureLine = 0;
		[[maybe_unused]] auto KeepScriptAlive = weak_from_this().lock();
		if (!ShouldStep() || Thread) return Status;
		Status = ScriptStatus::Running;

		if (ExecutionGeneration==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("[Script:Runtime] Execution generation exhausted");
		++ExecutionGeneration; ExecutionActive=true;
		auto Security=GetRunContext()==Enums::RunContext::Client?ScriptSecurityContext::ClientRuntime():ScriptSecurityContext::ServerRuntime();
		Security.IssuingScript = GetPublishedScriptId(*this);
		Security.ExecutionGeneration = ExecutionGeneration;
		ScriptSecurityScope SecurityScope(Security);

		auto scriptEngine = ScriptEngine::Get(L);
		[[maybe_unused]] auto KeepRuntimeAlive = scriptEngine->weak_from_this().lock();
		if (FailureLine || StepLine) {
			auto Options = scriptEngine->CompileOptions;
			Options.debugLevel = std::max(Options.debugLevel, 1);
			CompileBytecode(&Options);
		} else CompileBytecode(&scriptEngine->CompileOptions);
		if (BytecodeCompileStatus != BytecodeCompileStatus::Success) {
			Status = ScriptStatus::Error;
			ErrorMessage = BytecodeCompileError.value();
			RetireExecution();
			return Status;
		}

		Thread = lua_newthread(L);
		// lua_newthread leaves the coroutine on the caller's stack. Root that
		// exact value before it can run or collect, then restore the stack.
		ThreadReference = lua_ref(L, -1);
		lua_pop(L, 1);

		auto loadError = LoadIntoState(Thread);
		if (loadError.has_value()) {
			Status = ScriptStatus::Error;
			ErrorMessage = loadError.value();
			RetireExecution();
			Cleanup();
			return Status;
		}
		try {
			scriptEngine->TrackManagedScriptThread(
				Thread, std::static_pointer_cast<Script>(shared_from_this())
			);
		} catch (...) {
			Cleanup();
			throw;
		}
		return ResumeOwnedThread(L, FailureLine, DebugBudget, StepLine);
	}

	ScriptStatus Script::ResumeDebug(lua_State *L, const ScriptSecurityContext &Security,
		GameplayResumeBudget &Budget, int *StepLine, int *FailureLine) {
		if (FailureLine) *FailureLine = 0;
		if (Status != ScriptStatus::DebugPaused || !Thread || lua_status(Thread) != LUA_BREAK)
			throw std::logic_error("[Editor:PlayDebugger] Suspended Script is no longer owned");
		[[maybe_unused]] auto KeepScriptAlive = weak_from_this().lock();
		[[maybe_unused]] auto KeepRuntimeAlive = ScriptEngine::Get(L)->weak_from_this().lock();
		auto Bound = Security;
		Bound.IssuingScript = GetPublishedScriptId(*this);
		Bound.ExecutionGeneration = ExecutionGeneration;
		ScriptSecurityScope SecurityScope(Bound);
		Status = ScriptStatus::Running;
		return ResumeOwnedThread(L, FailureLine, &Budget, StepLine);
	}

	ScriptStatus Script::ResumeOwnedThread(lua_State *L, int *FailureLine, GameplayResumeBudget *DebugBudget, int *StepLine) {
		// Property writes and Destroying callbacks can request cleanup from inside
		// this coroutine. Keep its registry root until lua_resume has returned.
		auto *RunningThread = Thread;
		const auto *RootClosure = DebugRootClosure;
		if (lua_status(RunningThread) == LUA_OK && (FailureLine || StepLine)) {
			RootClosure = lua_topointer(RunningThread, -1);
			if (StepLine) DebugRootClosure = RootClosure;
		}
		Resuming = true;
		int status;
		try {
			if (StepLine) {
				RootScriptDebugTurn Turn(RunningThread, *StepLine);
				status = ResumeGameplay(RunningThread, L, 0, DebugBudget);
				*StepLine = Turn.GetLine();
			} else status = ResumeGameplay(RunningThread, L, 0, DebugBudget);
		} catch (...) {
			Resuming = false;
			if (CleanupAfterResume) Cleanup();
			throw;
		}
		Resuming = false;
		if (status != LUA_BREAK) DebugRootClosure = nullptr;
		if (status == LUA_ERRRUN && FailureLine && RootClosure && !CleanupAfterResume && lua_checkstack(RunningThread, 1)) {
			// Locate the failing Luau frame without reading the user-controlled error text.
			// Only the exact loaded Script closure can supply an authored line.
			for (int Level = 0; Level < 32; ++Level) {
				lua_Debug Frame{};
				if (!lua_getinfo(RunningThread, Level, "slf", &Frame)) break;
				const bool IsRoot = lua_topointer(RunningThread, -1) == RootClosure;
				lua_pop(RunningThread, 1);
				if (Frame.what && Frame.what[0] == 'C') continue;
				if (IsRoot && Frame.currentline > 0) *FailureLine = Frame.currentline;
				break;
			}
		}
		if (status != LUA_YIELD && status != LUA_BREAK && status != LUA_OK) {
			const auto *Message = lua_tostring(RunningThread, -1);
			ErrorMessage = Message ? Message : "Luau execution failed";
		}
		if (CleanupAfterResume) {
			// A disable followed by enable requests a fresh Idle run. A destroyed
			// Script cannot restart, even if it completed before cleanup.
			if (GetDestroyed() || IsDestroying() || Status == ScriptStatus::Running)
				Status = ScriptStatus::Disabled;
			Cleanup();
			return Status;
		}
		switch (status) {
		case LUA_YIELD:
			Status = ScriptStatus::Yielded;
			break;

		case LUA_BREAK:
			Status = StepLine && *StepLine > 0 ? ScriptStatus::DebugPaused : ScriptStatus::Yielded;
			break;

		case LUA_OK:
			Status = ScriptStatus::Finished;
			break;

		default:
			Status = ScriptStatus::Error;
			break;
		}
		// A completed coroutine no longer needs Script's registry root. Tasks,
		// signal callbacks, and Remote requests retain their own Lua references.
		if (Status == ScriptStatus::Error) RetireExecution();
		if (Status == ScriptStatus::Finished || Status == ScriptStatus::Error) Cleanup();

		return Status;
	}
}

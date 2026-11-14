#include "gargantuan/classes/Script.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <lua.h>

namespace gargantuan {
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
			Cleanup();
			Status = ScriptStatus::Disabled;
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
			ScriptEngine::Get(L)->UntrackManagedScriptThread(Thread);
			lua_unref(L, ThreadReference);
		}
		ThreadReference = LUA_NOREF;
		Thread = nullptr;
		CleanupAfterResume = false;
	}

	bool Script::ShouldStep() {
		switch (Status) {
		case ScriptStatus::Error:
		case ScriptStatus::Disabled:
		case ScriptStatus::Yielded:
		case ScriptStatus::Finished:
			return false;

		default:
			return true;
		}
	}

	ScriptStatus Script::Step(lua_State *L) {
		[[maybe_unused]] auto KeepScriptAlive = weak_from_this().lock();
		if (!ShouldStep() || Thread) return Status;
		Status = ScriptStatus::Running;

		ScriptSecurityScope SecurityScope(
			GetRunContext() == Enums::RunContext::Client ? ScriptSecurityContext::ClientRuntime()
														 : ScriptSecurityContext::ServerRuntime()
		);

		auto scriptEngine = ScriptEngine::Get(L);
		[[maybe_unused]] auto KeepRuntimeAlive = scriptEngine->weak_from_this().lock();
		CompileBytecode(&scriptEngine->CompileOptions);
		if (BytecodeCompileStatus != BytecodeCompileStatus::Success) {
			Status = ScriptStatus::Error;
			ErrorMessage = BytecodeCompileError.value();
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

		// Property writes and Destroying callbacks can request cleanup from inside
		// this coroutine. Keep its registry root until lua_resume has returned.
		auto *RunningThread = Thread;
		Resuming = true;
		int status;
		try {
			status = lua_resume(RunningThread, L, 0);
		} catch (...) {
			Resuming = false;
			if (CleanupAfterResume) Cleanup();
			throw;
		}
		Resuming = false;
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
		case LUA_BREAK:
			Status = ScriptStatus::Yielded;
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
		if (Status == ScriptStatus::Finished || Status == ScriptStatus::Error) Cleanup();

		return Status;
	}
}

#include "gargantuan/scripting/ThreadEngine.hpp"
#include "gargantuan/scripting/ExecutionBudget.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/Log.hpp"
#include "DiagnosticText.hpp"

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <chrono>
#include <lua.h>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	double GetCurrentTime() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};

	ThreadEngine::ThreadEngine(lua_State *mainState) : L(mainState) {};

	int ThreadEngine::TakeThreadReference(lua_State *thread) {
		if (!lua_checkstack(thread, 1)) throw std::length_error("[Runtime:Task] Cannot retain thread reference");
		lua_pushthread(thread);
		try {
			const int Reference = lua_ref(thread, -1);
			lua_pop(thread, 1);
			return Reference;
		} catch (...) {
			lua_pop(thread, 1);
			throw;
		}
	}

	void ThreadEngine::ResumeThread(lua_State *thread, int threadReference, int argumentCount) {
		ResumeThreadWithContext(thread, threadReference, argumentCount, GetCurrentScriptSecurityContext());
	}

	void ThreadEngine::ResumeThreadWithContext(
		lua_State *thread,
		int threadReference,
		int argumentCount,
		ScriptSecurityContext securityContext
	) {
		[[maybe_unused]] auto KeepAlive = ScriptEngine::Get(L)->weak_from_this().lock();
		if (Stopping) {
			lua_unref(L, threadReference);
			return;
		}
		ScriptSecurityScope SecurityScope(std::move(securityContext));
		const int PreResumeStatus = lua_costatus(L, thread);
		int status = ResumeGameplay(thread, L, argumentCount);
		ScriptEngine::Get(L)->CompleteManagedScriptThreadResume(thread, status, PreResumeStatus);
		switch (status) {
		case LUA_OK:
		case LUA_YIELD:
			break;

		default: {
			size_t Length = 0;
			const char *Detail = lua_tolstring(thread, -1, &Length);
			std::string Message = "[Runtime:Task] ";
			Message += Detail ? BoundedDiagnosticText({Detail, Length}, 1024) : "Task failed";
			LOG_ERROR(Lua, "%s", Message.c_str());
			ScriptEngine::Get(L)->EmitRuntimeDiagnostic("Error", std::move(Message));
			break;
		}
		}
		lua_unref(L, threadReference);
	}

	void ThreadEngine::ShutdownRuntime() {
		if (Stopping) return;
		Stopping = true;
		for (const auto &Task : ScheduledQueue) ReleaseTask(Task.Entry);
		ScheduledQueue.clear();
		for (const auto &Task : DeferredQueue) ReleaseTask(Task);
		DeferredQueue.clear();
	}

	bool ThreadEngine::CancelPendingTasks(lua_State *Caller, lua_State *Thread) {
		if (Stopping || !Caller || !Thread || lua_mainthread(Caller) != L || lua_mainthread(Thread) != L ||
			lua_costatus(Caller, Thread) != LUA_COSUS)
			return false;

		const auto Release = [&](const TaskEntry &Task) {
			if (Task.Thread != Thread) return false;
			ReleaseTask(Task);
			return true;
		};
		const auto RemovedScheduled = std::erase_if(ScheduledQueue, [&](const auto &Task) { return Release(Task.Entry); });
		if (RemovedScheduled != 0) std::make_heap(ScheduledQueue.begin(), ScheduledQueue.end(), CompareWakeTime{});
		const auto RemovedDeferred = std::erase_if(DeferredQueue, Release);
		return RemovedScheduled != 0 || RemovedDeferred != 0;
	}

	ThreadEngine::TaskEntry ThreadEngine::CaptureTask(lua_State *Thread, ArgumentRange Arguments) {
		if (!Thread || lua_mainthread(Thread) != L || Arguments.Count < 0 ||
			(Arguments.Count != 0 && (!Arguments.Source || lua_mainthread(Arguments.Source) != L ||
				Arguments.First < 1 || Arguments.Count > lua_gettop(Arguments.Source) - Arguments.First + 1)))
			throw std::invalid_argument("[Runtime:Task] Invalid scheduler thread or argument range");
		TaskEntry Task{
			.Thread = Thread,
			.ThreadReference = TakeThreadReference(Thread),
			.ArgumentCount = Arguments.Count,
			.SecurityContext = GetCurrentScriptSecurityContext(),
		};
		if (Arguments.Count == 0) return Task;
		auto *Source = Arguments.Source;
		const int OriginalTop = lua_gettop(Source);
		try {
			if (!lua_checkstack(Source, 2)) throw std::length_error("[Runtime:Task] Cannot retain queued arguments");
			// Keep arguments private to this queue entry until its own resume.
			lua_createtable(Source, Arguments.Count, 0);
			for (int Index = 0; Index < Arguments.Count; ++Index) {
				lua_pushvalue(Source, Arguments.First + Index);
				lua_rawseti(Source, -2, Index + 1);
			}
			Task.ArgumentReference = lua_ref(Source, -1);
			lua_pop(Source, 1);
		} catch (...) {
			lua_settop(Source, OriginalTop);
			ReleaseTask(Task);
			throw;
		}
		return Task;
	}

	void ThreadEngine::ReleaseTask(const TaskEntry &Task) {
		lua_unref(L, Task.ThreadReference);
		lua_unref(L, Task.ArgumentReference);
	}

	void ThreadEngine::ResumeQueuedTask(TaskEntry Task, std::optional<double> Elapsed) {
		if (Stopping || lua_costatus(L, Task.Thread) != LUA_COSUS) {
			ReleaseTask(Task);
			return;
		}
		const int Count = Elapsed ? 1 : Task.ArgumentCount;
		const int StackSpace = Count + (Task.ArgumentReference != LUA_NOREF ? 1 : 0);
		bool HasStackSpace = false;
		try {
			HasStackSpace = StackSpace == 0 || lua_checkstack(Task.Thread, StackSpace);
		} catch (...) {
			ReleaseTask(Task);
			throw;
		}
		if (!HasStackSpace) {
			ReleaseTask(Task);
			LOG_ERROR(Lua, "[Runtime:Task] Too many arguments to resume scheduled thread");
			return;
		}
		if (Elapsed) {
			lua_pushnumber(Task.Thread, *Elapsed);
		} else if (Task.ArgumentReference != LUA_NOREF) {
			lua_getref(Task.Thread, Task.ArgumentReference);
			const int Table = lua_absindex(Task.Thread, -1);
			for (int Index = 1; Index <= Count; ++Index) lua_rawgeti(Task.Thread, Table, Index);
			lua_remove(Task.Thread, Table);
		}
		lua_unref(L, Task.ArgumentReference);
		ResumeThreadWithContext(Task.Thread, Task.ThreadReference, Count, std::move(Task.SecurityContext));
	}

	void ThreadEngine::Step() {
		// A task can destroy the Engine while its active continuation still
		// owns a coroutine reference that must be released before the VM closes.
		[[maybe_unused]] auto KeepAlive = ScriptEngine::Get(L)->weak_from_this().lock();
		const auto CurrentTime = GetCurrentTime();
		while (!Stopping && !ScheduledQueue.empty() && ScheduledQueue.front().WakeTime <= CurrentTime) {
			std::pop_heap(ScheduledQueue.begin(), ScheduledQueue.end(), CompareWakeTime{});
			auto Task = std::move(ScheduledQueue.back());
			ScheduledQueue.pop_back();

			if (Task.type == ScheduledTask::Type::Wait)
				ResumeQueuedTask(std::move(Task.Entry), CurrentTime - Task.ScheduledTime);
			else ResumeQueuedTask(std::move(Task.Entry));
		}

		// Keep all queued turns visible to cancellation and shutdown while a
		// callback runs. New deferrals append after the turns already pending.
		while (!Stopping && !DeferredQueue.empty()) {
			auto Task = std::move(DeferredQueue.front());
			DeferredQueue.pop_front();
			ResumeQueuedTask(std::move(Task));
		}
	}

	bool ThreadEngine::QueueScheduledTask(
		lua_State *Thread, ScheduledTask::Type Type, double DelaySeconds, ArgumentRange Arguments
	) {
		if (Stopping || ScheduledQueue.size() + DeferredQueue.size() >= MaximumQueuedScriptTasks) return false;
		const auto Now = GetCurrentTime();
		const auto Entry = CaptureTask(Thread, Arguments);
		try {
			ScheduledQueue.push_back({.type = Type, .Entry = Entry, .ScheduledTime = Now, .WakeTime = Now + DelaySeconds});
		} catch (...) {
			ReleaseTask(Entry);
			throw;
		}
		std::push_heap(ScheduledQueue.begin(), ScheduledQueue.end(), CompareWakeTime{});
		return true;
	}

	bool ThreadEngine::QueueDeferredTask(lua_State *Thread, ArgumentRange Arguments) {
		if (Stopping || ScheduledQueue.size() + DeferredQueue.size() >= MaximumQueuedScriptTasks) return false;
		const auto Entry = CaptureTask(Thread, Arguments);
		try {
			DeferredQueue.push_back(Entry);
		} catch (...) {
			ReleaseTask(Entry);
			throw;
		}
		return true;
	}
} // namespace gargantuan

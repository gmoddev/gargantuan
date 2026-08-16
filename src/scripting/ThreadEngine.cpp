#include "gargantuan/scripting/ThreadEngine.hpp"
#include "gargantuan/Log.hpp"

#include <SDL3/SDL_log.h>
#include <chrono>
#include <lua.h>
#include <utility>

namespace gargantuan {
	double GetCurrentTime() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	};

	ThreadEngine::ThreadEngine(lua_State *mainState) : L(mainState) {};

	int ThreadEngine::TakeThreadReference(lua_State *thread) {
		lua_pushthread(thread);
		lua_xmove(thread, L, 1);
		int reference = lua_ref(L, -1);
		// lua_ref keeps the value on the stack
		lua_pop(L, 1);
		return reference;
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
		ScriptSecurityScope SecurityScope(std::move(securityContext));
		int status = lua_resume(thread, L, argumentCount);
		lua_unref(L, threadReference);
		switch (status) {
		case LUA_OK:
		case LUA_YIELD:
			break;

		default:
			LOG_ERROR(Lua, "Thread error: %s", lua_tostring(thread, -1));
		}
	}

	void ThreadEngine::Step() {
		auto currentTime = GetCurrentTime();
		while (!ScheduledQueue.empty() && ScheduledQueue.top().WakeTime <= currentTime) {
			auto task = ScheduledQueue.top();
			ScheduledQueue.pop();

			switch (task.type) {
			case ThreadEngine::ScheduledTask::Type::Delay: {
				ResumeThreadWithContext(
					task.Thread, task.ThreadReference, task.ArgumentCount, std::move(task.SecurityContext)
				);
				break;
			}
			case ThreadEngine::ScheduledTask::Type::Wait: {
				double actualWait = currentTime - task.ScheduledTime;
				lua_pushnumber(task.Thread, actualWait);
				ResumeThreadWithContext(task.Thread, task.ThreadReference, 1, std::move(task.SecurityContext));
				break;
			}
			}
		}

		while (!DeferredQueue.empty()) {
			std::vector<DeferredTask> currentBatch;
			currentBatch.swap(DeferredQueue);

			for (auto &task : currentBatch) {
				ResumeThreadWithContext(
					task.Thread, task.ThreadReference, task.ArgumentCount, std::move(task.SecurityContext)
				);
			}
		}
	}

	bool ThreadEngine::QueueScheduledTask(
		lua_State *thread, ScheduledTask::Type type, double delaySeconds, int argumentCount
	) {
		if (ScheduledQueue.size() + DeferredQueue.size() >= MaximumQueuedScriptTasks) return false;
		ScheduledQueue.push({
			.type = type,
			.Thread = thread,
			.ThreadReference = TakeThreadReference(thread),
			.ArgumentCount = argumentCount,
			.ScheduledTime = GetCurrentTime(),
			.WakeTime = GetCurrentTime() + delaySeconds,
			.SecurityContext = GetCurrentScriptSecurityContext(),
		});
		return true;
	}

	bool ThreadEngine::QueueDeferredTask(lua_State *thread, int argumentCount) {
		if (ScheduledQueue.size() + DeferredQueue.size() >= MaximumQueuedScriptTasks) return false;
		DeferredQueue.push_back({
			.Thread = thread,
			.ThreadReference = TakeThreadReference(thread),
			.ArgumentCount = argumentCount,
			.SecurityContext = GetCurrentScriptSecurityContext(),
		});
		return true;
	}
} // namespace gargantuan

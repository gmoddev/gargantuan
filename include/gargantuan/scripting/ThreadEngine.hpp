#pragma once

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <lua.h>
#include <cstddef>
#include <queue>
#include <vector>

namespace gargantuan {
	inline constexpr std::size_t MaximumQueuedScriptTasks = 65'536;

	class ThreadEngine {
	  public:
		struct ScheduledTask {
			enum class Type {
				// task.wait()
				Wait,
				// task.delay()
				Delay,
			};

			Type type = Type::Wait;
			lua_State *Thread = nullptr;
			int ThreadReference = LUA_NOREF;
			int ArgumentCount = 0;
			double ScheduledTime = 0;
			double WakeTime = 0;
			ScriptSecurityContext SecurityContext;
		};

		struct DeferredTask {
			lua_State *Thread = nullptr;
			int ThreadReference = LUA_NOREF;
			int ArgumentCount = 0;
			ScriptSecurityContext SecurityContext;
		};

		ThreadEngine(lua_State *mainState);
		int TakeThreadReference(lua_State *thread);
		void Step();
		void ResumeThread(lua_State *thread, int threadReference, int argumentCount);
		bool QueueScheduledTask(lua_State *thread, ScheduledTask::Type type, double delaySeconds, int argumentCount);
		bool QueueDeferredTask(lua_State *thread, int argumentCount);

	  private:
		lua_State *L;

		struct CompareWakeTime {
			bool operator()(const ScheduledTask &lhs, const ScheduledTask &rhs) {
				return lhs.WakeTime > rhs.WakeTime;
			};
		};

		std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, CompareWakeTime> ScheduledQueue;
		std::vector<DeferredTask> DeferredQueue;

		void ResumeThreadWithContext(
			lua_State *thread,
			int threadReference,
			int argumentCount,
			ScriptSecurityContext securityContext
		);
	};
} // namespace gargantuan

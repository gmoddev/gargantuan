#pragma once

#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <lua.h>
#include <chrono>
#include <functional>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace gargantuan {
	inline constexpr std::size_t MaximumQueuedScriptTasks = 65'536;

	class ThreadEngine {
		struct TaskEntry {
			lua_State *Thread = nullptr;
			int ThreadReference = LUA_NOREF;
			int ArgumentReference = LUA_NOREF;
			int ArgumentCount = 0;
			ScriptSecurityContext SecurityContext;
		};

	  public:
		struct ArgumentRange {
			lua_State *Source = nullptr;
			int First = 0;
			int Count = 0;
		};

		struct ScheduledTask {
			enum class Type {
				// task.wait()
				Wait,
				// task.delay()
				Delay,
			};

			Type type = Type::Wait;
			TaskEntry Entry;
			double ScheduledTime = 0;
			double WakeTime = 0;
		};

		ThreadEngine(lua_State *mainState);
		int TakeThreadReference(lua_State *thread);
		void ShutdownRuntime();
		void Step();
		void SetDevelopmentClock(std::function<std::chrono::steady_clock::time_point()> Clock);
		void ResumeThread(lua_State *thread, int threadReference, int argumentCount);
		bool CancelPendingTasks(lua_State *Caller, lua_State *Thread);
		bool QueueScheduledTask(lua_State *Thread, ScheduledTask::Type Type, double DelaySeconds, ArgumentRange Arguments);
		bool QueueDeferredTask(lua_State *Thread, ArgumentRange Arguments);

	  private:
		lua_State *L;
		bool Stopping = false;
		std::function<std::chrono::steady_clock::time_point()> DevelopmentClock;
		[[nodiscard]] double Now() const;

		struct CompareWakeTime {
			bool operator()(const ScheduledTask &Left, const ScheduledTask &Right) const {
				return Left.WakeTime > Right.WakeTime;
			};
		};

		std::vector<ScheduledTask> ScheduledQueue;
		std::deque<TaskEntry> DeferredQueue;

		TaskEntry CaptureTask(lua_State *Thread, ArgumentRange Arguments);
		void ReleaseTask(const TaskEntry &Task);
		void ResumeQueuedTask(TaskEntry Task, std::optional<double> Elapsed = std::nullopt);
		void ResumeThreadWithContext(
			lua_State *thread,
			int threadReference,
			int argumentCount,
			ScriptSecurityContext securityContext
		);
	};
} // namespace gargantuan

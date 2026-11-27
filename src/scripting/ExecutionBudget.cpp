#include "gargantuan/scripting/ExecutionBudget.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <algorithm>
#include <chrono>
#include <lualib.h>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;
		constexpr unsigned SafepointsPerClockCheck = 512;
		struct BudgetScope {
			static thread_local BudgetScope *Active;
			lua_State *Main;
			BudgetScope *Parent;
			void (*PriorInterrupt)(lua_State *, int);
			Clock::time_point Deadline;
			unsigned UntilCheck = 1;
			bool Expired = false;

			BudgetScope(lua_State *State, Clock::duration Duration)
				: Main(lua_mainthread(State)), Parent(Active), PriorInterrupt(lua_callbacks(Main)->interrupt),
				  Deadline(Clock::now() + Duration) {
				Active = this;
				lua_callbacks(Main)->interrupt = Interrupt;
			}
			~BudgetScope() {
				lua_callbacks(Main)->interrupt = PriorInterrupt;
				Active = Parent;
			}

			static void Interrupt(lua_State *State, int Gc) {
				auto *Budget = Active;
				const auto *Main = lua_mainthread(State);
				while (Budget->Main != Main) Budget = Budget->Parent;
				if (Budget->PriorInterrupt) Budget->PriorInterrupt(State, Gc);
				if (Gc >= 0 || --Budget->UntilCheck != 0) return;
				if (Budget->Expired || Clock::now() >= Budget->Deadline) {
					Budget->Expired = true;
					Budget->UntilCheck = 1;
					luaL_error(State, "[Runtime:ExecutionBudget] gameplay execution deadline exceeded");
				}
				Budget->UntilCheck = SafepointsPerClockCheck;
			}
		};
		thread_local BudgetScope *BudgetScope::Active = nullptr;

		template <typename Function>
		int RunGameplay(lua_State *State, Function Call, GameplayResumeBudget *Retained = nullptr) {
			if (lua_callbacks(State)->interrupt == BudgetScope::Interrupt) return Call();
			auto *Engine = ScriptEngine::Get(State);
			const auto Configured = std::min(Engine->GameplayExecutionBudget, std::chrono::milliseconds(86'400'000));
			if (!Retained && Configured.count() <= 0) return Call();
			const auto Duration = Retained ? Retained->Remaining : Clock::duration(Configured);
			int Status;
			bool Expired;
			{
				const auto Started = Retained ? Clock::now() : Clock::time_point{};
				BudgetScope Scope(State, Duration);
				Status = Call();
				Expired = Scope.Expired;
				if (Retained) Retained->Remaining -= Clock::now() - Started;
			}
			if (Expired)
				Engine->EmitRuntimeDiagnostic("Error", "[Runtime:ExecutionBudget] gameplay execution exceeded " +
					std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Duration).count()) + " ms");
			return Status;
		}
	}

	int ResumeGameplay(lua_State *Thread, lua_State *From, int ArgumentCount, GameplayResumeBudget *Retained) {
		return RunGameplay(Thread, [&] { return lua_resume(Thread, From, ArgumentCount); }, Retained);
	}
	int CallGameplay(lua_State *State, int ArgumentCount, int ResultCount) {
		return RunGameplay(State, [&] { return lua_pcall(State, ArgumentCount, ResultCount, 0); });
	}
}

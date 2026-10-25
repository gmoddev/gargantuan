#pragma once

#include "gargantuan/network/Scheduler.hpp"

namespace gargantuan::network::detail {
	// One GameSession peer/step allowance shared by early, structural and late
	// flush points. This owns no messages and changes no scheduler precedence.
	class SessionSendAllowance final {
	  public:
		void Reset() noexcept { Bytes = 0; Messages = 0; Stopped = false; }

		SchedulerFlushResult Flush(INetworkScheduler &Scheduler, ConnectionId Connection, const NetworkLimits &Limits) {
			if (!Limits.IsValid()) return {.Status = SchedulerFlushStatus::InvalidBudget};
			if (Stopped) return {.Status = SchedulerFlushStatus::BudgetLimited};
			const auto Full = SchedulerTickBudget::FromNetworkLimits(Limits);
			if (Bytes > Full.MaximumBytes || Messages > Full.MaximumMessages)
				return {.Status = SchedulerFlushStatus::InvalidBudget};
			const SchedulerTickBudget Remaining{Full.MaximumBytes - Bytes, Full.MaximumMessages - Messages};
			// Preserve the existing minimum valid flush quantum (one largest legal
			// message). A smaller residual waits until the next step, without credit.
			if (!Remaining.IsValidFor(Limits)) {
				Stopped = true;
				return {.Status = SchedulerFlushStatus::BudgetLimited};
			}
			auto Result = Scheduler.Flush(Connection, Remaining);
			if (!Result.IsValidFor(Remaining)) {
				Stopped = true;
				return {.Status = SchedulerFlushStatus::InvalidBudget};
			}
			Bytes += Result.BytesSubmitted;
			Messages += Result.MessagesSubmitted;
			// Do not let late control/gameplay overtake structural prerequisites
			// still queued after a budget-limited/backpressured flush. Nor retry
			// a blocked backend at each remaining service point in the same step.
			Stopped = Result.Status != SchedulerFlushStatus::Drained;
			return Result;
		}

	  private:
		std::size_t Bytes = 0;
		std::uint32_t Messages = 0;
		bool Stopped = false;
	};
}

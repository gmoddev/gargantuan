#pragma once

#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "../../runtime/RuntimeWorkDiagnostics.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace gargantuan::host::detail {

// Host-only oracle: exclusively owns an already captured immutable source
// projection. Never reads GameSession or publishes into Main-thread evidence.
// The queue contains fixed frame identities/timings, never encoded payloads.
class FrozenRecoveryQuote final {
  public:
	struct Result {
		network::FrozenJournalQuoteStep Step;
		std::uint64_t AdvanceMicroseconds = 0;
		std::uint64_t BuildMicroseconds = 0;
		std::uint64_t EncodeMicroseconds = 0;
		std::uint64_t EncodeRetries = 0;
		std::uint64_t JournalLagRecords = 0;
	};
	static constexpr std::size_t QueueCapacity = 64;
	static constexpr std::uint64_t MaximumFrames = 65'536;

	FrozenRecoveryQuote(std::unique_ptr<network::ReplicationCoordinator> Snapshot,
		std::map<network::ConnectionId, std::size_t> Limits) {
		if (!Snapshot || Limits.empty()) throw std::invalid_argument("frozen recovery oracle requires a snapshot and limits");
		Worker = std::jthread([this, Snapshot = std::move(Snapshot), Limits = std::move(Limits)]
			(std::stop_token Stop) mutable noexcept { Run(Stop, *Snapshot, Limits); });
	}
	~FrozenRecoveryQuote() {
		Worker.request_stop();
		SpaceAvailable.notify_all();
		// Join before destroying the queue, mutex, or owned captured schema/state.
		if (Worker.joinable()) Worker.join();
	}
	FrozenRecoveryQuote(const FrozenRecoveryQuote &) = delete;
	FrozenRecoveryQuote &operator=(const FrozenRecoveryQuote &) = delete;

	// A missed lock is merely "not ready". Main never waits on encoding, queue
	// space, or the worker's synchronization primitive during measured service.
	[[nodiscard]] std::optional<Result> TryPop() {
		std::unique_lock Lock(Mutex, std::try_to_lock);
		if (!Lock.owns_lock() || Count == 0) return {};
		auto Value = std::move(Queue[Head]);
		Queue[Head].reset();
		Head = (Head + 1) % QueueCapacity;
		--Count;
		Lock.unlock();
		SpaceAvailable.notify_one();
		return Value;
	}

  private:
	friend struct FrozenRecoveryQuoteTestAccess;
	std::mutex Mutex;
	std::condition_variable_any SpaceAvailable;
	std::array<std::optional<Result>, QueueCapacity> Queue;
	std::size_t Head = 0, Count = 0;
	bool WaitingForSpace = false;
	// Preallocate the exception fallback before starting the worker. Even an
	// allocation failure in replay can report failure without allocating again.
	Result ExceptionResult{.Step = {.Error = "frozen recovery oracle replay threw an exception"}};
	Result ExhaustedResult{.Step = {.Error = "frozen recovery oracle frame bound exceeded"}};
	std::jthread Worker;

	bool Publish(std::stop_token Stop, Result Value) {
		std::unique_lock Lock(Mutex);
		WaitingForSpace = Count == QueueCapacity;
		const bool Ready = SpaceAvailable.wait(Lock, Stop, [this] { return Count < QueueCapacity; });
		WaitingForSpace = false;
		if (!Ready || Stop.stop_requested()) return false;
		Queue[(Head + Count) % QueueCapacity].emplace(std::move(Value));
		++Count;
		return true;
	}
	void Run(std::stop_token Stop, network::ReplicationCoordinator &Snapshot,
		const std::map<network::ConnectionId, std::size_t> &Limits) noexcept {
		try {
			std::uint64_t Frames = 0;
			// Planning slices and filtered journal coverage are progress, not
			// retained frame evidence. Their count is not bounded by MaximumFrames.
			// The owning campaign's existing finite lifetime bounds execution;
			// cancellation is checked between advances and during queue waits.
			while (!Stop.stop_requested()) {
				Result Value;
				runtime_detail::WorkSample Work{};
				const auto Started = std::chrono::steady_clock::now();
				{
					runtime_detail::WorkCapture Capture(&Work);
					Value.Step = Snapshot.AdvanceFrozenJournalQuote(Limits);
				}
				if (Value.Step.Frame) {
					if (Frames == MaximumFrames) {
						Value.Step.Frame.reset();
						Value.Step.Complete = false;
						Value.Step.Error = std::move(ExhaustedResult.Step.Error);
					} else ++Frames;
				}
				Value.AdvanceMicroseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - Started).count());
				Value.BuildMicroseconds = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::IncrementalPreparation)].ExclusiveNanoseconds / 1000;
				Value.EncodeMicroseconds = Work[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].ExclusiveNanoseconds / 1000;
				Value.EncodeRetries = Work.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::EncodeRetries)];
				for (const auto &[Connection, Limit] : Limits) {
					(void)Limit;
					const auto Lag = Snapshot.GetJournalLag(Connection);
					if (Lag > std::numeric_limits<std::uint64_t>::max() - Value.JournalLagRecords)
						throw std::overflow_error("frozen recovery oracle lag overflow");
					Value.JournalLagRecords += Lag;
				}
				const bool Terminal = Value.Step.Complete || !Value.Step.Error.empty();
				if (!Publish(Stop, std::move(Value)) || Terminal) return;
			}
		} catch (...) {
			// A system-level mutex failure cannot be recovered, but ordinary replay
			// exceptions (including allocation) remain terminal evidence results.
			try { (void)Publish(Stop, std::move(ExceptionResult)); } catch (...) {}
		}
	}
};
}

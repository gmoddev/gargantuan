#pragma once

#include "../../../cmake/gns/ServiceTimingDiagnostics.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string_view>
#if defined(GARGANTUAN_WITH_GNS)
#include <steam/steamnetworkingsockets.h>
#endif

namespace gargantuan::host::detail {
struct FarmServiceTimingEvidenceTestAccess;
// Opt-in raw observation only. No timer, admission, F1 or exit predicate reads
// this recorder. Its owner must outlive transport destruction/native joins.
class FarmServiceTimingEvidence final {
public:
	using Record = SteamNetworkingSocketsLib::GargantuanServiceTimingRecord;
	// A four-MiB text budget at at most 512 bytes per raw record. This is a
	// diagnostic storage limit, never a service duration or qualification bound.
	static constexpr std::size_t Capacity = (4 * 1024 * 1024) / 512;
	struct Clock {
		std::uint64_t Process = 0, Thread = 0, SteadyNs = 0, QpcBefore = 0, QpcAfter = 0, Frequency = 0;
		bool Valid = false;
		static Clock Capture() noexcept {
			Clock Value;
#if defined(_WIN32)
			LARGE_INTEGER Before{}, After{}, Frequency{};
			const bool BeforeValid = QueryPerformanceCounter(&Before) != 0;
			Value.SteadyNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
			const bool AfterValid = QueryPerformanceCounter(&After) != 0;
			Value.Process = GetCurrentProcessId(); Value.Thread = GetCurrentThreadId();
			Value.Valid = BeforeValid && AfterValid && QueryPerformanceFrequency(&Frequency) &&
				Before.QuadPart >= 0 && After.QuadPart >= Before.QuadPart && Frequency.QuadPart > 0;
			if (Value.Valid) {
				Value.QpcBefore = static_cast<std::uint64_t>(Before.QuadPart);
				Value.QpcAfter = static_cast<std::uint64_t>(After.QuadPart);
				Value.Frequency = static_cast<std::uint64_t>(Frequency.QuadPart);
			}
#endif
			return Value;
		}
	};
	class History final {
		friend struct FarmServiceTimingEvidenceTestAccess;
		struct Slot { std::atomic_flag Writing = ATOMIC_FLAG_INIT; std::uint64_t Sequence = 0; Record Value{}; };
		std::array<Slot, Capacity> Slots{};
		std::atomic<std::uint64_t> Attempts{0}, Written{0}, Overwritten{0}, Dropped{0}, Filtered{0}, Cutoff{0};
		std::atomic<bool> CounterOverflow{false};
		void Publish(std::uint64_t Index, const Record &Value) noexcept {
			auto &Entry = Slots[static_cast<std::size_t>(Index % Capacity)];
			// Native callbacks never wait. A wrapped concurrent writer collision
			// is a declared diagnostic loss, never a racing overwrite.
			if (Entry.Writing.test_and_set(std::memory_order_acquire)) { Dropped.fetch_add(1); return; }
			const auto CurrentEnd = Cutoff.load(std::memory_order_acquire);
			if (CurrentEnd && Value.BeginQpc > CurrentEnd) {
				Filtered.fetch_add(1); Entry.Writing.clear(std::memory_order_release); return;
			}
			// A writer delayed before acquiring its slot must not replace a
			// newer wrap of that slot with an older sequence.
			if (Entry.Sequence > Index + 1) {
				Dropped.fetch_add(1); Entry.Writing.clear(std::memory_order_release); return;
			}
			if (Entry.Sequence) Overwritten.fetch_add(1, std::memory_order_relaxed);
			Entry.Value = Value; Entry.Sequence = Index + 1;
			Written.fetch_add(1, std::memory_order_relaxed);
			Entry.Writing.clear(std::memory_order_release);
		}
	public:
		void Append(const Record &Value) noexcept {
			const auto End = Cutoff.load(std::memory_order_acquire);
			// Keep a span which began before freeze even when it ends afterward.
			if ((End && Value.BeginQpc > End) || CounterOverflow.load(std::memory_order_relaxed)) return;
			const auto Index = Attempts.fetch_add(1, std::memory_order_relaxed);
			if (Index == std::numeric_limits<std::uint64_t>::max()) { CounterOverflow.store(true); return; }
			Publish(Index, Value);
		}
		void Freeze(std::uint64_t Qpc) noexcept { if (Qpc) { std::uint64_t Empty = 0; Cutoff.compare_exchange_strong(Empty, Qpc); } }
		std::uint64_t AttemptCount() const noexcept { return Attempts.load(); }
		std::uint64_t WrittenCount() const noexcept { return Written.load(); }
		std::uint64_t OverwrittenCount() const noexcept { return Overwritten.load(); }
		std::uint64_t DroppedCount() const noexcept { return Dropped.load(); }
		std::uint64_t FilteredCount() const noexcept { return Filtered.load(); }
		bool Overflowed() const noexcept { return CounterOverflow.load(); }
		// Only after every writer/native thread has joined. Freeze is a filter,
		// not permission to read storage concurrently with an in-flight span.
		template<class Visitor> void Visit(const Visitor &Value) const {
			const auto End = Cutoff.load(std::memory_order_acquire);
			// Main may have paused between reading the freeze clock and
			// publishing it. Filter records already published in that gap too.
			for (const auto &Entry : Slots)
				if (Entry.Sequence && (!End || Entry.Value.BeginQpc <= End)) Value(Entry.Sequence, Entry.Value);
		}
	};
private:
	struct ThreadStart { Clock Value{}; std::atomic<bool> Ready{false}; };
	std::unique_ptr<History> Values;
	std::array<ThreadStart, 16> Threads{};
	std::atomic<unsigned> ThreadCount{0};
	std::array<Record, 16> TimerCreates{};
	std::atomic<unsigned> TimerCount{0};
	Clock Begin{}, Failure{};
	std::uint64_t FailureObservedUs = 0;
	bool Registered = false, CallbackRegistered = false;
	inline static std::atomic<FarmServiceTimingEvidence *> Owner{nullptr};
	SteamNetworkingSocketsLib::GargantuanServiceTimingSink Sink{this, RecordTiming, true};
	static void RecordTiming(void *Context, const Record &Value) noexcept {
		auto &Self = *static_cast<FarmServiceTimingEvidence *>(Context);
		if (Value.Phase == SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::TimerCreate) {
			const auto Index = Self.TimerCount.fetch_add(1, std::memory_order_relaxed);
			if (Index < Self.TimerCreates.size()) Self.TimerCreates[Index] = Value;
			return;
		}
		Self.Values->Append(Value);
	}
	static void ServiceStarted() noexcept {
		auto *Raw = Owner.load(std::memory_order_acquire); if (!Raw) return;
		auto &Self = *Raw;
		const auto Index = Self.ThreadCount.fetch_add(1, std::memory_order_relaxed);
		if (Index >= Self.Threads.size()) return;
		auto &Entry = Self.Threads[Index]; Entry.Value = Clock::Capture(); Entry.Ready.store(true, std::memory_order_release);
	}
	static void PrintClock(std::ostream &Out, std::string_view Run, const char *Boundary, const Clock &Value) {
		Out << "[Qualification:FarmServiceClock] run=" << Run << " boundary=" << Boundary
			<< " pid=" << Value.Process << " native_tid=" << Value.Thread << " clock_valid=" << Value.Valid
			<< " steady_ns=" << Value.SteadyNs << " qpc_before=" << Value.QpcBefore << " qpc_after=" << Value.QpcAfter
			<< " qpc_frequency=" << Value.Frequency << '\n';
	}
public:
	static bool EnabledValue(std::string_view Value) noexcept { return Value == "1"; }
	static bool Requested() noexcept {
		const char *Value = std::getenv("GARGANTUAN_FARM_SERVICE_TIMING");
		return Value && EnabledValue(Value);
	}
	// Supplied false mode lets tests prove no allocation, registration or clock.
	explicit FarmServiceTimingEvidence(bool Enabled) {
		if (!Enabled) return;
		Values = std::make_unique<History>(); Begin = Clock::Capture();
		if (!SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(&Sink))
			throw std::runtime_error("[Qualification:FarmServiceTiming] timing sink already owned");
		Registered = true;
		FarmServiceTimingEvidence *Empty = nullptr;
		if (!Owner.compare_exchange_strong(Empty, this)) {
			SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(nullptr); Registered = false;
			throw std::runtime_error("[Qualification:FarmServiceTiming] service callback already owned");
		}
#if defined(GARGANTUAN_WITH_GNS)
		SteamNetworkingSockets_SetServiceThreadInitCallback(ServiceStarted); CallbackRegistered = true;
#endif
	}
	~FarmServiceTimingEvidence() { Stop(); }
	FarmServiceTimingEvidence(const FarmServiceTimingEvidence &) = delete;
	FarmServiceTimingEvidence &operator=(const FarmServiceTimingEvidence &) = delete;
	bool Enabled() const noexcept { return Values != nullptr; }
	static void Failed(void *Context, std::uint64_t ObservedUs) noexcept {
		static_cast<FarmServiceTimingEvidence *>(Context)->Freeze(ObservedUs);
	}
	void Freeze(std::uint64_t ObservedUs) noexcept {
		if (!Values || FailureObservedUs) return;
		FailureObservedUs = ObservedUs; Failure = Clock::Capture();
		if (Failure.Valid) Values->Freeze(Failure.QpcAfter);
	}
	// Caller has destroyed every transport and joined all native threads.
	bool Stop() noexcept {
		if (!Registered) return true;
#if defined(GARGANTUAN_WITH_GNS)
		if (CallbackRegistered) SteamNetworkingSockets_SetServiceThreadInitCallback(nullptr);
#endif
		CallbackRegistered = false;
		FarmServiceTimingEvidence *Expected = this; Owner.compare_exchange_strong(Expected, nullptr);
		Registered = false;
		if (SteamNetworkingSocketsLib::GargantuanServiceTimingObserver.load(std::memory_order_acquire) != &Sink) return false;
		return SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(nullptr);
	}
	void Write(std::ostream &Out, std::string_view Run, bool Joined) const {
		if (!Values) return;
		PrintClock(Out, Run, "BEGIN", Begin); PrintClock(Out, Run, "FAILURE_OBSERVER", Failure);
		const auto Starts = ThreadCount.load(std::memory_order_acquire);
		for (unsigned I = 0; Joined && I < Starts && I < Threads.size(); ++I)
			if (Threads[I].Ready.load(std::memory_order_acquire)) PrintClock(Out, Run, "SERVICE_THREAD", Threads[I].Value);
		std::uint64_t Retained = 0, Earliest = 0, Latest = 0;
		auto Print = [&](std::uint64_t Sequence, const Record &Value) {
			++Retained; if (!Earliest || Value.BeginQpc < Earliest) Earliest = Value.BeginQpc;
			Latest = std::max(Latest, Value.EndQpc);
			Out << "[Qualification:FarmServiceTiming] run=" << Run << " sequence=" << Sequence
				<< " phase=" << static_cast<unsigned>(Value.Phase) << " native_tid=" << Value.ThreadId
				<< " clock_valid=" << Value.ClockValid << " qpc_begin=" << Value.BeginQpc << " qpc_end=" << Value.EndQpc
				<< " count=" << Value.Count << " bytes=" << Value.Bytes << " detail=" << Value.Detail << " result=" << Value.Result
				<< " error=" << Value.Error << " flags=" << Value.Flags << '\n';
		};
		if (Joined) {
			Values->Visit(Print);
			for (unsigned I = 0; I < TimerCount.load() && I < TimerCreates.size(); ++I) Print(0, TimerCreates[I]);
		}
		Out << "[Qualification:FarmServiceTimingSummary] run=" << Run << " enabled=1 joined=" << Joined
			<< " registered=" << Registered << " capacity=" << Capacity << " attempts=" << Values->AttemptCount()
			<< " written=" << Values->WrittenCount() << " overwritten=" << Values->OverwrittenCount()
			<< " dropped=" << Values->DroppedCount() << " counter_overflow=" << Values->Overflowed()
			<< " filtered_after_reservation=" << Values->FilteredCount()
			<< " retained=" << Retained << " earliest_qpc=" << Earliest << " latest_qpc=" << Latest
			<< " timer_creates=" << TimerCount.load() << " service_threads=" << Starts
			<< " timer_overflow=" << (TimerCount.load() > TimerCreates.size()) << " thread_overflow=" << (Starts > Threads.size())
			<< " failure_observed_us=" << FailureObservedUs << " frozen=" << (FailureObservedUs && Failure.Valid)
			<< " certificate_clock=steady_us timing_clock=qpc timer_deadline_clock=gns_local_us"
			<< " native_snapshot_bracket=NOT_MEASURED interval_coverage=NOT_MEASURED attribution=NOT_MEASURED\n";
	}
};
}

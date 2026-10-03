#pragma once

#include "WorkloadTimingEvidence.hpp"
#include <limits>

namespace gargantuan::test_detail {

struct WorkloadSleepSnapshot {
	std::uint64_t Count = 0, RequestedNs = 0, ActualNs = 0, LastCompletedNs = 0;
	bool Valid = false;
};

// One fixture thread owns this ledger. These are the existing measured
// BeforeSleep/AfterSleep windows, not a claim of pure kernel wait duration.
struct WorkloadSleepLedger {
	std::uint64_t Thread, BeginNs = 0, RequestedNs = 0;
	WorkloadSleepSnapshot Completed;
	bool Sleeping = false, Invalid = false;
	explicit WorkloadSleepLedger(std::uint64_t OwnerThread = WorkloadNativeThread()) noexcept : Thread(OwnerThread) {}
	void Begin(std::uint64_t BeforeNs, std::uint64_t Requested, std::uint64_t CallerThread) noexcept {
		if (!Thread || CallerThread != Thread || Sleeping || !BeforeNs || BeforeNs < Completed.LastCompletedNs) {
			Invalid = true; return;
		}
		BeginNs = BeforeNs; RequestedNs = Requested; Sleeping = true;
	}
	void End(std::uint64_t AfterNs, std::uint64_t CallerThread) noexcept {
		if (!Sleeping || CallerThread != Thread || AfterNs < BeginNs) { Invalid = true; return; }
		Sleeping = false;
		const auto Actual = AfterNs - BeginNs;
		constexpr auto Maximum = std::numeric_limits<std::uint64_t>::max();
		if (Completed.Count == Maximum || RequestedNs > Maximum - Completed.RequestedNs || Actual > Maximum - Completed.ActualNs) {
			Invalid = true; return;
		}
		++Completed.Count;
		Completed.RequestedNs += RequestedNs; Completed.ActualNs += Actual; Completed.LastCompletedNs = AfterNs;
	}
	[[nodiscard]] WorkloadSleepSnapshot Snapshot(std::uint64_t Ns, std::uint64_t CallerThread) const noexcept {
		auto Value = Completed;
		Value.Valid = Thread && CallerThread == Thread && !Invalid && !Sleeping && Ns >= Completed.LastCompletedNs;
		return Value;
	}
};

struct WorkloadEndpointCounters {
	WorkloadCpuSample BeforeCpu, AfterCpu;
	std::uint64_t BeforeNs = 0, AfterNs = 0;
	WorkloadSleepSnapshot Sleep;
};

struct WorkloadObservedEndpoint {
	std::chrono::steady_clock::time_point At;
	std::uint64_t Thread = 0;
	WorkloadEndpointCounters Counters;
	[[nodiscard]] static WorkloadObservedEndpoint Capture(const WorkloadSleepLedger &Sleep) noexcept {
		WorkloadObservedEndpoint Value;
		Value.Thread = WorkloadNativeThread();
		Value.Counters.BeforeNs = WorkloadTimestamp(std::chrono::steady_clock::now());
		Value.Counters.BeforeCpu = CaptureWorkloadCpu();
		Value.At = std::chrono::steady_clock::now(); // Original latency clock, never CPU time.
		Value.Counters.AfterCpu = CaptureWorkloadCpu();
		Value.Counters.AfterNs = WorkloadTimestamp(std::chrono::steady_clock::now());
		Value.Counters.Sleep = Sleep.Snapshot(WorkloadTimestamp(Value.At), Value.Thread);
		return Value;
	}
};

struct WorkloadCounterRange { std::uint64_t Lower100ns = 0, Upper100ns = 0; };

[[nodiscard]] inline std::optional<WorkloadCounterRange> WorkloadCpuRange(std::uint64_t StartBefore, std::uint64_t StartAfter,
	std::uint64_t EndBefore, std::uint64_t EndAfter, bool Valid) noexcept {
	if (!Valid || StartBefore > StartAfter || StartAfter > EndBefore || EndBefore > EndAfter) return {};
	return WorkloadCounterRange{EndBefore - StartAfter, EndAfter - StartBefore};
}

inline void PrintRemoteCounters(std::ostream &Output, std::string_view Case, std::string_view Profile,
	std::uint64_t Id, bool Rpc, std::uint64_t StartNs, std::uint64_t EndNs, std::uint64_t StartThread,
	std::uint64_t EndThread, bool Terminal, const WorkloadEndpointCounters &Start, const WorkloadEndpointCounters &End) {
	const bool Boundaries = Terminal && StartThread && StartThread == EndThread && Start.BeforeNs &&
		Start.BeforeNs <= StartNs && StartNs <= Start.AfterNs && Start.AfterNs <= End.BeforeNs &&
		End.BeforeNs <= EndNs && EndNs <= End.AfterNs;
	const auto Thread = WorkloadCpuRange(Start.BeforeCpu.Thread100ns, Start.AfterCpu.Thread100ns,
		End.BeforeCpu.Thread100ns, End.AfterCpu.Thread100ns, Boundaries && Start.BeforeCpu.ThreadValid &&
		Start.AfterCpu.ThreadValid && End.BeforeCpu.ThreadValid && End.AfterCpu.ThreadValid);
	const auto Process = WorkloadCpuRange(Start.BeforeCpu.Process100ns, Start.AfterCpu.Process100ns,
		End.BeforeCpu.Process100ns, End.AfterCpu.Process100ns, Boundaries && Start.BeforeCpu.ProcessValid &&
		Start.AfterCpu.ProcessValid && End.BeforeCpu.ProcessValid && End.AfterCpu.ProcessValid);
	const bool SleepValid = Boundaries && Start.Sleep.Valid && End.Sleep.Valid &&
		Start.Sleep.LastCompletedNs <= StartNs && End.Sleep.LastCompletedNs <= EndNs &&
		Start.Sleep.LastCompletedNs <= End.Sleep.LastCompletedNs &&
		Start.Sleep.Count <= End.Sleep.Count && Start.Sleep.RequestedNs <= End.Sleep.RequestedNs &&
		Start.Sleep.ActualNs <= End.Sleep.ActualNs && End.Sleep.ActualNs - Start.Sleep.ActualNs <= EndNs - StartNs &&
		(End.Sleep.Count == Start.Sleep.Count ?
			(End.Sleep.RequestedNs == Start.Sleep.RequestedNs && End.Sleep.ActualNs == Start.Sleep.ActualNs &&
			 End.Sleep.LastCompletedNs == Start.Sleep.LastCompletedNs) : End.Sleep.LastCompletedNs >= StartNs);
	Output << "[Qualification:RemoteResourceSpan] case=" << Case << " profile=" << Profile << " kind=" << (Rpc ? "RPC" : "EVENT")
		<< " id=" << Id << " start_ns=" << StartNs << " end_ns=" << EndNs << " start_tid=" << StartThread << " end_tid=" << EndThread
		<< " terminal=" << Terminal << " endpoint_order_valid=" << Boundaries
		<< " start_sample_before_ns=" << Start.BeforeNs << " start_sample_after_ns=" << Start.AfterNs
		<< " end_sample_before_ns=" << End.BeforeNs << " end_sample_after_ns=" << End.AfterNs;
	const auto Cpu = [&](std::string_view Label, const WorkloadCpuSample &Value) {
		Output << ' ' << Label << "_thread_100ns=" << Value.Thread100ns << ' ' << Label << "_thread_valid=" << Value.ThreadValid
			<< ' ' << Label << "_process_100ns=" << Value.Process100ns << ' ' << Label << "_process_valid=" << Value.ProcessValid;
	};
	Cpu("start_before", Start.BeforeCpu); Cpu("start_after", Start.AfterCpu);
	Cpu("end_before", End.BeforeCpu); Cpu("end_after", End.AfterCpu);
	const auto Range = [&](std::string_view Label, const std::optional<WorkloadCounterRange> &Value) {
		Output << ' ' << Label << "_cpu_lower_100ns=";
		if (Value) Output << Value->Lower100ns; else Output << "NOT_MEASURED";
		Output << ' ' << Label << "_cpu_upper_100ns=";
		if (Value) Output << Value->Upper100ns; else Output << "NOT_MEASURED";
	};
	Range("thread", Thread); Range("process", Process);
	const auto Sleep = [&](std::string_view Label, const WorkloadSleepSnapshot &Value) {
		Output << ' ' << Label << "_sleep_count=" << Value.Count << ' ' << Label << "_sleep_requested_ns=" << Value.RequestedNs
			<< ' ' << Label << "_sleep_actual_ns=" << Value.ActualNs << ' ' << Label << "_sleep_last_completed_ns=" << Value.LastCompletedNs
			<< ' ' << Label << "_sleep_valid=" << Value.Valid;
	};
	Sleep("start", Start.Sleep); Sleep("end", End.Sleep);
	Output << " sleep_delta_valid=" << SleepValid << " measured_sleep_count=";
	if (SleepValid) Output << End.Sleep.Count - Start.Sleep.Count; else Output << "NOT_MEASURED";
	Output << " measured_sleep_requested_ns=";
	if (SleepValid) Output << End.Sleep.RequestedNs - Start.Sleep.RequestedNs; else Output << "NOT_MEASURED";
	Output << " measured_sleep_actual_ns=";
	if (SleepValid) Output << End.Sleep.ActualNs - Start.Sleep.ActualNs; else Output << "NOT_MEASURED";
	Output << '\n';
}

} // namespace gargantuan::test_detail

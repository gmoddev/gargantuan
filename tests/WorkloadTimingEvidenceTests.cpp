#include "WorkloadTimingEvidence.hpp"
#include "WorkloadActionEvidence.hpp"
#include "WorkloadRemoteEvidence.hpp"

#include <iostream>
#include <memory>
#include <sstream>

int main() {
	using namespace gargantuan::test_detail;
	int Failures = 0;
	const auto Check = [&](bool Condition, const char *Message) {
		if (!Condition) { std::cerr << "FAIL: " << Message << '\n'; ++Failures; }
	};
	WorkloadTimingEvidence Evidence;
	const WorkloadCpuSample Before{100, 200, true, true};
	Evidence.Record(WorkloadPhase::ServerSession, 1, 50, Before, {400100, 1000200, true, true});
	Evidence.Record(WorkloadPhase::ServerSession, 2, 402, Before, {1100, 5200, true, true});
	const auto &Maximum = Evidence.Maxima[static_cast<std::size_t>(WorkloadPhase::ServerSession)];
	Check(Maximum.Step == 2 && Maximum.WallMs == 402 && Maximum.ThreadCpuMs == 0.1 && Maximum.ProcessCpuMs == 0.5,
		"CPU evidence belongs to the maximum wall-time span, not the maximum CPU span");
	Evidence.Record(WorkloadPhase::ServerSession, 3, 20, Before, {2000100, 2000200, true, true});
	Check(Maximum.Step == 2 && Maximum.ThreadCpuMs == 0.1, "short CPU-heavy span cannot overwrite slow-span evidence");
	Evidence.Record(WorkloadPhase::StepInterval, 4, 1400, Before, {100100, 50000200, true, true}, 16.667, 164);
	const auto &Interval = Evidence.Maxima[static_cast<std::size_t>(WorkloadPhase::StepInterval)];
	Check(Interval.RequestedSleepMs == 16.667 && Interval.ActualSleepMs == 164 && Interval.ProcessCpuMs == 5000,
		"whole interval retains matching sleep and multicore process CPU without clamping to wall time");
	Check(!WorkloadTimingEvidence::CpuDelta(10, 5, true, true), "CPU counter rollback is not zero CPU");
	Check(!WorkloadTimingEvidence::CpuDelta(0, 0, false, true), "missing beginning CPU sample is not zero CPU");
	Check(!WorkloadTimingEvidence::CpuDelta(0, 0, true, false), "missing ending CPU sample is not zero CPU");
	Check(WorkloadTimingEvidence::CpuDelta(5, 5, true, true) == 0, "measured zero CPU remains distinguishable");
	Evidence.Record(WorkloadPhase::Observer, 5, 100, {}, {});
	std::ostringstream Text;
	Evidence.Print(Text, "recovery");
	Check(Text.str().find("case=recovery phase=observer step=5 wall_ms=100 thread_cpu_ms=NOT_MEASURED process_cpu_ms=NOT_MEASURED") != std::string::npos,
		"unsupported or failed CPU reads are explicit in the retained tuple");
	const auto First = CaptureWorkloadCpu();
	const auto Last = CaptureWorkloadCpu();
#if defined(_WIN32)
	Check(First.ThreadValid && First.ProcessValid && Last.ThreadValid && Last.ProcessValid,
		"Windows current process/thread CPU counters are accessible");
	Check(Last.Thread100ns >= First.Thread100ns && Last.Process100ns >= First.Process100ns,
		"Windows current process/thread CPU counters are monotonic");
#else
	Check(!First.ThreadValid && !Last.ThreadValid && !First.ProcessValid && !Last.ProcessValid,
		"unsupported platforms report counters as not measured");
#endif
	WorkloadActionEvidence Actions;
	Actions.Begin(7, 180, 1'000'000'000, Before, false);
	Actions.End(7, 239, 3'227'250'000, {100, 200, true, true}, false);
	Actions.Begin(8, 240, 4'000'000'000, {}, true);
	Actions.End(8, 241, 4'001'000'000, {}, true);
	Actions.Begin(9, 242, 5'000'000'000, Before, true);
	std::ostringstream ActionText;
	Actions.Print(ActionText, "mixed", 300, 25'000'000'000, {200100, 300200, true, true}, true);
	Check(ActionText.str().find("sequence=7 recovery_probe=0 outcome=COMPLETED start_step=180 observed_end_step=239 start_ns=1000000000 observed_end_ns=3227250000 wall_ms=2227.25 thread_cpu_ms=0 process_cpu_ms=0") != std::string::npos,
		"action tuple joins original submission and resolution, retaining slow off-CPU span without latency discount");
	Check(ActionText.str().find("sequence=8 recovery_probe=1 outcome=REJECTED") != std::string::npos &&
		ActionText.str().find("wall_ms=1 thread_cpu_ms=NOT_MEASURED process_cpu_ms=NOT_MEASURED") != std::string::npos,
		"rejected action and unavailable CPU are distinguishable from successful zero CPU");
	Check(ActionText.str().find("sequence=9 recovery_probe=1 outcome=MISSING_AT_SERVICE_DEADLINE") != std::string::npos &&
		Actions.Records[2].Outcome == "PENDING", "missing completion is an observation, not a fabricated terminal event");
	std::ostringstream EarlyText;
	Actions.Print(EarlyText, "mixed", 243, 5'010'000'000, {}, false);
	Check(EarlyText.str().find("outcome=MISSING_AT_CASE_END") != std::string::npos,
		"early disconnected case end does not claim a service timeout");
	Actions.End(9, 244, 5'020'000'000, Before, false);
	Actions.End(9, 245, 5'030'000'000, Before, false);
	Check(Actions.Invalid && Actions.Records[2].EndStep == 244, "duplicate completion cannot overwrite first evidence");
	WorkloadActionEvidence WrongIdentity;
	WrongIdentity.Begin(1, 1, 10, {}, false);
	WrongIdentity.End(2, 2, 20, {}, false);
	Check(WrongIdentity.Invalid && WrongIdentity.Records[0].Outcome == "PENDING", "wrong sequence cannot resolve an action");
	WorkloadActionEvidence Bounded;
	for (std::size_t Index = 0; Index < WorkloadActionEvidence::Capacity; ++Index) {
		Bounded.Begin(Index + 1, Index, Index * 10, Before, false);
		Bounded.End(Index + 1, Index + 1, Index * 10 + 1, Before, false);
	}
	Bounded.Begin(WorkloadActionEvidence::Capacity + 1, 100, 1000, Before, true);
	Check(Bounded.Count == WorkloadActionEvidence::Capacity && Bounded.Overflow && !Bounded.Invalid,
		"bounded diagnostics fail visibly without overwriting chronology or affecting workload scheduling");
	std::ostringstream BoundedText;
	Bounded.Print(BoundedText, "mixed", 100, 1000, Before, false);
	Check(BoundedText.str().find("records=29 capacity=29 invalid=0 overflow=1") != std::string::npos,
		"receipt exposes truncation rather than claiming complete evidence");
	Evidence.Record(WorkloadPhase::Sleep, 8, 833.651, Before, Before, 16.4441, 833.651, 100, 833651100);
	Evidence.Record(WorkloadPhase::Sleep, 9, 20, Before, Before, 16, 20, 900000000, 920000000);
	const auto &Sleep = Evidence.Maxima[static_cast<std::size_t>(WorkloadPhase::Sleep)];
	Check(Sleep.StartNs == 100 && Sleep.EndNs == 833651100 && Sleep.Step == 8 && Sleep.ThreadCpuMs == 0,
		"maximum phase retains its own original start/end timestamps and CPU, never independent samples");
	std::ostringstream PhaseText;
	Evidence.Print(PhaseText, "recovery");
	Check(PhaseText.str().find("start_ns=100 end_ns=833651100 timestamps_valid=1") != std::string::npos &&
		PhaseText.str().find("start_ns=0 end_ns=0 timestamps_valid=0") != std::string::npos,
		"unmeasured legacy phase timestamps cannot impersonate exact observed interval");
	const auto Anchor = WorkloadClockAnchor::Capture();
	Check(Anchor.SteadyNs > 0, "clock anchor has original monotonic observation");
#if defined(_WIN32)
	Check(Anchor.NativeValid && Anchor.Process == GetCurrentProcessId() && Anchor.Thread == GetCurrentThreadId() &&
		Anchor.QpcFrequency > 0 && Anchor.QpcAfter >= Anchor.QpcBefore, "Windows anchor brackets clock with QPC and exact native identity");
#else
	Check(!Anchor.NativeValid && !Anchor.Thread && !Anchor.Process && !Anchor.QpcFrequency,
		"unsupported native clock identity is explicitly absent");
#endif
	std::ostringstream AnchorText;
	Anchor.Print(AnchorText, "mixed", "FULL_RESERVATION", "BEGIN");
	Check(AnchorText.str().find("case=mixed profile=FULL_RESERVATION boundary=BEGIN pid=") != std::string::npos,
		"deferred anchor identifies exact case/profile boundary");
	auto RemoteStorage = std::make_unique<WorkloadRemoteEvidence>();
	auto &Remote = *RemoteStorage;
	static_assert(sizeof(WorkloadRemoteEvidence) < 384 * 1024);
	Remote.Begin(40, true, false, 48, 1000, 123);
	Remote.Submitted(40, true, 48, 1100, 123);
	Remote.Begin(41, false, false, 48, 1200, 123);
	Remote.Submitted(41, true, 48, 1300, 123);
	Remote.End(41, 52, 491775200, 123, -1, true);
	Remote.End(40, 52, 491792000, 123, 0, true);
	Remote.Begin(42, true, false, 54, 500000000, 123);
	Remote.Submitted(42, false, 54, 500001000, 123);
	Remote.Begin(43, true, true, 60, 600000000, 123);
	Remote.End(43, 60, 600000100, 123, 1, false); // Synchronous callback before API return is valid.
	Remote.Submitted(43, true, 60, 600000200, 123);
	Remote.Begin(44, false, true, 65, 700000000, 123);
	Remote.Submitted(44, true, 65, 700000100, 123);
	Check(!Remote.Invalid && Remote.Count == 5 && Remote.Records[0].EndNs == 491792000 && Remote.Records[1].EndNs == 491775200,
		"concurrent RPC/Event completions bind exact existing IDs despite reversed callback order");
	std::ostringstream RemoteText;
	Remote.Print(RemoteText, "recovery", "FULL_RESERVATION", 70, 20700000000, true);
	Check(RemoteText.str().find("kind=RPC id=42 recovery_probe=0 outcome=REJECTED") != std::string::npos &&
		RemoteText.str().find("kind=RPC id=43 recovery_probe=1 outcome=TERMINAL") != std::string::npos &&
		RemoteText.str().find("terminal_status=1 payload_matched=0") != std::string::npos,
		"submission rejection is separate from accepted timeout callback and payload diagnostics");
	Check(RemoteText.str().find("kind=EVENT id=44 recovery_probe=1 outcome=MISSING_AT_SERVICE_DEADLINE") != std::string::npos &&
		Remote.Records[4].EndNs == 0 && !Remote.Records[4].Terminal,
		"missing Event callback reports observation without fabricating completion timestamp");
	Remote.Begin(45, true, true, 66, 800000000, 123);
	std::ostringstream IncompleteRemote;
	Remote.Print(IncompleteRemote, "recovery", "FULL_RESERVATION", 70, 900000000, false);
	Check(IncompleteRemote.str().find("id=44 recovery_probe=1 outcome=MISSING_AT_CASE_END") != std::string::npos &&
		IncompleteRemote.str().find("id=45 recovery_probe=1 outcome=SUBMISSION_NOT_OBSERVED") != std::string::npos,
		"early case end and unobserved submission cannot be promoted to timeout or completion");
	Remote.End(40, 53, 491793000, 123, 0, true);
	Check(Remote.Invalid && Remote.Records[0].EndNs == 491792000, "duplicate callback cannot overwrite first terminal evidence");
	auto InvalidRemoteStorage = std::make_unique<WorkloadRemoteEvidence>();
	auto &InvalidRemote = *InvalidRemoteStorage;
	InvalidRemote.Begin(1, true, false, 1, 100, 1);
	InvalidRemote.Submitted(1, true, 2, 200, 1);
	InvalidRemote.End(1, 1, 150, 1, 0, true);
	Check(InvalidRemote.Invalid && !InvalidRemote.Records[0].Terminal, "callback preceding completed submission is rejected");
	InvalidRemote.Reset();
	InvalidRemote.Begin(1, false, false, 1, 100, 1);
	InvalidRemote.End(2, 2, 200, 1, -1, true);
	Check(InvalidRemote.Invalid && !InvalidRemote.Records[0].Terminal, "unknown Remote ID cannot satisfy an outstanding operation");
	InvalidRemote.Reset();
	InvalidRemote.Begin(1, true, false, 1, 100, 1);
	InvalidRemote.Submitted(1, false, 1, 101, 1);
	InvalidRemote.End(1, 2, 200, 1, 0, true);
	Check(InvalidRemote.Invalid && !InvalidRemote.Records[0].Terminal, "rejected submission cannot later be claimed accepted completion");
	auto RemoteBoundStorage = std::make_unique<WorkloadRemoteEvidence>();
	auto &RemoteBound = *RemoteBoundStorage;
	for (std::size_t Index = 0; Index < WorkloadRemoteEvidence::Capacity; ++Index) {
		RemoteBound.Begin(Index + 1, true, false, Index, Index * 10 + 1, 123);
		RemoteBound.Submitted(Index + 1, true, Index, Index * 10 + 2, 123);
		RemoteBound.End(Index + 1, Index, Index * 10 + 3, 123, 0, true);
	}
	RemoteBound.Begin(WorkloadRemoteEvidence::Capacity + 1, false, true, 2000, 20000, 123);
	RemoteBound.Submitted(WorkloadRemoteEvidence::Capacity + 1, true, 2000, 20001, 123);
	RemoteBound.End(WorkloadRemoteEvidence::Capacity + 1, 2001, 20002, 123, -1, true);
	Check(RemoteBound.Overflow && !RemoteBound.Invalid && RemoteBound.Count == WorkloadRemoteEvidence::Capacity &&
		RemoteBound.Records[0].EndNs == 3, "diagnostic overflow preserves prior records and never limits workload execution");
	std::ostringstream RemoteBoundText;
	RemoteBound.Print(RemoteBoundText, "mixed", "POOLED_SERVICE", 2002, 20003, false);
	Check(RemoteBoundText.str().find("records=1062 capacity=1062 invalid=0 overflow=1") != std::string::npos,
		"Remote diagnostic truncation remains explicit and bounded");
	WorkloadSleepLedger Sleeps(123);
	Sleeps.Begin(10, 5, 123); Sleeps.End(20, 123); // Completed before this Remote begins.
	const auto StartSleep = Sleeps.Snapshot(100, 123);
	Sleeps.Begin(200, 40, 123);
	Check(!Sleeps.Snapshot(210, 123).Valid, "an endpoint inside an active measured sleep cannot claim whole-sleep attribution");
	Sleeps.End(260, 123);
	Sleeps.Begin(300, 0, 123); Sleeps.End(310, 123); // Already-late sleep request remains zero.
	const auto EndSleep = Sleeps.Snapshot(500, 123);
	Sleeps.Begin(600, 90, 123); Sleeps.End(800, 123); // Completed after this Remote ends.
	Check(EndSleep.Valid && EndSleep.Count - StartSleep.Count == 2 &&
		EndSleep.RequestedNs - StartSleep.RequestedNs == 40 && EndSleep.ActualNs - StartSleep.ActualNs == 70,
		"per-Remote frozen cumulative differences contain only its two completed measured sleep intervals");
	WorkloadEndpointCounters StartCounters{{100, 200, true, true}, {110, 220, true, true}, 90, 110, StartSleep};
	WorkloadEndpointCounters EndCounters{{310, 10220, true, true}, {330, 10420, true, true}, 490, 510, EndSleep};
	Remote.Reset();
	Remote.Begin(190, true, false, 400, 100, 123, StartCounters);
	Remote.Begin(191, false, false, 400, 120, 123, StartCounters);
	Remote.End(191, 404, 520, 123, -1, true, EndCounters);
	Remote.End(190, 404, 500, 123, 0, true, EndCounters);
	Remote.Submitted(190, true, 404, 501, 123);
	Remote.Submitted(191, true, 404, 521, 123);
	Check(!Remote.Invalid && Remote.Records[0].Id == 190 && Remote.Records[1].Id == 191 &&
		Remote.Records[0].StartCounters.BeforeCpu.Thread100ns == 100 &&
		Remote.Records[0].EndCounters.Sleep.Count == EndSleep.Count,
		"CPU/sleep endpoints stay attached to exact concurrent Remote IDs with reversed or synchronous completion");
	const auto PrintCounters = [&](const WorkloadEndpointCounters &Begin, const WorkloadEndpointCounters &End,
		std::uint64_t EndThread = 123, bool Terminal = true) {
		std::ostringstream Text;
		PrintRemoteCounters(Text, "burst-concurrent", "FULL_RESERVATION", 190, true,
			100, 500, 123, EndThread, Terminal, Begin, End);
		return Text.str();
	};
	const auto CounterText = PrintCounters(StartCounters, EndCounters);
	Check(CounterText.find("thread_cpu_lower_100ns=200 thread_cpu_upper_100ns=230") != std::string::npos &&
		CounterText.find("process_cpu_lower_100ns=10000 process_cpu_upper_100ns=10220") != std::string::npos,
		"CPU endpoints yield counter bounds without clamping multicore process counters to wall time");
	Check(CounterText.find("sleep_delta_valid=1 measured_sleep_count=2 measured_sleep_requested_ns=40 measured_sleep_actual_ns=70") != std::string::npos,
		"exact Remote ID retains matched requested and actual sleep totals instead of unrelated phase maxima");
	Check(PrintCounters(StartCounters, EndCounters, 999).find("thread_cpu_lower_100ns=NOT_MEASURED") != std::string::npos &&
		PrintCounters(StartCounters, EndCounters, 123, false).find("measured_sleep_count=NOT_MEASURED") != std::string::npos,
		"wrong callback thread or missing terminal boundary never fabricates resource attribution");
	auto InvalidCounters = EndCounters;
	InvalidCounters.BeforeCpu.ThreadValid = false;
	Check(PrintCounters(StartCounters, InvalidCounters).find("thread_cpu_lower_100ns=NOT_MEASURED") != std::string::npos,
		"unavailable endpoint CPU sample remains unmeasured");
	InvalidCounters = EndCounters; InvalidCounters.BeforeCpu.Thread100ns = 1;
	Check(PrintCounters(StartCounters, InvalidCounters).find("thread_cpu_lower_100ns=NOT_MEASURED") != std::string::npos,
		"CPU rollback cannot become a zero delta");
	InvalidCounters = EndCounters; InvalidCounters.BeforeNs = 99;
	Check(PrintCounters(StartCounters, InvalidCounters).find("endpoint_order_valid=0") != std::string::npos,
		"overlapping endpoint sampling envelopes invalidate resource bounds");
	InvalidCounters = EndCounters; InvalidCounters.Sleep.Count = StartCounters.Sleep.Count;
	Check(PrintCounters(StartCounters, InvalidCounters).find("sleep_delta_valid=0") != std::string::npos,
		"inconsistent frozen cumulative sleep count and totals cannot pass");
	WorkloadSleepLedger WrongSleep(123);
	WrongSleep.Begin(10, 5, 123); WrongSleep.End(20, 999);
	Check(!WrongSleep.Snapshot(30, 123).Valid, "foreign-thread sleep completion is invalid");
	WorkloadSleepLedger OverflowSleep(123);
	OverflowSleep.Completed.ActualNs = std::numeric_limits<std::uint64_t>::max();
	OverflowSleep.Begin(10, 0, 123); OverflowSleep.End(11, 123);
	Check(OverflowSleep.Invalid && !OverflowSleep.Snapshot(20, 123).Valid, "sleep sum overflow cannot wrap to plausible evidence");
	const auto Zero = WorkloadCpuRange(5, 5, 5, 5, true);
	Check(Zero && Zero->Lower100ns == 0 && Zero->Upper100ns == 0,
		"sampled zero CPU counters remain explicit bounds, not a claim of no physical execution");
	Check(RemoteBoundText.str().size() < 2 * 1024 * 1024,
		"maximum bounded Remote log remains below two MiB per case");
	return Failures == 0 ? 0 : 1;
}

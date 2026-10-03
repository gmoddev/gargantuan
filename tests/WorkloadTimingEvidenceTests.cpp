#include "WorkloadTimingEvidence.hpp"
#include "WorkloadActionEvidence.hpp"

#include <iostream>
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
	return Failures == 0 ? 0 : 1;
}

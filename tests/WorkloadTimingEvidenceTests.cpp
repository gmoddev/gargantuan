#include "WorkloadTimingEvidence.hpp"

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
	return Failures == 0 ? 0 : 1;
}

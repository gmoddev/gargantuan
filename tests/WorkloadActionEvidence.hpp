#pragma once

#include "WorkloadTimingEvidence.hpp"

namespace gargantuan::test_detail {

// Eight opportunities in the existing 480-step/60-step workload, plus at
// most 21 recovery opportunities separated by one second up to the existing
// 20-second service deadline (including a boundary observation). This bounds
// diagnostics only; it neither schedules actions nor limits the workload.
struct WorkloadActionEvidence {
	static constexpr std::size_t Capacity = 480 / 60 + 20 + 1;
	struct Record {
		std::uint64_t Sequence = 0, StartStep = 0, EndStep = 0, StartNs = 0, EndNs = 0;
		WorkloadCpuSample StartCpu, EndCpu;
		bool Recovery = false;
		std::string_view Outcome = "PENDING";
	};
	std::array<Record, Capacity> Records{};
	std::size_t Count = 0;
	bool Invalid = false, Overflow = false;

	void Begin(std::uint64_t Sequence, std::uint64_t Step, std::uint64_t Ns,
		const WorkloadCpuSample &Cpu, bool Recovery) noexcept {
		if (!Sequence || (Count && (Records[Count - 1].Outcome == "PENDING" ||
			Sequence <= Records[Count - 1].Sequence))) { Invalid = true; return; }
		if (Count == Capacity) { Overflow = true; return; }
		Records[Count++] = {.Sequence = Sequence, .StartStep = Step, .StartNs = Ns,
			.StartCpu = Cpu, .Recovery = Recovery};
	}

	void End(std::uint64_t Sequence, std::uint64_t Step, std::uint64_t Ns,
		const WorkloadCpuSample &Cpu, bool Rejected) noexcept {
		if (!Count || Records[Count - 1].Sequence != Sequence || Records[Count - 1].Outcome != "PENDING" ||
			Step < Records[Count - 1].StartStep || Ns < Records[Count - 1].StartNs) { Invalid = true; return; }
		auto &Value = Records[Count - 1];
		Value.EndStep = Step;
		Value.EndNs = Ns;
		Value.EndCpu = Cpu;
		Value.Outcome = Rejected ? "REJECTED" : "COMPLETED";
	}

	void Print(std::ostream &Output, std::string_view Case, std::uint64_t ObservedStep,
		std::uint64_t ObservedNs, const WorkloadCpuSample &ObservedCpu, bool ServiceDeadlineReached,
		std::string_view Profile = "NOT_MEASURED") const {
		Output << "[Qualification:ActionChronology] case=" << Case << " profile=" << Profile << " records=" << Count
			<< " capacity=" << Capacity << " invalid=" << Invalid << " overflow=" << Overflow << '\n';
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto &Value = Records[Index];
			const bool Pending = Value.Outcome == "PENDING";
			const auto EndNs = Pending ? ObservedNs : Value.EndNs;
			const auto &Cpu = Pending ? ObservedCpu : Value.EndCpu;
			Output << "[Qualification:ActionSpan] case=" << Case << " profile=" << Profile << " sequence=" << Value.Sequence
				<< " recovery_probe=" << Value.Recovery << " outcome="
				<< (Pending ? (ServiceDeadlineReached ? "MISSING_AT_SERVICE_DEADLINE" : "MISSING_AT_CASE_END") : Value.Outcome)
				<< " start_step=" << Value.StartStep << " observed_end_step=" << (Pending ? ObservedStep : Value.EndStep)
				<< " start_ns=" << Value.StartNs << " observed_end_ns=" << EndNs << " wall_ms=";
			if (EndNs >= Value.StartNs) Output << static_cast<double>(EndNs - Value.StartNs) / 1'000'000.0;
			else Output << "NOT_MEASURED";
			Output << " thread_cpu_ms=";
			const auto Thread = WorkloadTimingEvidence::CpuDelta(Value.StartCpu.Thread100ns, Cpu.Thread100ns,
				Value.StartCpu.ThreadValid, Cpu.ThreadValid);
			if (Thread) Output << *Thread; else Output << "NOT_MEASURED";
			Output << " process_cpu_ms=";
			const auto Process = WorkloadTimingEvidence::CpuDelta(Value.StartCpu.Process100ns, Cpu.Process100ns,
				Value.StartCpu.ProcessValid, Cpu.ProcessValid);
			if (Process) Output << *Process; else Output << "NOT_MEASURED";
			Output << '\n';
		}
	}
};

} // namespace gargantuan::test_detail

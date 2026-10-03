#pragma once

#include "WorkloadTimingEvidence.hpp"
#include "WorkloadRemoteSpanCounters.hpp"

namespace gargantuan::test_detail {

// Conservative product of existing 480 frames, minimum eight-tick interval,
// maximum 16 RPCs plus one Event; plus two probes per second over 20 seconds.
// This limits diagnostics only. It neither admits nor schedules Remote work.
struct WorkloadRemoteEvidence {
	static constexpr std::size_t Capacity = (480 / 8) * (16 + 1) + 2 * (20 + 1);
	struct Record {
		std::uint64_t Id = 0, StartStep = 0, SubmittedStep = 0, EndStep = 0;
		std::uint64_t StartNs = 0, SubmittedNs = 0, EndNs = 0;
		std::uint64_t StartThread = 0, SubmittedThread = 0, EndThread = 0;
		int TerminalStatus = -1;
		bool Rpc = false, Recovery = false, Decided = false, Accepted = false, Terminal = false;
		bool PayloadMatched = false;
		WorkloadEndpointCounters StartCounters, EndCounters;
	};
	std::array<Record, Capacity> Records{};
	std::size_t Count = 0;
	bool Invalid = false, Overflow = false;
	void Reset() noexcept {
		for (auto &Value : Records) Value = Record{};
		Count = 0; Invalid = false; Overflow = false;
	}

	void Begin(std::uint64_t Id, bool Rpc, bool Recovery, std::uint64_t Step,
		std::uint64_t Ns, std::uint64_t Thread, const WorkloadEndpointCounters &Counters = {}) noexcept {
		if (Overflow) return;
		if (!Id || !Ns || (Count && Id != Records[0].Id + Count)) { Invalid = true; return; }
		if (Count == Capacity) { Overflow = true; return; }
		Records[Count++] = {.Id = Id, .StartStep = Step, .StartNs = Ns,
			.StartThread = Thread, .Rpc = Rpc, .Recovery = Recovery, .StartCounters = Counters};
	}
	[[nodiscard]] Record *Find(std::uint64_t Id) noexcept {
		if (!Count || Id < Records[0].Id) { Invalid = true; return nullptr; }
		const auto Index = Id - Records[0].Id;
		if (Index >= Count) {
			if (!(Overflow && Index >= Capacity)) Invalid = true;
			return nullptr;
		}
		return &Records[static_cast<std::size_t>(Index)];
	}
	void Submitted(std::uint64_t Id, bool Accepted, std::uint64_t Step,
		std::uint64_t Ns, std::uint64_t Thread) noexcept {
		auto *Value = Find(Id);
		if (!Value) return;
		if (Value->Decided || Step < Value->StartStep || Ns < Value->StartNs ||
			(Value->Terminal && (!Accepted || Ns < Value->EndNs || Step < Value->EndStep))) {
			Invalid = true; return;
		}
		Value->Decided = true; Value->Accepted = Accepted;
		Value->SubmittedStep = Step; Value->SubmittedNs = Ns; Value->SubmittedThread = Thread;
	}
	void End(std::uint64_t Id, std::uint64_t Step, std::uint64_t Ns, std::uint64_t Thread,
		int TerminalStatus, bool PayloadMatched, const WorkloadEndpointCounters &Counters = {}) noexcept {
		auto *Value = Find(Id);
		if (!Value) return;
		if (Value->Terminal || (Value->Decided && (!Value->Accepted || Ns < Value->SubmittedNs || Step < Value->SubmittedStep)) ||
			Step < Value->StartStep || Ns < Value->StartNs) {
			Invalid = true; return;
		}
		Value->Terminal = true; Value->EndStep = Step; Value->EndNs = Ns; Value->EndThread = Thread;
		Value->TerminalStatus = TerminalStatus; Value->PayloadMatched = PayloadMatched;
		Value->EndCounters = Counters;
	}
	void Print(std::ostream &Output, std::string_view Case, std::string_view Profile,
		std::uint64_t ObservedStep, std::uint64_t ObservedNs, bool Deadline) const {
		Output << "[Qualification:RemoteChronology] case=" << Case << " profile=" << Profile << " records=" << Count
			<< " capacity=" << Capacity << " invalid=" << Invalid << " overflow=" << Overflow << '\n';
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto &Value = Records[Index];
			const auto Outcome = !Value.Decided ? "SUBMISSION_NOT_OBSERVED" : !Value.Accepted ? "REJECTED" :
				Value.Terminal ? "TERMINAL" : Deadline ? "MISSING_AT_SERVICE_DEADLINE" : "MISSING_AT_CASE_END";
			Output << "[Qualification:RemoteSpan] case=" << Case << " profile=" << Profile << " kind=" << (Value.Rpc ? "RPC" : "EVENT")
				<< " id=" << Value.Id << " recovery_probe=" << Value.Recovery << " outcome=" << Outcome
				<< " start_step=" << Value.StartStep << " start_ns=" << Value.StartNs << " start_tid=" << Value.StartThread
				<< " submitted_step=" << Value.SubmittedStep << " submitted_ns=" << Value.SubmittedNs << " submitted_tid=" << Value.SubmittedThread
				<< " accepted=" << Value.Accepted << " terminal=" << Value.Terminal << " end_step=" << Value.EndStep
				<< " end_ns=" << Value.EndNs << " end_tid=" << Value.EndThread << " terminal_status=" << Value.TerminalStatus
				<< " payload_matched=" << Value.PayloadMatched << " observed_step=" << ObservedStep << " observed_ns=" << ObservedNs << '\n';
			PrintRemoteCounters(Output, Case, Profile, Value.Id, Value.Rpc, Value.StartNs, Value.EndNs,
				Value.StartThread, Value.EndThread, Value.Terminal, Value.StartCounters, Value.EndCounters);
		}
	}
};

} // namespace gargantuan::test_detail

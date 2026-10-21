#pragma once

#include "WorkloadRemoteEvidence.hpp"
#include <algorithm>
#include <string>

namespace gargantuan::test_detail {

// Diagnostics only. Bounds follow the existing 480-frame/40-frame submission
// cadence and final overload submission, never admission or latency policy.
struct AggregatePeerEvidence {
	struct Identity {
		std::uint32_t Phase = 0, Peer = 0, Slot = 0, Generation = 0;
	};
	using RemoteEvidence = BoundedWorkloadRemoteEvidence<(480 / 40) * 16 + 16>;
	RemoteEvidence Remote;
	std::uint64_t RequestSequence = 0;
	std::uint32_t Peer = 0, Slot = 0, Generation = 0, Phase = 0;
	bool Configured = false, Invalid = false;
	[[nodiscard]] Identity CaptureIdentity() const noexcept { return {Phase, Peer, Slot, Generation}; }
	void End(const Identity &Captured, std::uint64_t Id, std::uint64_t Step,
		std::uint64_t Ns, std::uint64_t Thread, int TerminalStatus, bool PayloadMatched,
		const WorkloadEndpointCounters &Counters = {}) noexcept {
		if (!Configured || Captured.Phase != Phase || Captured.Peer != Peer ||
			Captured.Slot != Slot || Captured.Generation != Generation) {
			Invalid = true; return;
		}
		Remote.End(Id, Step, Ns, Thread, TerminalStatus, PayloadMatched, Counters);
	}
	void Configure(std::uint32_t PhaseValue, std::uint32_t PeerValue,
		std::uint32_t SlotValue, std::uint32_t GenerationValue) noexcept {
		if (PhaseValue > 2 || PeerValue >= 32 || !SlotValue || !GenerationValue || (!Configured && PhaseValue != 0) ||
			(Configured && (PhaseValue != Phase + 1 || PeerValue != Peer || SlotValue != Slot || GenerationValue != Generation))) {
			Invalid = true; return;
		}
		Phase = PhaseValue; Peer = PeerValue; Slot = SlotValue; Generation = GenerationValue;
		Configured = true; RequestSequence = 0; Remote.Reset();
	}
	[[nodiscard]] std::string Context() const {
		return " phase_index=" + std::to_string(Phase) + " peer=" + std::to_string(Peer) +
			" slot=" + std::to_string(Slot) + " generation=" + std::to_string(Generation);
	}
};

struct AggregateOperationSpan {
	std::uint64_t StartNs = 0, EndNs = 0, Thread = 0;
	WorkloadCpuSample Before, After;
	std::uint32_t Slot = 0, Generation = 0;
	[[nodiscard]] bool Valid() const noexcept { return StartNs && EndNs >= StartNs; }
	[[nodiscard]] std::uint64_t Duration() const noexcept { return Valid() ? EndNs - StartNs : 0; }
};

struct AggregateStepOperations {
	std::uint64_t Tick = 0, StartedNs = 0, EndedNs = 0;
	std::uint64_t GapStartedNs = 0;
	WorkloadCpuSample Before, After;
	// Peers 0..31 plus the server at index32; subphases Poll/Engine/Session.
	std::array<std::array<AggregateOperationSpan, 3>, 33> Spans{};
};

struct AggregateOperationEvidence {
	static constexpr std::size_t SnapshotCapacity = 64;
	std::array<AggregateStepOperations, SnapshotCapacity> Snapshots{};
	std::array<std::array<AggregateOperationSpan, 3>, 33> Maxima{};
	std::array<std::array<std::uint64_t, 3>, 33> MaximumTicks{};
	std::size_t Count = 0;
	std::uint64_t ObservedSteps = 0;
	bool Overflow = false, Invalid = false;
	void Reset() noexcept { Count = 0; ObservedSteps = 0; Overflow = Invalid = false; Maxima = {}; MaximumTicks = {}; }
	void Record(const AggregateStepOperations &Step, double GapMs) noexcept {
		++ObservedSteps;
		if (!Step.StartedNs || !Step.GapStartedNs || Step.GapStartedNs > Step.StartedNs || Step.EndedNs < Step.StartedNs) {
			Invalid = true; return;
		}
		for (std::size_t Peer = 0; Peer < Step.Spans.size(); ++Peer)
			for (std::size_t Phase = 0; Phase < 3; ++Phase) {
				const auto &Span = Step.Spans[Peer][Phase];
				if (!Span.StartNs && !Span.EndNs) continue; // Uninstantiated diagnostic peers are absent.
				if (!Span.Valid() || Span.StartNs < Step.StartedNs || Span.EndNs > Step.EndedNs) { Invalid = true; continue; }
				if (!Maxima[Peer][Phase].Valid() || Span.Duration() > Maxima[Peer][Phase].Duration()) {
					Maxima[Peer][Phase] = Span; MaximumTicks[Peer][Phase] = Step.Tick;
				}
			}
		// Existing RPC p95 gate is used only to select diagnostic snapshots.
		// Shorter operations and other intervals remain explicitly unsampled.
		if (GapMs < 150) return;
		if (Count == SnapshotCapacity) { Overflow = true; return; }
		Snapshots[Count++] = Step;
	}
	static void PrintSpan(std::ostream &Output, std::string_view Case, std::string_view Profile,
		std::string_view Coverage, std::uint32_t Phase, std::uint32_t Peer, std::uint64_t Tick,
		std::size_t Subphase, const AggregateOperationSpan &Span) {
		static constexpr std::array<std::string_view, 3> Names{"poll", "engine", "session"};
		const auto ThreadCpu = WorkloadTimingEvidence::CpuDelta(Span.Before.Thread100ns, Span.After.Thread100ns,
			Span.Before.ThreadValid, Span.After.ThreadValid);
		const auto ProcessCpu = WorkloadTimingEvidence::CpuDelta(Span.Before.Process100ns, Span.After.Process100ns,
			Span.Before.ProcessValid, Span.After.ProcessValid);
		Output << "[Qualification:AggregateOperationSpan] case=" << Case << " profile=" << Profile
			<< " phase_index=" << Phase << " peer=" << Peer << " side=" << (Peer == 32 ? "server" : "client")
			<< " slot=" << Span.Slot << " generation=" << Span.Generation
			<< " coverage=" << Coverage << " tick=" << Tick << " subphase=" << Names[Subphase]
			<< " start_ns=" << Span.StartNs << " end_ns=" << Span.EndNs << " native_tid=" << Span.Thread
			<< " timestamps_valid=" << Span.Valid() << " thread_cpu_ms=";
		if (ThreadCpu) Output << *ThreadCpu; else Output << "NOT_MEASURED";
		Output << " process_cpu_ms=";
		if (ProcessCpu) Output << *ProcessCpu; else Output << "NOT_MEASURED";
		Output << " before_thread_100ns=" << Span.Before.Thread100ns << " before_thread_valid=" << Span.Before.ThreadValid
			<< " after_thread_100ns=" << Span.After.Thread100ns << " after_thread_valid=" << Span.After.ThreadValid
			<< " before_process_100ns=" << Span.Before.Process100ns << " before_process_valid=" << Span.Before.ProcessValid
			<< " after_process_100ns=" << Span.After.Process100ns << " after_process_valid=" << Span.After.ProcessValid << '\n';
	}
	void Print(std::ostream &Output, std::string_view Case, std::string_view Profile, std::uint32_t Phase) const {
		Output << "[Qualification:AggregateOperationCoverage] case=" << Case << " profile=" << Profile
			<< " phase_index=" << Phase << " observed_steps=" << ObservedSteps << " snapshots=" << Count
			<< " capacity=" << SnapshotCapacity << " invalid=" << Invalid << " overflow=" << Overflow
			<< " selection=existing_p95_gap_ge_150ms coverage=SAMPLED_NOT_COMPLETE_CAUSAL_PROOF\n";
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto &Step = Snapshots[Index];
			Output << "[Qualification:AggregateStepSnapshot] case=" << Case << " profile=" << Profile
				<< " phase_index=" << Phase << " tick=" << Step.Tick << " gap_start_ns=" << Step.GapStartedNs
				<< " gap_end_ns=" << Step.EndedNs << " work_start_ns=" << Step.StartedNs << " work_end_ns=" << Step.EndedNs
				<< " before_thread_100ns=" << Step.Before.Thread100ns << " before_thread_valid=" << Step.Before.ThreadValid
				<< " after_thread_100ns=" << Step.After.Thread100ns << " after_thread_valid=" << Step.After.ThreadValid
				<< " before_process_100ns=" << Step.Before.Process100ns << " before_process_valid=" << Step.Before.ProcessValid
				<< " after_process_100ns=" << Step.After.Process100ns << " after_process_valid=" << Step.After.ProcessValid << '\n';
			for (std::size_t Peer = 0; Peer < 33; ++Peer)
				for (std::size_t Subphase = 0; Subphase < 3; ++Subphase)
					if (Snapshots[Index].Spans[Peer][Subphase].StartNs)
						PrintSpan(Output, Case, Profile, "slow_step", Phase, static_cast<std::uint32_t>(Peer),
							Snapshots[Index].Tick, Subphase, Snapshots[Index].Spans[Peer][Subphase]);
		}
		for (std::size_t Peer = 0; Peer < 33; ++Peer)
			for (std::size_t Subphase = 0; Subphase < 3; ++Subphase)
				if (Maxima[Peer][Subphase].StartNs)
					PrintSpan(Output, Case, Profile, "phase_maximum", Phase, static_cast<std::uint32_t>(Peer),
						MaximumTicks[Peer][Subphase], Subphase, Maxima[Peer][Subphase]);
	}
};

} // namespace gargantuan::test_detail

#pragma once
#include "AckStatsTimingEvidence.hpp"
#include "../src/network/FiniteGrantServiceCurve.hpp"
#include "../src/network/PooledServiceDiagnostics.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace PooledRecoveryEvidence {
using namespace gargantuan::network;
using Feedback = detail::ReliableServiceFeedback;
using Anchor = gargantuan::test_detail::WorkloadClockAnchor;
inline bool Enabled() noexcept {
#if defined(_WIN32)
	char Value[2]{};
	return GetEnvironmentVariableA("GARGANTUAN_SCHEDULER_POOLED_RECOVERY", Value, 2) == 1 && Value[0] == '1';
#else
	return false;
#endif
}
inline bool HealthyControl() noexcept {
#if defined(_WIN32)
	char Value[2]{};
	return GetEnvironmentVariableA("GARGANTUAN_SCHEDULER_POOLED_RECOVERY_HEALTHY", Value, 2) == 1 && Value[0] == '1';
#else
	return false;
#endif
}
inline void PrintAnchor(const Anchor &Value, const char *Boundary) {
	std::cout << "[Qualification:PooledRecoveryClock] boundary=" << Boundary
		<< " pid=" << Value.Process << " native_tid=" << Value.Thread << " native_valid=" << Value.NativeValid
		<< " steady_ns=" << Value.SteadyNs << " qpc_before=" << Value.QpcBefore << " qpc_after=" << Value.QpcAfter
		<< " qpc_frequency=" << Value.QpcFrequency << '\n';
}
struct Certificate {
	ConnectionId Connection;
	Feedback Native;
	Anchor Before, After;
	std::uint64_t Tick = 0;
	bool BridgeValid = false;
};
inline bool ValidCertificate(const Certificate &Value) {
	const auto &S = Value.Native;
	if (!Value.Connection.IsValid() || S.Connection != Value.Connection || !S.CountersValid ||
		!S.StructuralLastCompletedGrantToken || !S.StructuralLastCompletedGrantBytes ||
		S.StructuralLastCompletedGrantBytes > MaximumReliableServiceGroupBytes ||
		!S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
		S.StructuralLastCompletedGrantFirstSendAtMicroseconds < S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
		S.StructuralLastCompletedGrantCompletedAtMicroseconds < S.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
		S.StructuralLastCompletedGrantCompletedAtMicroseconds > S.ObservedAtMicroseconds ||
		!S.LastCompletedStructuralSegmentEventCount ||
		S.LastCompletedStructuralSegmentEventCount > S.LastCompletedStructuralSegmentEvents.size()) return false;
	std::uint64_t Sum = 0, Last = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
	for (std::size_t I = 0; I < S.LastCompletedStructuralSegmentEventCount; ++I) {
		const auto &Event = S.LastCompletedStructuralSegmentEvents[I];
		if (!Event.PayloadBytes || Event.AtMicroseconds < Last ||
			Event.AtMicroseconds > S.StructuralLastCompletedGrantCompletedAtMicroseconds ||
			Event.PayloadBytes > S.StructuralLastCompletedGrantBytes - Sum) return false;
		Sum += Event.PayloadBytes; Last = Event.AtMicroseconds;
	}
	return Sum == S.StructuralLastCompletedGrantBytes &&
		S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds == S.StructuralLastCompletedGrantFirstSendAtMicroseconds &&
		Last == S.StructuralLastCompletedGrantCompletedAtMicroseconds;
}
inline bool FailedCertificate(const Feedback &Value) {
	return Value.StructuralLastCompletedGrantFailed ||
		Value.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds >
		gargantuan::network::FiniteGrantServiceCurve::RunningBoundByteMicroseconds;
}
inline bool FailedCertificate(const Certificate &Value) { return FailedCertificate(Value.Native); }
inline bool WholeGrantInWindow(const Certificate &Value, std::uint64_t Begin, std::uint64_t End) {
	return Value.Native.StructuralLastCompletedGrantActivatedAtMicroseconds >= Begin &&
		Value.Native.StructuralLastCompletedGrantCompletedAtMicroseconds <= End;
}
inline bool CommonFour(const Certificate *Records, std::size_t Count, std::array<std::size_t, 4> &Selected,
	std::uint64_t &Start, std::uint64_t &End) {
	for (std::size_t Candidate = 0; Candidate < Count; ++Candidate) {
		Start = Records[Candidate].Native.StructuralLastCompletedGrantFirstSendAtMicroseconds;
		End = std::numeric_limits<std::uint64_t>::max();
		std::size_t Found = 0;
		for (std::size_t I = 0; I < Count && Found < 4; ++I) {
			const auto &Value = Records[I]; const auto &S = Value.Native;
			if (!ValidCertificate(Value) || S.StructuralLastCompletedGrantFirstSendAtMicroseconds > Start ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds <= Start) continue;
			bool Duplicate = false;
			for (std::size_t J = 0; J < Found; ++J) Duplicate |= Records[Selected[J]].Connection == Value.Connection;
			if (Duplicate) continue;
			Selected[Found++] = I; End = std::min(End, S.StructuralLastCompletedGrantCompletedAtMicroseconds);
		}
		if (Found == 4 && End > Start) return true;
	}
	Start = End = 0; return false;
}
class Recorder;
inline Recorder *Current = nullptr;
class Recorder {
	struct Peer {
		ConnectionId Id;
		std::uint64_t Sequence = 0, NativeObserved = 0;
		Anchor Before, After;
		bool First = false, Failed = false;
	};
	std::array<Peer, 32> Peers{};
	std::array<Certificate, 64> Certificates{};
	std::size_t CertificateCount = 0, FirstFailed = 64;
	bool Open = false, Invalid = false, Registered = false;
	std::uint64_t BeginUs = 0, EndUs = 0, Carryover = 0, Ready = 0;
	std::uint64_t MaximumGrants = 0, Offered = 0, Accepted = 0, Completed = 0, Errors = 0;
	Anchor BeginAnchor, EndAnchor;
	std::atomic<std::uint64_t> BeginQpc{0}, EndQpc{0};
	std::unique_ptr<AckStatsTimingEvidence::ServiceTimingBuffer> Timing =
		std::make_unique<AckStatsTimingEvidence::ServiceTimingBuffer>();
	SteamNetworkingSocketsLib::GargantuanServiceTimingSink TimingSink{this, RecordTiming, true};
	detail::PooledServiceSink *Previous = nullptr;
	detail::PooledServiceSink PooledSink{this, RecordPooled, AcceptPooled};
	struct SuppliedTest {};
	explicit Recorder(SuppliedTest) {}
	Peer *CheckPeer(ConnectionId Id) {
		if (!Id.IsValid() || Id.Slot > Peers.size()) { Invalid = true; return nullptr; }
		auto &Value = Peers[Id.Slot - 1];
		if (Value.Id.IsValid() && Value.Id != Id) { Invalid = true; return nullptr; }
		Value.Id = Id; return &Value;
	}
	static void RecordTiming(void *Context, const SteamNetworkingSocketsLib::GargantuanServiceTimingRecord &Value) noexcept {
		auto &Self = *static_cast<Recorder *>(Context);
		const auto Start = Self.BeginQpc.load(std::memory_order_acquire);
		const auto End = Self.EndQpc.load(std::memory_order_acquire);
		if (Value.Phase != SteamNetworkingSocketsLib::GargantuanServiceTimingPhase::TimerCreate &&
			(!Start || Value.EndQpc < Start || Value.BeginQpc > End)) return;
		AckStatsTimingEvidence::ServiceTimingBuffer::Record(Self.Timing.get(), Value);
	}
	static void AcceptPooled(void *Context, ConnectionId Id, std::uint64_t Token,
		std::uint64_t Bytes, std::uint64_t Activated) noexcept {
		auto &Self = *static_cast<Recorder *>(Context);
		if (Self.Previous && Self.Previous->AcceptedGrant)
			Self.Previous->AcceptedGrant(Self.Previous->Context, Id, Token, Bytes, Activated);
	}
	static void RecordPooled(void *Context, const detail::PooledServiceRecord &Value) noexcept {
		auto &Self = *static_cast<Recorder *>(Context);
		if (Self.Previous && Self.Previous->Record) Self.Previous->Record(Self.Previous->Context, Value);
		if (!Self.Open || !Value.Feedback) return;
		Self.MaximumGrants = std::max(Self.MaximumGrants, Value.Admission.ActiveDrainGrants);
		const auto &S = *Value.Feedback;
		auto *Found = Self.CheckPeer(Value.Connection);
		if (!Found) return;
		auto &Peer = *Found;
		if (S.StructuralCompletedGrantSequence <= Peer.Sequence) return;
		Peer.Sequence = S.StructuralCompletedGrantSequence;
		if (S.StructuralLastCompletedGrantCompletedAtMicroseconds < Self.BeginUs ||
			S.StructuralLastCompletedGrantCompletedAtMicroseconds > Self.EndUs) return;
		const bool Failed = FailedCertificate(S);
		if (Peer.First && (!Failed || Peer.Failed)) return;
		if (Self.CertificateCount == Self.Certificates.size()) { Self.Invalid = true; return; }
		auto &Saved = Self.Certificates[Self.CertificateCount];
		Saved.Connection = Value.Connection; Saved.Native = S; Saved.Tick = Value.SimulationTick;
		Saved.Before = Peer.Before; Saved.After = Peer.After;
		Saved.BridgeValid = Peer.Id == Value.Connection && Peer.NativeObserved == S.ObservedAtMicroseconds &&
			Peer.Before.NativeValid && Peer.After.NativeValid &&
			Peer.Before.SteadyNs / 1000 <= S.ObservedAtMicroseconds && S.ObservedAtMicroseconds <= Peer.After.SteadyNs / 1000;
		Self.Invalid |= !ValidCertificate(Saved) || !Saved.BridgeValid;
		Peer.First = true; Peer.Failed |= Failed;
		if (Failed && Self.FirstFailed == Self.Certificates.size()) Self.FirstFailed = Self.CertificateCount;
		++Self.CertificateCount;
	}
public:
	class Chain {
		Recorder &Owner;
	public:
		explicit Chain(Recorder &Input) : Owner(Input) {
			Owner.Previous = detail::ActivePooledService; detail::ActivePooledService = &Owner.PooledSink;
		}
		~Chain() { detail::ActivePooledService = Owner.Previous; Owner.Previous = nullptr; }
	};
	Recorder() {
		if (!SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(&TimingSink))
			throw std::runtime_error("pooled recovery timing sink already owned");
		Registered = true;
		Current = this;
		AckStatsTimingEvidence::CurrentArm.store(4);
		AckStatsTimingEvidence::ThreadCount.store(0);
		for (auto &Entry : AckStatsTimingEvidence::ServiceStarts) Entry.Ready.store(false);
		SteamNetworkingSockets_SetServiceThreadInitCallback(AckStatsTimingEvidence::ServiceThreadStarted);
	}
	~Recorder() {
		if (!Registered) return;
		// Declared before every transport: their shutdown/join precedes this.
		SteamNetworkingSocketsLib::GargantuanSetServiceTimingSink(nullptr);
		SteamNetworkingSockets_SetServiceThreadInitCallback(nullptr);
		Current = nullptr; Print();
	}
	std::optional<Feedback> Read(const IGameTransport &Transport, ConnectionId Id) {
		if (!Open) return detail::ReliableServiceFeedbackAccess::Observe(Transport, Id);
		const auto Before = Anchor::Capture();
		auto Sample = detail::ReliableServiceFeedbackAccess::Observe(Transport, Id);
		const auto After = Anchor::Capture();
		if (Sample) {
			if (auto *Peer = CheckPeer(Id)) {
				Peer->NativeObserved = Sample->ObservedAtMicroseconds; Peer->Before = Before; Peer->After = After;
			}
		}
		return Sample;
	}
	void Begin(std::uint64_t ReadyPeers, std::uint64_t Pending) {
		Ready = ReadyPeers; Carryover = Pending;
		BeginAnchor = Anchor::Capture(); BeginUs = BeginAnchor.SteadyNs / 1000; EndUs = BeginUs + 500000;
		EndQpc.store(BeginAnchor.QpcAfter + BeginAnchor.QpcFrequency / 2, std::memory_order_release);
		BeginQpc.store(BeginAnchor.QpcBefore, std::memory_order_release); Open = true;
		Invalid |= Ready != 32 || !BeginAnchor.NativeValid;
	}
	void End() { Open = false; EndAnchor = Anchor::Capture(); }
	void Submitted(bool Success) { ++Offered; Accepted += Success; }
	void Resolved(bool Success) { ++Completed; Errors += !Success; }
	bool Failed() const {
		if (Invalid) return true;
		for (std::size_t I = 0; I < CertificateCount; ++I) if (FailedCertificate(Certificates[I])) return true;
		return false;
	}
	void Print() const {
		PrintAnchor(BeginAnchor, "BEGIN");
		PrintAnchor(EndAnchor, "END");
		std::array<std::size_t, 4> Selected{}; std::uint64_t Start = 0, End = 0;
		const bool Four = CommonFour(Certificates.data(), CertificateCount, Selected, Start, End);
		for (std::size_t I = 0; I < CertificateCount; ++I) {
			const auto &Value = Certificates[I]; const auto &S = Value.Native;
			std::cout << "[Qualification:PooledRecoveryGrant] index=" << I << " connection=" << Value.Connection.Slot << ':'
				<< Value.Connection.Generation << " token=" << S.StructuralLastCompletedGrantToken
				<< " bytes=" << S.StructuralLastCompletedGrantBytes << " activated_us=" << S.StructuralLastCompletedGrantActivatedAtMicroseconds
				<< " first_us=" << S.StructuralLastCompletedGrantFirstSendAtMicroseconds
				<< " complete_us=" << S.StructuralLastCompletedGrantCompletedAtMicroseconds
				<< " running_deficit=" << S.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
				<< " failed=" << S.StructuralLastCompletedGrantFailed << " segments=" << S.LastCompletedStructuralSegmentEventCount
				<< " observed_us=" << S.ObservedAtMicroseconds << " tick=" << Value.Tick << " bridge_valid=" << Value.BridgeValid
				<< " before_steady_ns=" << Value.Before.SteadyNs << " after_steady_ns=" << Value.After.SteadyNs
				<< " qpc_before=" << Value.Before.QpcBefore << " qpc_after=" << Value.After.QpcAfter
				<< " qpc_frequency=" << Value.Before.QpcFrequency
				<< " whole_grant_in_window=" << WholeGrantInWindow(Value, BeginUs, EndUs) << '\n';
			const bool Timeline = I == FirstFailed || (Four && std::find(Selected.begin(), Selected.end(), I) != Selected.end());
			if (Timeline) for (std::size_t J = 0; J < std::min<std::size_t>(S.LastCompletedStructuralSegmentEventCount,
				S.LastCompletedStructuralSegmentEvents.size()); ++J)
				std::cout << "[Qualification:PooledRecoverySegment] index=" << I << " token=" << S.StructuralLastCompletedGrantToken
					<< " sequence=" << J << " at_us=" << S.LastCompletedStructuralSegmentEvents[J].AtMicroseconds
					<< " bytes=" << S.LastCompletedStructuralSegmentEvents[J].PayloadBytes << '\n';
		}
		const auto Threads = AckStatsTimingEvidence::ThreadCount.load(std::memory_order_acquire);
		for (unsigned I = 0; I < Threads && I < AckStatsTimingEvidence::ServiceStarts.size(); ++I) {
			const auto &Entry = AckStatsTimingEvidence::ServiceStarts[I];
			if (Entry.Ready.load(std::memory_order_acquire)) PrintAnchor(Entry.Clock, "SERVICE_THREAD");
		}
		const auto Count = Timing->Count.load(std::memory_order_acquire);
		for (std::size_t I = 0; I < std::min<std::uint64_t>(Count, Timing->Records.size()); ++I) {
			const auto &Value = Timing->Records[I];
			std::cout << "[Qualification:PooledRecoveryServiceTiming] sequence=" << I << " phase=" << static_cast<unsigned>(Value.Phase)
				<< " native_tid=" << Value.ThreadId << " clock_valid=" << Value.ClockValid << " qpc_begin=" << Value.BeginQpc
				<< " qpc_end=" << Value.EndQpc << " qpc_frequency=" << BeginAnchor.QpcFrequency << " count=" << Value.Count
				<< " bytes=" << Value.Bytes << " detail=" << Value.Detail << " result=" << Value.Result << " error=" << Value.Error
				<< " flags=" << Value.Flags << '\n';
		}
		std::cout << "[Qualification:PooledRecoverySummary] window_us=500000 ready=" << Ready << " carryover_pending=" << Carryover
			<< " offered=" << Offered << " accepted=" << Accepted << " completed=" << Completed << " errors=" << Errors
			<< " rejected=" << Offered - Accepted << " unresolved=" << Accepted - Completed
			<< " healthy_input_covered=" << (Carryover == 0 && Offered == 32 && Accepted == 32 && Completed == 32 && Errors == 0)
			<< " certificates=" << CertificateCount << " first_failed_observed=" << (FirstFailed == 64 ? -1 : static_cast<int>(FirstFailed))
			<< " common_four=" << Four << " overlap_begin_us=" << Start << " overlap_end_us=" << End
			<< " active_grants_high=" << MaximumGrants << " invalid=" << Invalid << " diagnostic_failed=" << Failed() << " timing_records=" << Count
			<< " timing_overflow=" << Timing->Overflow.load() << " provider_acceptance=NOT_CLAIMED\n";
	}
	static bool TestSupplied() {
		std::array<Certificate, 4> Values{};
		for (std::size_t I = 0; I < Values.size(); ++I) {
			auto &Value = Values[I]; auto &S = Value.Native;
			Value.Connection = S.Connection = {static_cast<std::uint32_t>(I + 1), 1};
			S.StructuralLastCompletedGrantToken = I + 1; S.StructuralLastCompletedGrantBytes = 1258;
			S.StructuralLastCompletedGrantActivatedAtMicroseconds = 1000 + I * 10;
			S.StructuralLastCompletedGrantFirstSendAtMicroseconds = 1100 + I * 10;
			S.StructuralLastCompletedGrantCompletedAtMicroseconds = 1500 + I * 10; S.ObservedAtMicroseconds = 2000;
			S.LastCompletedStructuralSegmentEventCount = 2;
			S.LastCompletedStructuralSegmentEvents[0] = {S.StructuralLastCompletedGrantFirstSendAtMicroseconds, 1135};
			S.LastCompletedStructuralSegmentEvents[1] = {S.StructuralLastCompletedGrantCompletedAtMicroseconds, 123};
		}
		std::array<std::size_t, 4> Selected{}; std::uint64_t Start = 0, End = 0;
		bool Good = ValidCertificate(Values[0]) && CommonFour(Values.data(), Values.size(), Selected, Start, End) && End > Start;
		auto Bad = Values[0]; Bad.Native.Connection.Generation = 2;
		Good &= !ValidCertificate(Bad);
		Bad = Values[0]; Bad.Native.LastCompletedStructuralSegmentEventCount = 513;
		Good &= !ValidCertificate(Bad);
		Bad = Values[0]; Bad.Native.StructuralLastCompletedGrantFirstSendAtMicroseconds = 999;
		Good &= !ValidCertificate(Bad) && !WholeGrantInWindow(Values[0], 1100, 2000);
		Bad = Values[0]; Bad.Native.StructuralMaximumDeficitByteMicroseconds = std::numeric_limits<std::uint64_t>::max();
		Good &= !FailedCertificate(Bad); // Never attach historical global max to this healthy token.
		Bad.Native.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds = FiniteGrantServiceCurve::RunningBoundByteMicroseconds + 1;
		Good &= FailedCertificate(Bad);
		unsigned Records = 0, Accepts = 0;
		struct Counts { unsigned &Records, &Accepts; } Count{Records, Accepts};
		detail::PooledServiceSink Previous{&Count,
			[](void *Context, const detail::PooledServiceRecord &) noexcept { ++static_cast<Counts *>(Context)->Records; },
			[](void *Context, ConnectionId, std::uint64_t, std::uint64_t, std::uint64_t) noexcept { ++static_cast<Counts *>(Context)->Accepts; }};
		auto *Original = detail::ActivePooledService; detail::ActivePooledService = &Previous;
		auto Test = std::unique_ptr<Recorder>(new Recorder(SuppliedTest{}));
		{
			Chain Forward(*Test);
			detail::ActivePooledService->Record(detail::ActivePooledService->Context, {});
			detail::ActivePooledService->AcceptedGrant(detail::ActivePooledService->Context, {1, 1}, 1, 1258, 1000);
		}
		Good &= Records == 1 && Accepts == 1 && detail::ActivePooledService == &Previous;
		detail::ActivePooledService = Original;
		Good &= Test->CheckPeer({1, 1}) != nullptr && Test->CheckPeer({1, 2}) == nullptr && Test->Invalid;
		std::cout << "[Qualification:PooledRecoverySupplied] cases=7 passed=" << Good << '\n'; return Good;
	}
};
}

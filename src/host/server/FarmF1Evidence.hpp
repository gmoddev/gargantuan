#pragma once

#include "../../network/PooledServiceDiagnostics.hpp"
#include "../../network/FiniteGrantServiceCurve.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>

namespace gargantuan::host::detail {
// Farm-only Main-thread retention. Native F1 remains grant scoped. Remembering
// that an earlier finite grant failed does not accrue service across grants.
class FarmF1Evidence final {
public:
	enum class Contract { FiniteGrantV1, HybridF1V1 };
	struct FailedGrant {
		// One immutable diagnostic witness per peer. Invalid/missing timelines
		// cannot become measured certificates through later requalification.
		bool Observed = false, Measured = false;
		std::uint64_t Sequence = 0, Token = 0, Bytes = 0, Activated = 0, First = 0, Completed = 0;
		std::uint64_t ObservedAt = 0, MaximumRunning = 0;
		std::uint32_t SegmentCount = 0;
		network::detail::InitialServiceTrace InitialTrace{};
		bool InitialTraceValid = false;
		std::array<network::detail::ReliableServiceFeedback::StructuralSegmentEvent, 512> Segments{};
	};
	struct InitialFailure {
		// Independent of the first general/running failure. Keep only a validated
		// zero-first-send witness; malformed feedback cannot consume this slot.
		bool Measured = false, InitialTraceValid = false;
		std::uint64_t Sequence = 0, Token = 0, Bytes = 0, Activated = 0, First = 0, Completed = 0, ObservedAt = 0;
		network::detail::InitialServiceTrace InitialTrace{};
	};
	struct Peer {
		network::ConnectionId Connection{};
		std::uint64_t Samples = 0, Completed = 0, Qualified = 0, CompletedBytes = 0;
		std::uint64_t Accepted = 0, FirstSent = 0, Acked = 0, Retired = 0;
		std::uint64_t QualifiedAccepted = 0, QualifiedBytes = 0, BootstrapAccepted = 0, BootstrapBytes = 0;
		std::uint64_t BootstrapRetired = 0, LastAcceptedToken = 0;
		std::uint64_t PendingToken = 0, PendingBytes = 0, PendingActivated = 0;
		bool PendingCompleted = false;
		std::uint64_t LastToken = 0, LastBytes = 0, LastActivated = 0, LastFirst = 0, LastCompleted = 0;
		std::uint64_t MaximumRunning = 0, MaximumFiniteShortfall = 0;
		bool Failed = false, AccountingInvalid = false;
		bool CompletionFailed = false, LastCompletionFailed = false, CompletionEvidenceInvalid = false, ExecutionFailed = false;
		bool MainOpportunityFailed = false, TurnoverFailed = false;
		bool QueuedFailed = false;
		std::uint64_t MaximumMainOpportunity = 0, MaximumTurnover = 0;
		std::uint64_t CompletedTurnovers = 0, CompletedAdmissionOpportunities = 0;
		std::uint64_t LastTurnoverToken = 0, LastTurnoverActivated = 0, LastTurnoverAt = 0;
		bool Hybrid = false;
		std::uint64_t OrdinaryRegistered = 0, OrdinaryCompleted = 0, OrdinaryPending = 0, OrdinaryMaximumLatency = 0;
		bool OrdinaryEverFailed = false, OrdinaryLastCompletedFailed = false;
		bool OrdinaryActiveDeadlineFailed = false, OrdinaryEvidenceInvalid = false;
		std::uint64_t OrdinaryFailureSequence = 0, OrdinaryFailurePhase = 0, OrdinaryFailureMessageNumber = 0;
		std::uint64_t OrdinaryFailureAccepted = 0, OrdinaryFailureCompleted = 0;
		std::uint32_t OrdinaryFailureExpectedBytes = 0, OrdinaryFailureFirstSentBytes = 0;
		FailedGrant FirstFailed;
		InitialFailure FirstInitialFailure;
	};
private:
	const Contract ServiceContract;
	std::array<Peer, 32> Peers{};
	std::size_t Count = 0;
	bool Invalid = false;
	bool FailureNotified = false;
	bool FailureInitialOnly = false;
	void *FailureContext = nullptr;
	void (*FailureObserver)(void *, std::uint64_t) noexcept = nullptr;
	network::detail::PooledServiceSink Sink{this, Record, AcceptedGrant};
	network::detail::PooledServiceSink *Previous = nullptr;
	static void Record(void *Context, const network::detail::PooledServiceRecord &Value) noexcept {
		static_cast<FarmF1Evidence *>(Context)->Observe(Value);
	}
	static void AcceptedGrant(void *Context, network::ConnectionId Connection, std::uint64_t Token,
		std::uint64_t Bytes, std::uint64_t Activated) noexcept {
		static_cast<FarmF1Evidence *>(Context)->Accept(Connection, Token, Bytes, Activated);
	}
	Peer *Find(network::ConnectionId Connection) noexcept {
		if (!Connection.IsValid()) { Invalid = true; return nullptr; }
		for (std::size_t I = 0; I < Count; ++I) if (Peers[I].Connection == Connection) return &Peers[I];
		if (Count == Peers.size()) { Invalid = true; return nullptr; }
		auto *P = &Peers[Count++]; P->Connection = Connection; return P;
	}
	static bool ValidInitialTrace(const network::detail::InitialServiceTrace &T,
		std::uint64_t Token, std::uint64_t Activated, std::uint64_t First) noexcept {
		return T.Enabled && T.Token == Token && T.MessageNumber && T.NativeHandle && T.DispatchSequence &&
			T.TransportEntered >= Activated && T.AdapterLocked >= T.TransportEntered &&
			T.NativeApiEntered >= T.AdapterLocked && T.NativeApiLocked >= T.NativeApiEntered &&
			T.NativeEnqueued >= T.NativeApiLocked && T.FirstSnpVisit >= T.NativeEnqueued &&
			T.PacketPreparationEntered >= T.FirstSnpVisit && T.WorkerQueued >= T.PacketPreparationEntered &&
			T.WorkerStarted >= T.WorkerQueued && T.NativeCompleted >= T.WorkerStarted &&
			T.NativeCompleted == First && T.ActorReconciled >= T.NativeCompleted;
	}
	static bool ValidCompletedSegments(const network::detail::ReliableServiceFeedback &S) noexcept {
		const auto Count = S.LastCompletedStructuralSegmentEventCount;
		const auto Bytes = S.StructuralLastCompletedGrantBytes;
		if (!Count || Count > S.LastCompletedStructuralSegmentEvents.size()) return false;
		std::uint64_t Sum = 0, Last = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
		for (std::size_t I = 0; I < Count; ++I) {
			const auto &Event = S.LastCompletedStructuralSegmentEvents[I];
			if (!Event.PayloadBytes || Event.PayloadBytes > Bytes - Sum || Event.AtMicroseconds < Last ||
				Event.AtMicroseconds > S.StructuralLastCompletedGrantCompletedAtMicroseconds) return false;
			Sum += Event.PayloadBytes; Last = Event.AtMicroseconds;
		}
		return Sum == Bytes && S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds ==
			S.StructuralLastCompletedGrantFirstSendAtMicroseconds && Last == S.StructuralLastCompletedGrantCompletedAtMicroseconds;
	}
	static bool HybridCompletionPass(const network::detail::ReliableServiceFeedback &S) noexcept {
		using Curve = network::FiniteGrantServiceCurve;
		const auto Activated = S.StructuralLastCompletedGrantActivatedAtMicroseconds;
		const auto First = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
		const auto Completed = S.StructuralLastCompletedGrantCompletedAtMicroseconds;
		if (!Activated || First < Activated || Completed < First) return false;
		const auto Duration = Completed - Activated;
		return Duration <= (Curve::FiniteInterceptByteMicroseconds + Curve::MaximumGrantBytes * 1'000'000) /
			Curve::PeerRateBytesPerSecond;
	}
	static void RetainInitialFailure(Peer &P, const network::detail::ReliableServiceFeedback &S,
		bool CompletionValid) noexcept {
		if (P.FirstInitialFailure.Measured || !CompletionValid || !S.CountersValid || S.Connection != P.Connection ||
			S.StructuralCompletedGrantSequence <= P.Completed || S.StructuralCompletedGrantSequence - P.Completed != 1) return;
		const auto Activated = S.StructuralLastCompletedGrantActivatedAtMicroseconds;
		const auto First = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
		if (!Activated || Activated == std::numeric_limits<std::uint64_t>::max() || First < Activated) return;
		// Immediately before the first native-positive event D_g is exactly zero.
		// F1 requires positive service iff R_i * elapsed exceeds its intercept.
		// W_g/R_i is a drain allowance, never extra zero-first-send allowance.
		if (!network::FiniteGrantServiceCurve::ZeroFirstSendBreach(Activated, First)) return;
		const auto Bytes = S.StructuralLastCompletedGrantBytes;
		const auto Completed = S.StructuralLastCompletedGrantCompletedAtMicroseconds;
		const auto Count = S.LastCompletedStructuralSegmentEventCount;
		if (!Count || Count > S.LastCompletedStructuralSegmentEvents.size()) return;
		std::uint64_t Sum = 0, Last = First;
		for (std::size_t I = 0; I < Count; ++I) {
			const auto &Event = S.LastCompletedStructuralSegmentEvents[I];
			if (!Event.PayloadBytes || Event.PayloadBytes > Bytes - Sum ||
				Event.AtMicroseconds < Last || Event.AtMicroseconds > Completed) return;
			Sum += Event.PayloadBytes; Last = Event.AtMicroseconds;
		}
		if (Sum != Bytes || S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds != First || Last != Completed) return;
		auto &W = P.FirstInitialFailure;
		W.Measured = true; W.Sequence = S.StructuralCompletedGrantSequence;
		W.Token = S.StructuralLastCompletedGrantToken; W.Bytes = Bytes; W.Activated = Activated;
		W.First = First; W.Completed = Completed; W.ObservedAt = S.ObservedAtMicroseconds;
		W.InitialTrace = S.LastCompletedInitialTrace;
		W.InitialTraceValid = ValidInitialTrace(W.InitialTrace, W.Token, W.Activated, W.First);
	}
	static void RetainFailure(Peer &P, const network::detail::ReliableServiceFeedback &S,
		bool CompletionValid) noexcept {
		if (P.FirstFailed.Observed || (!S.StructuralLastCompletedGrantFailed &&
			S.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds <=
				network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds)) return;
		auto &W = P.FirstFailed;
		W.Observed = true;
		W.Sequence = S.StructuralCompletedGrantSequence; W.Token = S.StructuralLastCompletedGrantToken;
		W.Bytes = S.StructuralLastCompletedGrantBytes;
		W.Activated = S.StructuralLastCompletedGrantActivatedAtMicroseconds;
		W.First = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
		W.Completed = S.StructuralLastCompletedGrantCompletedAtMicroseconds;
		W.ObservedAt = S.ObservedAtMicroseconds;
		W.MaximumRunning = S.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds;
		W.SegmentCount = S.LastCompletedStructuralSegmentEventCount;
		if (!CompletionValid || !S.CountersValid || S.Connection != P.Connection ||
			S.StructuralCompletedGrantSequence <= P.Completed ||
			S.StructuralCompletedGrantSequence - P.Completed != 1 ||
			!W.SegmentCount || W.SegmentCount > W.Segments.size()) return;
		std::uint64_t Sum = 0, Last = W.First;
		for (std::size_t I = 0; I < W.SegmentCount; ++I) {
			const auto &Event = S.LastCompletedStructuralSegmentEvents[I];
			if (!Event.PayloadBytes || Event.PayloadBytes > W.Bytes - Sum ||
				Event.AtMicroseconds < Last || Event.AtMicroseconds > W.Completed) return;
			Sum += Event.PayloadBytes; Last = Event.AtMicroseconds;
		}
		if (Sum != W.Bytes || S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds != W.First ||
			Last != W.Completed) return;
		W.Segments = S.LastCompletedStructuralSegmentEvents;
		W.InitialTrace = S.LastCompletedInitialTrace;
		W.InitialTraceValid = ValidInitialTrace(W.InitialTrace, W.Token, W.Activated, W.First);
		W.Measured = true;
	}
public:
	explicit FarmF1Evidence(Contract Value = Contract::FiniteGrantV1) noexcept :
		ServiceContract(Value), Previous(network::detail::ActivePooledService) {
		network::detail::ActivePooledService = &Sink;
	}
	~FarmF1Evidence() { network::detail::ActivePooledService = Previous; }
	FarmF1Evidence(const FarmF1Evidence &) = delete;
	FarmF1Evidence &operator=(const FarmF1Evidence &) = delete;
	// Farm-only Main-thread hook. The existing native observation timestamp
	// fences opt-in timing retention. InitialOnly selects an exact zero-first-send
	// breach; ordinary diagnostic mode keeps its first general failure behavior.
	void SetFailureObserver(void *Context, void (*Observer)(void *, std::uint64_t) noexcept,
		bool InitialOnly = false) noexcept {
		FailureContext = Context; FailureObserver = Observer; FailureInitialOnly = InitialOnly;
	}
	void Accept(network::ConnectionId Connection, std::uint64_t Token, std::uint64_t Bytes,
		std::uint64_t Activated) noexcept {
		auto *P = Find(Connection); if (!P) return;
		if (!Token || Token <= P->LastAcceptedToken || !Bytes || Bytes > network::MaximumReliableServiceGroupBytes ||
			!Activated || P->PendingToken || Bytes > std::numeric_limits<std::uint64_t>::max() - P->QualifiedBytes - P->BootstrapBytes) {
			P->Failed = P->AccountingInvalid = true; return;
		}
		P->LastAcceptedToken = P->PendingToken = Token; P->PendingBytes = Bytes;
		P->PendingActivated = Activated; P->PendingCompleted = false;
		if (Activated == std::numeric_limits<std::uint64_t>::max()) {
			// Bootstrap cannot resume after a qualified Ready grant.
			if (P->QualifiedAccepted) P->Failed = P->AccountingInvalid = true;
			++P->BootstrapAccepted; P->BootstrapBytes += Bytes;
		} else { ++P->QualifiedAccepted; P->QualifiedBytes += Bytes; }
	}
	void Observe(const network::detail::PooledServiceRecord &Value) noexcept {
		auto *P = Find(Value.Connection); if (!P) return;
		const bool RecordValid = Value.Result.Valid && Value.Accepted.Structural >= P->Accepted &&
			Value.Accepted.Structural == P->QualifiedBytes + P->BootstrapBytes;
		if (!RecordValid)
			P->Failed = P->AccountingInvalid = true;
		P->Accepted = Value.Accepted.Structural;
		if (ServiceContract == Contract::HybridF1V1) {
			P->Hybrid = true;
			P->QueuedFailed |= Value.Result.QueuedOrdinaryEverFailed || Value.Result.QueuedQualifiedOrdinaryEverFailed;
			P->Failed |= P->QueuedFailed;
			if (Value.Result.CompletedTurnovers < P->CompletedTurnovers ||
				Value.Result.CompletedAdmissionOpportunities < P->CompletedAdmissionOpportunities)
				P->Failed = P->AccountingInvalid = true;
			P->CompletedTurnovers = Value.Result.CompletedTurnovers;
			P->CompletedAdmissionOpportunities = Value.Result.CompletedAdmissionOpportunities;
			P->LastTurnoverToken = Value.Result.LastCompletedTurnoverToken;
			P->LastTurnoverActivated = Value.Result.LastCompletedTurnoverActivatedAtMicroseconds;
			P->LastTurnoverAt = Value.Result.LastCompletedTurnoverAtMicroseconds;
			P->MaximumMainOpportunity = std::max(P->MaximumMainOpportunity, Value.Result.MaximumMainOpportunityMicroseconds);
			P->MaximumTurnover = std::max(P->MaximumTurnover, Value.Result.MaximumTurnoverMicroseconds);
			P->MainOpportunityFailed |= Value.Result.MainOpportunityFailed ||
				P->MaximumMainOpportunity > network::PooledReliableServiceProfile::AdmissionOpportunityMicroseconds;
			P->TurnoverFailed |= Value.Result.TurnoverFailed ||
				P->MaximumTurnover > network::PooledReliableServiceProfile::StructuralTurnoverMicroseconds;
			P->ExecutionFailed |= Value.Result.ExecutionFailed || P->MainOpportunityFailed || P->TurnoverFailed;
			P->Failed |= P->ExecutionFailed;
		}
		if (!Value.Feedback) return; // Missing feedback cannot create completed evidence.
		const auto &S = *Value.Feedback;
		if (ServiceContract == Contract::HybridF1V1) {
			if (S.QualifiedOrdinaryRegisteredMessages < P->OrdinaryRegistered ||
				S.QualifiedOrdinaryCompletedMessages < P->OrdinaryCompleted ||
				S.QualifiedOrdinaryCompletedMessages > S.QualifiedOrdinaryRegisteredMessages ||
				S.QualifiedOrdinaryPendingMessages != S.QualifiedOrdinaryRegisteredMessages - S.QualifiedOrdinaryCompletedMessages ||
				S.QualifiedOrdinaryMaximumLatencyMicroseconds < P->OrdinaryMaximumLatency)
				P->Failed = P->AccountingInvalid = true;
			P->OrdinaryRegistered = S.QualifiedOrdinaryRegisteredMessages;
			P->OrdinaryCompleted = S.QualifiedOrdinaryCompletedMessages;
			P->OrdinaryPending = S.QualifiedOrdinaryPendingMessages;
			P->OrdinaryMaximumLatency = std::max(P->OrdinaryMaximumLatency, S.QualifiedOrdinaryMaximumLatencyMicroseconds);
			P->OrdinaryEverFailed |= S.QualifiedOrdinaryEverFailed;
			P->OrdinaryLastCompletedFailed = S.QualifiedOrdinaryLastCompletedFailed;
			P->OrdinaryActiveDeadlineFailed |= S.QualifiedOrdinaryActiveDeadlineFailed;
			P->OrdinaryEvidenceInvalid |= S.QualifiedOrdinaryEvidenceInvalid;
			if (!S.QualifiedOrdinaryFailureSequence && (S.QualifiedOrdinaryFailurePhase ||
				S.QualifiedOrdinaryFailureMessageNumber || S.QualifiedOrdinaryFailureAcceptedAtMicroseconds ||
				S.QualifiedOrdinaryFailureCompletedAtMicroseconds || S.QualifiedOrdinaryFailureExpectedBytes ||
				S.QualifiedOrdinaryFailureFirstSentBytes)) P->OrdinaryEvidenceInvalid = true;
			if (!P->OrdinaryFailureSequence && S.QualifiedOrdinaryFailureSequence) {
				P->OrdinaryFailureSequence = S.QualifiedOrdinaryFailureSequence;
				P->OrdinaryFailurePhase = S.QualifiedOrdinaryFailurePhase;
				P->OrdinaryFailureMessageNumber = S.QualifiedOrdinaryFailureMessageNumber;
				P->OrdinaryFailureAccepted = S.QualifiedOrdinaryFailureAcceptedAtMicroseconds;
				P->OrdinaryFailureCompleted = S.QualifiedOrdinaryFailureCompletedAtMicroseconds;
				P->OrdinaryFailureExpectedBytes = S.QualifiedOrdinaryFailureExpectedBytes;
				P->OrdinaryFailureFirstSentBytes = S.QualifiedOrdinaryFailureFirstSentBytes;
			}
			P->Failed |= P->OrdinaryEverFailed || P->OrdinaryLastCompletedFailed || P->OrdinaryActiveDeadlineFailed ||
				P->OrdinaryEvidenceInvalid || P->OrdinaryMaximumLatency > 50'000 || P->OrdinaryFailureSequence != 0;
		}
		++P->Samples;
		const bool FeedbackValid = !(!S.CountersValid || S.Connection != Value.Connection ||
			S.StructuralPayloadBytesFirstSent < P->FirstSent || S.StructuralPayloadBytesAcked < P->Acked ||
			S.StructuralPayloadBytesAcked > S.StructuralPayloadBytesFirstSent ||
			S.StructuralPayloadBytesFirstSent > P->Accepted ||
			S.StructuralCompletedGrantSequence < P->Completed ||
			S.StructuralCompletedGrantSequence - P->Completed > 1 ||
			Value.Result.RetiredBytes > std::numeric_limits<std::uint64_t>::max() - P->Retired);
		if (!FeedbackValid)
			P->Failed = P->AccountingInvalid = true;
		P->Retired += Value.Result.RetiredBytes;
		P->FirstSent = S.StructuralPayloadBytesFirstSent;
		P->Acked = S.StructuralPayloadBytesAcked;
		P->MaximumRunning = std::max(P->MaximumRunning, S.StructuralMaximumDeficitByteMicroseconds);
		P->MaximumFiniteShortfall = std::max(P->MaximumFiniteShortfall,
			S.StructuralMaximumFiniteShortfallByteMicroseconds);
		const bool Hybrid = ServiceContract == Contract::HybridF1V1;
		if (Hybrid) {
			P->CompletionFailed |= S.StructuralCompletionServiceFailed;
			P->CompletionEvidenceInvalid |= S.StructuralCompletionEvidenceInvalid;
			P->Failed |= P->CompletionFailed || P->CompletionEvidenceInvalid ||
				(Value.Result.Available && !Value.Result.Qualified);
		} else {
			P->Failed |= S.StructuralServiceFailed || S.StructuralLastCompletedGrantFailed ||
				(Value.Result.Available && !Value.Result.Qualified) || P->MaximumFiniteShortfall != 0 ||
				P->MaximumRunning > network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds;
		}
		if (S.StructuralCompletedGrantSequence != P->Completed) {
			const auto Bytes = S.StructuralLastCompletedGrantBytes;
			const bool CompletionValid = !(!P->PendingToken || P->PendingCompleted || P->PendingToken != S.StructuralLastCompletedGrantToken ||
				P->PendingBytes != Bytes || P->PendingActivated != S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				P->PendingActivated == std::numeric_limits<std::uint64_t>::max() ||
				!Bytes || Bytes > network::MaximumReliableServiceGroupBytes ||
				S.StructuralLastCompletedGrantToken <= P->LastToken ||
				!S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				S.StructuralLastCompletedGrantFirstSendAtMicroseconds < S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds < S.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds > S.ObservedAtMicroseconds ||
				Bytes > std::numeric_limits<std::uint64_t>::max() - P->CompletedBytes);
			if (!CompletionValid) P->Failed = P->AccountingInvalid = true;
			if (Hybrid) {
				P->LastCompletionFailed = S.StructuralLastCompletedCompletionFailed || !HybridCompletionPass(S);
				P->CompletionFailed |= P->LastCompletionFailed;
				P->CompletionEvidenceInvalid |= !CompletionValid || !ValidCompletedSegments(S);
				P->Failed |= P->CompletionFailed || P->CompletionEvidenceInvalid;
			}
			const bool RetirementValid = !Value.Result.RetiredBytes ||
				(P->PendingToken && Value.Result.RetiredToken == P->PendingToken &&
					Value.Result.RetiredBytes == P->PendingBytes &&
					P->PendingActivated != std::numeric_limits<std::uint64_t>::max() &&
					(P->PendingCompleted || CompletionValid));
			// Validate this record, not the historical AccountingInvalid latch.
			// An unrelated earlier accounting error cannot erase exact native data.
			RetainFailure(*P, S, CompletionValid && RecordValid && FeedbackValid && RetirementValid);
			RetainInitialFailure(*P, S, CompletionValid && RecordValid && FeedbackValid && RetirementValid);
			if (!FailureNotified && (FailureInitialOnly ? P->FirstInitialFailure.Measured : P->FirstFailed.Measured)) {
				FailureNotified = true;
				if (FailureObserver) FailureObserver(FailureContext,
					FailureInitialOnly ? P->FirstInitialFailure.ObservedAt : P->FirstFailed.ObservedAt);
			}
			P->Completed = S.StructuralCompletedGrantSequence;
			P->CompletedBytes += Bytes;
			if (Hybrid ? (!P->LastCompletionFailed && !P->CompletionEvidenceInvalid && CompletionValid &&
				RecordValid && FeedbackValid && S.CountersValid) : (!S.StructuralLastCompletedGrantFailed && S.CountersValid)) ++P->Qualified;
			P->LastToken = S.StructuralLastCompletedGrantToken; P->LastBytes = Bytes;
			P->LastActivated = S.StructuralLastCompletedGrantActivatedAtMicroseconds;
			P->LastFirst = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
			P->LastCompleted = S.StructuralLastCompletedGrantCompletedAtMicroseconds;
			P->PendingCompleted = true;
		}
		if (Value.Result.RetiredBytes) {
			if (!P->PendingToken || Value.Result.RetiredToken != P->PendingToken || Value.Result.RetiredBytes != P->PendingBytes)
				P->Failed = P->AccountingInvalid = true;
			if (P->PendingActivated == std::numeric_limits<std::uint64_t>::max()) {
				if (P->PendingCompleted) P->Failed = P->AccountingInvalid = true;
				P->BootstrapRetired += Value.Result.RetiredBytes;
			} else if (!P->PendingCompleted) P->Failed = P->AccountingInvalid = true;
			P->PendingToken = P->PendingBytes = P->PendingActivated = 0; P->PendingCompleted = false;
		}
	}
	static bool Complete(const Peer &P) noexcept {
		return !P.Failed && P.Samples && P.Completed && P.Completed == P.Qualified &&
			(!P.Hybrid || (P.CompletedAdmissionOpportunities && P.CompletedTurnovers == P.QualifiedAccepted &&
				P.LastTurnoverToken == P.LastToken && P.LastTurnoverActivated == P.LastActivated &&
				P.LastTurnoverAt >= P.LastCompleted && P.LastTurnoverAt - P.LastTurnoverActivated <=
					network::PooledReliableServiceProfile::StructuralTurnoverMicroseconds &&
				P.MaximumTurnover >= P.LastTurnoverAt - P.LastTurnoverActivated)) &&
			(!P.Hybrid || (P.OrdinaryRegistered && P.OrdinaryCompleted == P.OrdinaryRegistered && !P.OrdinaryPending)) &&
			!P.PendingToken && P.Completed == P.QualifiedAccepted && P.CompletedBytes == P.QualifiedBytes &&
			P.BootstrapBytes == P.BootstrapRetired && P.CompletedBytes + P.BootstrapBytes == P.Accepted && P.Accepted == P.FirstSent &&
			P.FirstSent == P.Acked && P.Acked == P.Retired;
	}
	bool Valid(std::size_t Expected = 32) const noexcept {
		if (Invalid || Count != Expected) return false;
		for (std::size_t I = 0; I < Count; ++I) if (!Complete(Peers[I])) return false;
		return true;
	}
	std::span<const Peer> Observations() const noexcept { return {Peers.data(), Count}; }
	void Write(std::ostream &Out, std::string_view Run) const {
		for (std::size_t I = 0; I < Count; ++I) {
			const auto &P = Peers[I];
			Out << "[Qualification:FarmF1] event=peer contract="
				<< (ServiceContract == Contract::HybridF1V1 ? "HYBRID_F1_V1" : "finite_grant_v1") << " run=" << Run
				<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
				<< " samples=" << P.Samples << " completed=" << P.Completed << " qualified=" << P.Qualified
				<< " completed_bytes=" << P.CompletedBytes << " accepted=" << P.Accepted
				<< " qualified_accepted=" << P.QualifiedAccepted << " qualified_bytes=" << P.QualifiedBytes
				<< " bootstrap_accepted=" << P.BootstrapAccepted << " bootstrap_bytes=" << P.BootstrapBytes
				<< " bootstrap_retired=" << P.BootstrapRetired << " pending_token=" << P.PendingToken
				<< " first_sent=" << P.FirstSent << " acked=" << P.Acked << " retired=" << P.Retired
				<< " last_token=" << P.LastToken << " last_bytes=" << P.LastBytes
				<< " last_activated_us=" << P.LastActivated << " last_first_us=" << P.LastFirst
				<< " last_completed_us=" << P.LastCompleted << " max_running_byte_us=" << P.MaximumRunning
				<< " max_finite_shortfall_byte_us=" << P.MaximumFiniteShortfall
				<< " failed=" << P.Failed << " valid=" << (!Invalid && Complete(P));
			if (ServiceContract == Contract::HybridF1V1)
				Out << " completion_failed=" << P.CompletionFailed << " last_completion_failed=" << P.LastCompletionFailed
					<< " completion_evidence_invalid=" << P.CompletionEvidenceInvalid << " execution_failed=" << P.ExecutionFailed
					<< " main_opportunity_failed=" << P.MainOpportunityFailed << " turnover_failed=" << P.TurnoverFailed
					<< " queued_failed=" << P.QueuedFailed
					<< " max_main_opportunity_us=" << P.MaximumMainOpportunity << " max_turnover_us=" << P.MaximumTurnover
					<< " execution_opportunities=" << P.CompletedAdmissionOpportunities << " completed_turnovers=" << P.CompletedTurnovers
					<< " last_turnover_token=" << P.LastTurnoverToken << " last_turnover_activated_us=" << P.LastTurnoverActivated
					<< " last_turnover_us=" << P.LastTurnoverAt
					<< " ordinary_registered=" << P.OrdinaryRegistered << " ordinary_completed=" << P.OrdinaryCompleted
					<< " ordinary_pending=" << P.OrdinaryPending << " ordinary_max_latency_us=" << P.OrdinaryMaximumLatency
					<< " ordinary_ever_failed=" << P.OrdinaryEverFailed << " ordinary_last_completed_failed=" << P.OrdinaryLastCompletedFailed
					<< " ordinary_active_deadline_failed=" << P.OrdinaryActiveDeadlineFailed << " ordinary_evidence_invalid=" << P.OrdinaryEvidenceInvalid
					<< " ordinary_failure_sequence=" << P.OrdinaryFailureSequence << " ordinary_failure_phase=" << P.OrdinaryFailurePhase
					<< " ordinary_failure_message=" << P.OrdinaryFailureMessageNumber << " ordinary_failure_accepted_us=" << P.OrdinaryFailureAccepted
					<< " ordinary_failure_completed_us=" << P.OrdinaryFailureCompleted << " ordinary_failure_expected_bytes=" << P.OrdinaryFailureExpectedBytes
					<< " ordinary_failure_first_sent_bytes=" << P.OrdinaryFailureFirstSentBytes;
			Out << '\n';
			// Legacy rows keep their schema; hybrid requires an explicit contract and hard certificate fields.
			Out << "[Qualification:FarmF1Failure] event=peer run=" << Run
				<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
				<< " storage_invalid=" << Invalid
				<< " accounting_invalid=" << P.AccountingInvalid
				<< " first_failed_observed=" << P.FirstFailed.Observed
				<< " first_failed_measured=" << P.FirstFailed.Measured << '\n';
			const auto &Initial = P.FirstInitialFailure;
			Out << "[Qualification:FarmF1InitialFailure] event=peer run=" << Run
				<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
				<< " measured=" << Initial.Measured << '\n';
			if (Initial.Measured) {
				Out << "[Qualification:FarmF1InitialFailure] event=grant run=" << Run
					<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
					<< " sequence=" << Initial.Sequence << " token=" << Initial.Token << " bytes=" << Initial.Bytes
					<< " activated_us=" << Initial.Activated << " first_us=" << Initial.First
					<< " completed_us=" << Initial.Completed << " observed_us=" << Initial.ObservedAt
					<< " zero_first_us=" << Initial.First - Initial.Activated
					<< " rate_bytes_per_second=" << network::FiniteGrantServiceCurve::PeerRateBytesPerSecond
					<< " intercept_byte_us=" << network::FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds << '\n';
				const auto &T = Initial.InitialTrace;
				Out << "[Qualification:FarmF1InitialFailure] event=trace run=" << Run
					<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
					<< " valid=" << Initial.InitialTraceValid << " enabled=" << T.Enabled
					<< " token=" << T.Token << " message=" << T.MessageNumber << " native_handle=" << T.NativeHandle
					<< " activated_us=" << Initial.Activated << " transport_entered_us=" << T.TransportEntered
					<< " adapter_locked_us=" << T.AdapterLocked << " native_api_entered_us=" << T.NativeApiEntered
					<< " native_api_locked_us=" << T.NativeApiLocked << " native_enqueued_us=" << T.NativeEnqueued
					<< " pending_at_enqueue=" << T.PendingAtEnqueue << " first_snp_us=" << T.FirstSnpVisit
					<< " pending_at_first_snp=" << T.PendingAtFirstSnpVisit << " preparation_us=" << T.PacketPreparationEntered
					<< " worker_queued_us=" << T.WorkerQueued << " worker_started_us=" << T.WorkerStarted
					<< " native_completed_us=" << T.NativeCompleted << " actor_reconciled_us=" << T.ActorReconciled
					<< " dispatch_sequence=" << T.DispatchSequence
					<< " eligibility_recorded_us=" << T.EligibilityRecorded << " eligibility_native_sample_us=" << T.EligibilityNativeSample
					<< " requested_data_think_native_us=" << T.RequestedDataThinkNative << " eligibility_mode=" << unsigned(T.EligibilityMode)
					<< " prior_queued_us=" << T.PriorWorkerQueued << " prior_started_us=" << T.PriorWorkerStarted
					<< " prior_completed_us=" << T.PriorNativeCompleted << " prior_reconciled_us=" << T.PriorActorReconciled
					<< " prior_positive=" << T.PriorNativePositive << " clock=steady_us eligibility_clock=gns_local_us\n";
			}
			const auto &W = P.FirstFailed;
			if (!W.Observed) continue;
			Out << "[Qualification:FarmF1Failure] event=grant run=" << Run
				<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
				<< " sequence=" << W.Sequence << " token=" << W.Token << " bytes=" << W.Bytes
				<< " activated_us=" << W.Activated << " first_us=" << W.First << " completed_us=" << W.Completed
				<< " observed_us=" << W.ObservedAt << " max_running_byte_us=" << W.MaximumRunning
				<< " segments=" << W.SegmentCount << " measured=" << W.Measured << '\n';
			if (!W.Measured) continue;
			const auto &T = W.InitialTrace;
			if (T.Enabled)
				Out << "[Qualification:FarmF1InitialTrace] run=" << Run << " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
					<< " valid=" << W.InitialTraceValid
					<< " token=" << T.Token << " message=" << T.MessageNumber << " native_handle=" << T.NativeHandle
					<< " activated_us=" << W.Activated << " transport_entered_us=" << T.TransportEntered
					<< " adapter_locked_us=" << T.AdapterLocked << " native_api_entered_us=" << T.NativeApiEntered
					<< " native_api_locked_us=" << T.NativeApiLocked << " native_enqueued_us=" << T.NativeEnqueued
					<< " pending_at_enqueue=" << T.PendingAtEnqueue << " first_snp_us=" << T.FirstSnpVisit
					<< " pending_at_first_snp=" << T.PendingAtFirstSnpVisit << " preparation_us=" << T.PacketPreparationEntered
					<< " worker_queued_us=" << T.WorkerQueued << " worker_started_us=" << T.WorkerStarted
					<< " native_completed_us=" << T.NativeCompleted << " actor_reconciled_us=" << T.ActorReconciled
					<< " dispatch_sequence=" << T.DispatchSequence << " clock=steady_us\n";
			for (std::size_t J = 0; J < W.SegmentCount; ++J)
				Out << "[Qualification:FarmF1Failure] event=segment run=" << Run
					<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
					<< " token=" << W.Token << " sequence=" << J
					<< " at_us=" << W.Segments[J].AtMicroseconds << " bytes=" << W.Segments[J].PayloadBytes << '\n';
		}
	}
};
}

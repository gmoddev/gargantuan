#pragma once

#include "PooledReliableServiceFeedback.hpp"

namespace gargantuan::network::detail {
enum class PooledObservationPoint : std::uint8_t { Other, AdmissionBegin, AdmissionComplete, PeerTeardown };
struct PooledExecutionState {
	bool Captured = false, Invalid = false, Armed = false, HasBoundary = false, PermitAvailable = false;
	std::uint64_t RequiredSince = 0, Boundary = 0, OfferToken = 0, OfferBytes = 0, OfferAt = 0;
	std::uint64_t TurnoverToken = 0, TurnoverAt = 0;
};
inline PooledExecutionState CapturePooledExecution(const PooledReliableServiceFeedback &Value) noexcept {
	return {true, Value.Invalid, Value.OpportunityArmed, Value.HasOpportunityBoundary,
		Value.PendingTurnover && Value.PendingTurnover->PermitAvailable,
		Value.OpportunityRequiredSinceMicroseconds, Value.LastOpportunityBoundaryMicroseconds,
		Value.Offer ? Value.Offer->Token : 0, Value.Offer ? Value.Offer->Bytes : 0,
		Value.Offer ? Value.Offer->ActivatedAtMicroseconds : 0,
		Value.PendingTurnover ? Value.PendingTurnover->Token : 0,
		Value.PendingTurnover ? Value.PendingTurnover->ActivatedAtMicroseconds : 0};
}
enum class PooledLifecyclePoint : std::uint8_t { FailSession, FailPeer, PeerTeardown, LocalStop };
struct PooledLifecycleRecord {
	PooledLifecyclePoint Point;
	ConnectionId Connection;
	std::uint64_t NowMicroseconds = 0;
	std::uint8_t Role = 0, StatusBefore = 0;
	std::uint32_t Reason = 0;
};
// Borrowed observation on the owning GameSession thread. A diagnostic sink
// may copy into bounded storage only; it must not reenter or alter admission.
struct PooledServiceRecord {
	ConnectionId Connection;
	std::uint64_t SimulationTick = 0, NowMicroseconds = 0;
	std::uint64_t DebtToken = 0, DebtBytes = 0;
	std::uint64_t StructuralJournalLag = 0;
	ReliableServiceAcceptedBytes Accepted;
	std::optional<ReliableServiceFeedback> Feedback;
	PooledReliableServiceFeedback::Result Result;
	ReliableByteAdmissionMetrics Admission;
	PooledObservationPoint Point = PooledObservationPoint::Other;
	PooledExecutionState ExecutionBefore;
	bool ExecutionRejected = false, InvalidAfter = false, WorkReady = false;
	bool AcceptanceAvailable = true, DiagnosticOnly = false;
};
struct PooledServiceSink {
	void *Context = nullptr;
	void (*Record)(void *, const PooledServiceRecord &) noexcept = nullptr;
	// Successful production admission, before transport submission. Bootstrap
	// keeps exact debt ownership but deliberately has no qualified F1 clock.
	void (*AcceptedGrant)(void *, ConnectionId, std::uint64_t Token,
		std::uint64_t Bytes, std::uint64_t ActivatedAtMicroseconds) noexcept = nullptr;
	void (*Lifecycle)(void *, const PooledLifecycleRecord &) noexcept = nullptr;
	bool CaptureExecution = false; // Opt-in only; no additional clocks when absent.
};
inline thread_local PooledServiceSink *ActivePooledService = nullptr;
}

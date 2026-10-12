#pragma once

#include "ReliableServiceFeedback.hpp"
#include "FiniteGrantServiceCurve.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <limits>

namespace gargantuan::network::detail {

// Embedded in GameSession's generation-owned Peer. Acceptance is exact; native
// aggregate retirement leaves a bounded interval for the ordinary class split.
// No FIFO-retirement assumption, per-message history or semantic state is kept.
struct PooledReliableServiceFeedback {
	ReliableServiceAcceptedBytes Accepted;
	std::optional<ReliableServiceFeedback> Previous;
	std::uint64_t StructuralRetired = 0;
	std::uint64_t OrdinaryDebt = 0, GameplayLower = 0, GameplayUpper = 0;
	std::uint64_t OrdinaryUnsent = 0;
	bool Invalid = false;
	bool ServiceEligible = false;
	bool QueuedOfferFailed = false;
	bool QueuedOrdinaryFailed = false, QueuedOrdinaryEverFailed = false;
	bool ExecutionFailed = false, MainOpportunityFailed = false, TurnoverFailed = false;
	std::uint64_t MaximumMainOpportunityMicroseconds = 0, MaximumTurnoverMicroseconds = 0;
	std::uint64_t LastCompletedTurnoverToken = 0, LastCompletedTurnoverActivatedAtMicroseconds = 0;
	std::uint64_t LastCompletedTurnoverAtMicroseconds = 0;
	std::uint64_t CompletedTurnovers = 0, CompletedAdmissionOpportunities = 0;
	struct QueuedOffer {
		std::uint64_t Token = 0, Bytes = 0, ActivatedAtMicroseconds = 0, FirstSentBefore = 0;
	};
	std::optional<QueuedOffer> Offer;
	struct Turnover {
		std::uint64_t Token = 0, ActivatedAtMicroseconds = 0;
		bool PermitAvailable = false;
	};
	std::optional<Turnover> PendingTurnover;
	std::uint64_t LastOfferedToken = 0, OpportunityRequiredSinceMicroseconds = 0, LastOpportunityBoundaryMicroseconds = 0;
	bool OpportunityArmed = false, HasOpportunityBoundary = false;

	bool BeginAdmissionOpportunity(std::uint64_t Now, bool WorkReady) {
		if (!AdvanceOpportunity(Now)) return false;
		if ((WorkReady || HasTimedOwnership()) && !OpportunityArmed) {
			OpportunityArmed = true; OpportunityRequiredSinceMicroseconds = Now;
		}
		CheckExecution(Now);
		if (Invalid) { ServiceEligible = false; return false; }
		return !Invalid && !ExecutionFailed;
	}

	bool CompleteAdmissionOpportunity(std::uint64_t Now, bool WorkReady) {
		if (!AdvanceOpportunity(Now)) return false;
		CheckExecution(Now);
		if (Invalid) { ServiceEligible = false; return false; }
		if (OpportunityArmed) {
			if (CompletedAdmissionOpportunities == std::numeric_limits<std::uint64_t>::max()) { Invalid = true; return false; }
			++CompletedAdmissionOpportunities;
		}
		if (PendingTurnover && PendingTurnover->PermitAvailable) {
			if (CompletedTurnovers == std::numeric_limits<std::uint64_t>::max()) { Invalid = true; return false; }
			++CompletedTurnovers;
			LastCompletedTurnoverToken = PendingTurnover->Token;
			LastCompletedTurnoverActivatedAtMicroseconds = PendingTurnover->ActivatedAtMicroseconds;
			LastCompletedTurnoverAtMicroseconds = Now;
			PendingTurnover.reset(); // Metadata only; exact retirement already occurred.
		}
		OpportunityArmed = WorkReady || HasTimedOwnership();
		OpportunityRequiredSinceMicroseconds = Now;
		ServiceEligible &= !ExecutionFailed;
		return !Invalid && !ExecutionFailed;
	}

	// Publication follows exact scheduler acceptance. This is metadata about
	// the already-owned message, not a second payload queue. Native receives
	// the same token, byte count, and activation time at actual submission.
	bool PublishOffer(std::uint64_t Token, std::uint64_t Bytes, std::uint64_t ActivatedAtMicroseconds) {
		if (Invalid || ExecutionFailed || !Token || Token <= LastOfferedToken || !Bytes || Bytes > MaximumReliableServiceGroupBytes ||
			!ActivatedAtMicroseconds || Offer || (PendingTurnover && !PendingTurnover->PermitAvailable) ||
			(ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max() && HasOpportunityBoundary &&
				ActivatedAtMicroseconds < LastOpportunityBoundaryMicroseconds) ||
			Bytes > std::numeric_limits<std::uint64_t>::max() - Accepted.Structural) return false;
		Offer = QueuedOffer{Token, Bytes, ActivatedAtMicroseconds, Accepted.Structural};
		LastOfferedToken = Token;
		if (ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max() && !OpportunityArmed) {
			OpportunityArmed = true; OpportunityRequiredSinceMicroseconds = ActivatedAtMicroseconds;
		}
		QueuedOfferFailed = false;
		return true;
	}

	struct Result {
		bool Valid = false, Available = false, Qualified = false, Terminal = false;
		std::uint64_t ObservedAtMicroseconds = 0, RetiredToken = 0, RetiredBytes = 0;
		bool ExecutionFailed = false;
		bool QueuedOrdinaryEverFailed = false;
		bool MainOpportunityFailed = false, TurnoverFailed = false;
		std::uint64_t MaximumMainOpportunityMicroseconds = 0, MaximumTurnoverMicroseconds = 0;
		std::uint64_t CompletedTurnovers = 0, CompletedAdmissionOpportunities = 0;
		std::uint64_t LastCompletedTurnoverToken = 0, LastCompletedTurnoverActivatedAtMicroseconds = 0;
		std::uint64_t LastCompletedTurnoverAtMicroseconds = 0;
		std::uint64_t OrdinaryUnsent = 0;
	};

	Result Observe(ConnectionId Id, const ReliableServiceAcceptedBytes &Created,
		const std::optional<ReliableServiceFeedback> &Sample, std::uint64_t Now,
		std::uint64_t DebtToken, std::uint64_t Debt) {
		Result Output;
		Output.ExecutionFailed = ExecutionFailed;
		Output.QueuedOrdinaryEverFailed = QueuedOrdinaryEverFailed;
		Output.MainOpportunityFailed = MainOpportunityFailed; Output.TurnoverFailed = TurnoverFailed;
		Output.MaximumMainOpportunityMicroseconds = MaximumMainOpportunityMicroseconds;
		Output.MaximumTurnoverMicroseconds = MaximumTurnoverMicroseconds;
		Output.CompletedTurnovers = CompletedTurnovers; Output.CompletedAdmissionOpportunities = CompletedAdmissionOpportunities;
		Output.LastCompletedTurnoverToken = LastCompletedTurnoverToken;
		Output.LastCompletedTurnoverActivatedAtMicroseconds = LastCompletedTurnoverActivatedAtMicroseconds;
		Output.LastCompletedTurnoverAtMicroseconds = LastCompletedTurnoverAtMicroseconds;
		auto Reject = [&] {
			Invalid = true; ServiceEligible = false;
			Output.Valid = Output.Available = Output.Qualified = false;
			Output.RetiredToken = Output.RetiredBytes = 0; Output.ObservedAtMicroseconds = 0;
			return Output;
		};
		if (Sample && Sample->Connection != Id) return Reject();
		if (Sample && Sample->State == ConnectionState::Closed && (!Sample->CountersValid || Invalid)) {
			Output.Valid = Output.Terminal = true;
			return Output;
		}
		if (Invalid || Created.All < Accepted.All || Created.Structural < Accepted.Structural ||
			Created.Gameplay < Accepted.Gameplay || Created.Structural > Created.All ||
			Created.Gameplay > Created.All - Created.Structural) return Reject();
		const auto OrdinaryCreated = Created.All - Created.Structural;
		const auto PreviousOrdinaryCreated = Accepted.All - Accepted.Structural;
		if (OrdinaryCreated < PreviousOrdinaryCreated) return Reject();
		const auto KnownOrdinaryFirstSent = Previous ? Previous->OrdinaryPayloadBytesFirstSent : 0;
		if (KnownOrdinaryFirstSent > OrdinaryCreated) return Reject();
		OrdinaryUnsent = OrdinaryCreated - KnownOrdinaryFirstSent;
		Output.OrdinaryUnsent = OrdinaryUnsent;
		const auto Added = OrdinaryCreated - PreviousOrdinaryCreated;
		const auto GameAdded = Created.Gameplay - Accepted.Gameplay;
		if (GameAdded > Added || Added > std::numeric_limits<std::uint64_t>::max() - OrdinaryDebt) return Reject();
		OrdinaryDebt += Added; GameplayLower += GameAdded; GameplayUpper += GameAdded;
		Accepted = Created;
		Output.Valid = true;
		if (!Sample) {
			// An unresolved native result cannot qualify new service or age an old
			// observation into a fresh one. Retain exact debt and the last genuine
			// evidence time until a complete native snapshot can be reconciled.
			Output.ObservedAtMicroseconds = Previous ? Previous->ObservedAtMicroseconds : 0;
			ServiceEligible = false;
			return Output;
		}
		const auto &S = *Sample;
		if (!S.CountersValid || S.StructuralCompletionEvidenceInvalid || S.QualifiedOrdinaryEvidenceInvalid || S.ObservedAtMicroseconds > Now ||
			S.UniqueReliableStreamBytesAcked > S.UniqueReliableStreamBytesFirstSent ||
			S.StructuralPayloadBytesAcked > S.StructuralPayloadBytesFirstSent ||
			S.StructuralPayloadBytesFirstSent > S.UniqueReliableStreamBytesFirstSent ||
			S.OrdinaryPayloadBytesFirstSent > OrdinaryCreated ||
			S.OrdinaryPayloadBytesFirstSent > S.UniqueReliableStreamBytesFirstSent - S.StructuralPayloadBytesFirstSent ||
			S.StructuralPayloadBytesFirstSent > Created.Structural ||
			S.StructuralPayloadBytesAcked > S.ReliablePayloadBytesAcked ||
			S.ReliablePayloadBytesAcked - S.StructuralPayloadBytesAcked > S.OrdinaryPayloadBytesFirstSent ||
			S.StructuralPayloadBytesFirstSent - S.StructuralPayloadBytesAcked > MaximumReliableServiceGroupBytes ||
			S.ReliablePayloadBytesAcked > S.UniqueReliableStreamBytesAcked ||
			S.ReliablePayloadBytesAcked > Created.All) return Reject();
		const ReliableServiceFeedback Empty{.Connection = Id};
		const auto &P = Previous ? *Previous : Empty;
		if (S.ObservedAtMicroseconds < P.ObservedAtMicroseconds ||
			S.UniqueReliableStreamBytesFirstSent < P.UniqueReliableStreamBytesFirstSent ||
			S.UniqueReliableStreamBytesAcked < P.UniqueReliableStreamBytesAcked ||
			S.StructuralPayloadBytesFirstSent < P.StructuralPayloadBytesFirstSent ||
			S.StructuralPayloadBytesAcked < P.StructuralPayloadBytesAcked ||
			S.OrdinaryPayloadBytesFirstSent < P.OrdinaryPayloadBytesFirstSent ||
			S.StructuralQualifiedActiveMicroseconds < P.StructuralQualifiedActiveMicroseconds ||
			S.StructuralMaximumDeficitByteMicroseconds < P.StructuralMaximumDeficitByteMicroseconds ||
			S.StructuralMaximumFiniteShortfallByteMicroseconds < P.StructuralMaximumFiniteShortfallByteMicroseconds ||
			S.StructuralCompletedGrantSequence < P.StructuralCompletedGrantSequence ||
			S.StructuralCompletedGrantSequence - P.StructuralCompletedGrantSequence > 1 ||
			S.ReliablePayloadBytesAcked < P.ReliablePayloadBytesAcked ||
			S.ReliableStreamBytesRetransmitted < P.ReliableStreamBytesRetransmitted ||
			S.AttributedRetirementSequence < P.AttributedRetirementSequence ||
			S.AttributedRetirementSequence - P.AttributedRetirementSequence > 1) return Reject();
		if (S.StructuralCompletedGrantSequence != P.StructuralCompletedGrantSequence) {
			if (!Offer || Offer->ActivatedAtMicroseconds == std::numeric_limits<std::uint64_t>::max() ||
				S.StructuralLastCompletedGrantToken != Offer->Token || S.StructuralLastCompletedGrantBytes != Offer->Bytes ||
				S.StructuralLastCompletedGrantActivatedAtMicroseconds != Offer->ActivatedAtMicroseconds ||
				!S.StructuralLastCompletedGrantToken || !S.StructuralLastCompletedGrantBytes ||
				S.StructuralLastCompletedGrantBytes > MaximumReliableServiceGroupBytes ||
				!S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				S.StructuralLastCompletedGrantActivatedAtMicroseconds >
					S.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
				S.StructuralLastCompletedGrantFirstSendAtMicroseconds >
					S.StructuralLastCompletedGrantCompletedAtMicroseconds ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds > S.ObservedAtMicroseconds)
				return Reject();
		} else if (S.StructuralLastCompletedGrantToken != P.StructuralLastCompletedGrantToken ||
			S.StructuralLastCompletedGrantBytes != P.StructuralLastCompletedGrantBytes ||
			S.StructuralLastCompletedGrantActivatedAtMicroseconds !=
				P.StructuralLastCompletedGrantActivatedAtMicroseconds ||
			S.StructuralLastCompletedGrantFirstSendAtMicroseconds !=
				P.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
			S.StructuralLastCompletedGrantCompletedAtMicroseconds !=
				P.StructuralLastCompletedGrantCompletedAtMicroseconds ||
			S.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds !=
				P.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds ||
			S.StructuralLastCompletedGrantFailed != P.StructuralLastCompletedGrantFailed ||
			S.StructuralLastCompletedCompletionFailed != P.StructuralLastCompletedCompletionFailed)
			return Reject();
		if (Offer) {
			if (Created.Structural != Offer->FirstSentBefore + Offer->Bytes ||
				S.StructuralPayloadBytesFirstSent < Offer->FirstSentBefore) return Reject();
			if (Offer->ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max() &&
				S.StructuralPayloadBytesFirstSent == Created.Structural) {
				if (!S.StructuralCompletedGrantSequence || S.StructuralLastCompletedGrantToken != Offer->Token ||
					S.StructuralLastCompletedGrantBytes != Offer->Bytes ||
					S.StructuralLastCompletedGrantActivatedAtMicroseconds != Offer->ActivatedAtMicroseconds) return Reject();
				const auto Elapsed = S.StructuralLastCompletedGrantCompletedAtMicroseconds - Offer->ActivatedAtMicroseconds;
				if (Elapsed > std::numeric_limits<std::uint64_t>::max() / NativeGrantCompletionCurve::RateBytesPerSecond)
					return Reject();
				if (Elapsed * NativeGrantCompletionCurve::RateBytesPerSecond > NativeGrantCompletionCurve::CompletionBudgetByteMicroseconds)
					QueuedOfferFailed = true;
			}
			if (Offer->ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max() &&
				S.StructuralPayloadBytesFirstSent < Created.Structural && S.ObservedAtMicroseconds >= Offer->ActivatedAtMicroseconds) {
				const auto Elapsed = S.ObservedAtMicroseconds - Offer->ActivatedAtMicroseconds;
				if (Elapsed > std::numeric_limits<std::uint64_t>::max() / NativeGrantCompletionCurve::RateBytesPerSecond)
					return Reject();
				if (Elapsed * NativeGrantCompletionCurve::RateBytesPerSecond >
					NativeGrantCompletionCurve::CompletionBudgetByteMicroseconds) QueuedOfferFailed = true;
			}
		}
		const auto Retired = S.ReliablePayloadBytesAcked - P.ReliablePayloadBytesAcked;
		if (S.AttributedRetirementSequence != P.AttributedRetirementSequence) {
			if (!DebtToken || !Debt || S.LastAttributedRetirementToken != DebtToken ||
				S.LastAttributedRetiredPayloadBytes != Debt || !S.LastAttributedRetirementMessageNumber ||
				S.ActiveAttributedRetirementToken || Debt > Retired ||
				(P.ActiveAttributedMessageNumber && S.LastAttributedRetirementMessageNumber != P.ActiveAttributedMessageNumber) ||
				Debt > std::numeric_limits<std::uint64_t>::max() - StructuralRetired) return Reject();
			Output.RetiredToken = DebtToken; Output.RetiredBytes = Debt;
			StructuralRetired += Debt;
		} else if (S.LastAttributedRetirementToken != P.LastAttributedRetirementToken ||
			S.LastAttributedRetirementMessageNumber != P.LastAttributedRetirementMessageNumber ||
			S.LastAttributedRetiredPayloadBytes != P.LastAttributedRetiredPayloadBytes) return Reject();
		if ((S.ActiveAttributedRetirementToken && (S.ActiveAttributedRetirementToken != DebtToken ||
			!S.ActiveAttributedMessageNumber || Output.RetiredBytes)) ||
			(!S.ActiveAttributedRetirementToken && S.ActiveAttributedMessageNumber) ||
			// AckMessage advances this BODY counter and the exact message receipt
			// together, including bootstrap. Aggregate ordinary ACKs cannot stand
			// in for structural retirement or repair a contradictory snapshot.
			StructuralRetired != S.StructuralPayloadBytesAcked ||
			StructuralRetired > Created.Structural || Created.Structural - StructuralRetired != Debt - Output.RetiredBytes)
			return Reject();
		if (S.ActiveAttributedRetirementToken && P.ActiveAttributedRetirementToken &&
			S.ActiveAttributedMessageNumber != P.ActiveAttributedMessageNumber) return Reject();
		const auto OrdinaryRetired = Retired - Output.RetiredBytes;
		if (OrdinaryRetired > OrdinaryDebt) return Reject();
		OrdinaryDebt -= OrdinaryRetired;
		GameplayLower -= std::min(GameplayLower, OrdinaryRetired);
		GameplayUpper = std::min(GameplayUpper, OrdinaryDebt);
		if (GameplayLower > GameplayUpper) return Reject();
		if (Output.RetiredBytes && Offer) {
			if (Offer->Token != Output.RetiredToken || Offer->Bytes != Output.RetiredBytes) return Reject();
			if (Offer->ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max()) {
				if (PendingTurnover) return Reject();
				PendingTurnover = Turnover{Offer->Token, Offer->ActivatedAtMicroseconds};
			}
			Offer.reset();
		}
		Output.ObservedAtMicroseconds = S.ObservedAtMicroseconds;
		Output.Terminal = S.State == ConnectionState::Closed;
		Output.Available = S.State == ConnectionState::Connected &&
			Now - S.ObservedAtMicroseconds <= PooledReliableServiceProfile{}.FeedbackFreshnessMicroseconds;
		// The caller consumes this same result in the admission accountant before
		// completing its pass. A valid but stale receipt may retire exact bytes;
		// it cannot certify that the accountant's structural permit has returned.
		if (PendingTurnover && Output.Available) PendingTurnover->PermitAvailable = true;
		// The native sender checks every first-send event and every observed
		// still-backlogged interval. A short zero-ACK sample is not a failure.
		Output.Qualified = Output.Available && !S.StructuralCompletionServiceFailed && !QueuedOfferFailed &&
			!S.StructuralLastCompletedCompletionFailed && !ExecutionFailed && !QueuedOrdinaryFailed &&
			!S.QualifiedOrdinaryLastCompletedFailed && !S.QualifiedOrdinaryActiveDeadlineFailed;
		OrdinaryUnsent = OrdinaryCreated - S.OrdinaryPayloadBytesFirstSent;
		Output.OrdinaryUnsent = OrdinaryUnsent;
		ServiceEligible = Output.Qualified;
		Previous = S;
		return Output;
	}

private:
	bool HasTimedOwnership() const {
		return PendingTurnover || (Offer && Offer->ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max());
	}
	bool AdvanceOpportunity(std::uint64_t Now) {
		if (Invalid || (HasOpportunityBoundary && Now < LastOpportunityBoundaryMicroseconds)) {
			Invalid = true; ServiceEligible = false; return false;
		}
		HasOpportunityBoundary = true; LastOpportunityBoundaryMicroseconds = Now; return true;
	}
	void CheckExecution(std::uint64_t Now) {
		if (OpportunityArmed) {
			if (Now < OpportunityRequiredSinceMicroseconds) { Invalid = true; return; }
			const auto Elapsed = Now - OpportunityRequiredSinceMicroseconds;
			MaximumMainOpportunityMicroseconds = std::max(MaximumMainOpportunityMicroseconds, Elapsed);
			MainOpportunityFailed |= Elapsed > PooledReliableServiceProfile::AdmissionOpportunityMicroseconds;
		}
		auto CheckTurnover = [&](std::uint64_t Activated) {
			if (Now < Activated) { Invalid = true; return; }
			const auto Elapsed = Now - Activated;
			MaximumTurnoverMicroseconds = std::max(MaximumTurnoverMicroseconds, Elapsed);
			TurnoverFailed |= Elapsed > PooledReliableServiceProfile::StructuralTurnoverMicroseconds;
		};
		if (Offer && Offer->ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max())
			CheckTurnover(Offer->ActivatedAtMicroseconds);
		if (PendingTurnover) CheckTurnover(PendingTurnover->ActivatedAtMicroseconds);
		ExecutionFailed |= MainOpportunityFailed || TurnoverFailed;
		ServiceEligible &= !Invalid && !ExecutionFailed;
	}

public:

	// max(max(game, G0) + max(control, C0)) over the feasible aggregate
	// gameplay interval. This convex function reaches its maximum at an end.
	static std::optional<std::uint64_t> FundedOrdinary(std::uint64_t Total, std::uint64_t Lower, std::uint64_t Upper) {
		if (Lower > Upper || Upper > Total) return {};
		auto At = [&](std::uint64_t Game) -> std::optional<std::uint64_t> {
			const auto G = std::max(Game, PooledReliableServiceProfile::GameplayBurst);
			const auto C = std::max(Total - Game, PooledReliableServiceProfile::ControlBurst);
			if (C > std::numeric_limits<std::uint64_t>::max() - G) return {};
			return G + C;
		};
		auto First = At(Lower), Last = At(Upper);
		return First && Last ? std::optional(std::max(*First, *Last)) : std::nullopt;
	}
};
}

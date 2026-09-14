#pragma once

#include "ReliableServiceFeedback.hpp"
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
	bool Invalid = false;

	struct Result {
		bool Valid = false, Available = false, Qualified = false, Terminal = false;
		std::uint64_t ObservedAtMicroseconds = 0, RetiredToken = 0, RetiredBytes = 0;
	};

	Result Observe(ConnectionId Id, const ReliableServiceAcceptedBytes &Created,
		const std::optional<ReliableServiceFeedback> &Sample, std::uint64_t Now,
		std::uint64_t DebtToken, std::uint64_t Debt) {
		Result Output;
		auto Reject = [&] { Invalid = true; return Result{}; };
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
		const auto Added = OrdinaryCreated - PreviousOrdinaryCreated;
		const auto GameAdded = Created.Gameplay - Accepted.Gameplay;
		if (GameAdded > Added || Added > std::numeric_limits<std::uint64_t>::max() - OrdinaryDebt) return Reject();
		OrdinaryDebt += Added; GameplayLower += GameAdded; GameplayUpper += GameAdded;
		Accepted = Created;
		Output.Valid = true;
		if (!Sample) return Output; // Retain all prior debt while feedback is missing.
		const auto &S = *Sample;
		if (!S.CountersValid || S.ObservedAtMicroseconds > Now ||
			S.UniqueReliableStreamBytesAcked > S.UniqueReliableStreamBytesFirstSent ||
			S.ReliablePayloadBytesAcked > S.UniqueReliableStreamBytesAcked ||
			S.ReliablePayloadBytesAcked > Created.All) return Reject();
		const ReliableServiceFeedback Empty{.Connection = Id};
		const auto &P = Previous ? *Previous : Empty;
		if (S.ObservedAtMicroseconds < P.ObservedAtMicroseconds ||
			S.UniqueReliableStreamBytesFirstSent < P.UniqueReliableStreamBytesFirstSent ||
			S.UniqueReliableStreamBytesAcked < P.UniqueReliableStreamBytesAcked ||
			S.ReliablePayloadBytesAcked < P.ReliablePayloadBytesAcked ||
			S.ReliableStreamBytesRetransmitted < P.ReliableStreamBytesRetransmitted ||
			S.AttributedRetirementSequence < P.AttributedRetirementSequence ||
			S.AttributedRetirementSequence - P.AttributedRetirementSequence > 1) return Reject();
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
		Output.ObservedAtMicroseconds = S.ObservedAtMicroseconds;
		Output.Terminal = S.State == ConnectionState::Closed;
		Output.Available = S.State == ConnectionState::Connected &&
			Now - S.ObservedAtMicroseconds <= PooledReliableServiceProfile::QueueWindowMicroseconds;
		const auto Elapsed = S.ObservedAtMicroseconds - P.ObservedAtMicroseconds;
		if (Previous && Output.Available && P.State == ConnectionState::Connected && Elapsed &&
			Elapsed <= PooledReliableServiceProfile::QueueWindowMicroseconds) {
			const auto Required = (PooledReliableServiceProfile{}.PeerDrainFloor * Elapsed + 999'999) / 1'000'000;
			Output.Qualified = S.UniqueReliableStreamBytesFirstSent - P.UniqueReliableStreamBytesFirstSent >= Required &&
				S.UniqueReliableStreamBytesAcked > P.UniqueReliableStreamBytesAcked;
		}
		Previous = S;
		return Output;
	}

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

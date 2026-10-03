#pragma once

#include "../../network/PooledServiceDiagnostics.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <ostream>
#include <string_view>

namespace gargantuan::host::detail {
// Farm-only Main-thread retention. Native F1 remains grant scoped. Remembering
// that an earlier finite grant failed does not accrue service across grants.
class FarmF1Evidence final {
public:
	struct Peer {
		network::ConnectionId Connection{};
		std::uint64_t Samples = 0, Completed = 0, Qualified = 0, CompletedBytes = 0;
		std::uint64_t Accepted = 0, FirstSent = 0, Acked = 0, Retired = 0;
		std::uint64_t LastToken = 0, LastBytes = 0, LastActivated = 0, LastFirst = 0, LastCompleted = 0;
		std::uint64_t MaximumRunning = 0, MaximumFiniteShortfall = 0;
		bool Failed = false;
	};
private:
	std::array<Peer, 32> Peers{};
	std::size_t Count = 0;
	bool Invalid = false;
	network::detail::PooledServiceSink Sink{this, Record};
	network::detail::PooledServiceSink *Previous = nullptr;
	static void Record(void *Context, const network::detail::PooledServiceRecord &Value) noexcept {
		static_cast<FarmF1Evidence *>(Context)->Observe(Value);
	}
public:
	FarmF1Evidence() noexcept : Previous(network::detail::ActivePooledService) {
		network::detail::ActivePooledService = &Sink;
	}
	~FarmF1Evidence() { network::detail::ActivePooledService = Previous; }
	FarmF1Evidence(const FarmF1Evidence &) = delete;
	FarmF1Evidence &operator=(const FarmF1Evidence &) = delete;
	void Observe(const network::detail::PooledServiceRecord &Value) noexcept {
		if (!Value.Connection.IsValid()) { Invalid = true; return; }
		Peer *P = nullptr;
		for (std::size_t I = 0; I < Count; ++I)
			if (Peers[I].Connection == Value.Connection) { P = &Peers[I]; break; }
		if (!P) {
			if (Count == Peers.size()) { Invalid = true; return; }
			P = &Peers[Count++]; P->Connection = Value.Connection;
		}
		if (!Value.Result.Valid || Value.Accepted.Structural < P->Accepted) P->Failed = true;
		P->Accepted = Value.Accepted.Structural;
		if (!Value.Feedback) return; // Missing feedback cannot create completed evidence.
		const auto &S = *Value.Feedback;
		++P->Samples;
		if (!S.CountersValid || S.Connection != Value.Connection ||
			S.StructuralPayloadBytesFirstSent < P->FirstSent || S.StructuralPayloadBytesAcked < P->Acked ||
			S.StructuralPayloadBytesAcked > S.StructuralPayloadBytesFirstSent ||
			S.StructuralPayloadBytesFirstSent > P->Accepted ||
			S.StructuralCompletedGrantSequence < P->Completed ||
			S.StructuralCompletedGrantSequence - P->Completed > 1 ||
			Value.Result.RetiredBytes > std::numeric_limits<std::uint64_t>::max() - P->Retired)
			P->Failed = true;
		P->Retired += Value.Result.RetiredBytes;
		P->FirstSent = S.StructuralPayloadBytesFirstSent;
		P->Acked = S.StructuralPayloadBytesAcked;
		P->MaximumRunning = std::max(P->MaximumRunning, S.StructuralMaximumDeficitByteMicroseconds);
		P->MaximumFiniteShortfall = std::max(P->MaximumFiniteShortfall,
			S.StructuralMaximumFiniteShortfallByteMicroseconds);
		P->Failed |= S.StructuralServiceFailed || S.StructuralLastCompletedGrantFailed ||
			(Value.Result.Available && !Value.Result.Qualified) || P->MaximumFiniteShortfall != 0 ||
			P->MaximumRunning > network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds;
		if (S.StructuralCompletedGrantSequence != P->Completed) {
			const auto Bytes = S.StructuralLastCompletedGrantBytes;
			if (!Bytes || Bytes > network::MaximumReliableServiceGroupBytes ||
				S.StructuralLastCompletedGrantToken <= P->LastToken ||
				!S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				S.StructuralLastCompletedGrantFirstSendAtMicroseconds < S.StructuralLastCompletedGrantActivatedAtMicroseconds ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds < S.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
				S.StructuralLastCompletedGrantCompletedAtMicroseconds > S.ObservedAtMicroseconds ||
				Bytes > std::numeric_limits<std::uint64_t>::max() - P->CompletedBytes) P->Failed = true;
			P->Completed = S.StructuralCompletedGrantSequence;
			P->CompletedBytes += Bytes;
			if (!S.StructuralLastCompletedGrantFailed && S.CountersValid) ++P->Qualified;
			P->LastToken = S.StructuralLastCompletedGrantToken; P->LastBytes = Bytes;
			P->LastActivated = S.StructuralLastCompletedGrantActivatedAtMicroseconds;
			P->LastFirst = S.StructuralLastCompletedGrantFirstSendAtMicroseconds;
			P->LastCompleted = S.StructuralLastCompletedGrantCompletedAtMicroseconds;
		}
	}
	static bool Complete(const Peer &P) noexcept {
		return !P.Failed && P.Samples && P.Completed && P.Completed == P.Qualified &&
			P.CompletedBytes == P.Accepted && P.Accepted == P.FirstSent &&
			P.FirstSent == P.Acked && P.Acked == P.Retired;
	}
	bool Valid(std::size_t Expected = 32) const noexcept {
		if (Invalid || Count != Expected) return false;
		for (std::size_t I = 0; I < Count; ++I) if (!Complete(Peers[I])) return false;
		return true;
	}
	void Write(std::ostream &Out, std::string_view Run) const {
		for (std::size_t I = 0; I < Count; ++I) {
			const auto &P = Peers[I];
			Out << "[Qualification:FarmF1] event=peer contract=finite_grant_v1 run=" << Run
				<< " connection=" << P.Connection.Slot << ':' << P.Connection.Generation
				<< " samples=" << P.Samples << " completed=" << P.Completed << " qualified=" << P.Qualified
				<< " completed_bytes=" << P.CompletedBytes << " accepted=" << P.Accepted
				<< " first_sent=" << P.FirstSent << " acked=" << P.Acked << " retired=" << P.Retired
				<< " last_token=" << P.LastToken << " last_bytes=" << P.LastBytes
				<< " last_activated_us=" << P.LastActivated << " last_first_us=" << P.LastFirst
				<< " last_completed_us=" << P.LastCompleted << " max_running_byte_us=" << P.MaximumRunning
				<< " max_finite_shortfall_byte_us=" << P.MaximumFiniteShortfall
				<< " failed=" << P.Failed << " valid=" << (!Invalid && Complete(P)) << '\n';
		}
	}
};
}

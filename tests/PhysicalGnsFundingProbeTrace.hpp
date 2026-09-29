#pragma once
#include "../src/network/PooledServiceDiagnostics.hpp"
#include "../src/network/GnsServiceDiagnostics.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace physical_probe {
using namespace gargantuan::network;
constexpr unsigned PeerCount = 4, DemandGroupCount = 1, WaveCount = 32;
constexpr std::uint64_t GroupBytes = MaximumReliableServiceGroupBytes;
constexpr std::uint64_t DeficitBound = PooledReliableServiceProfile::ServiceDeficitBoundByteMicroseconds;
constexpr std::uint64_t FreshnessUs = 50'000;

struct Trace {
	static constexpr std::size_t Capacity = 65536, MaximumGrantsPerPeer = 64;
	struct Row {
		detail::PooledServiceRecord Sample;
		detail::GnsServiceRecord Backend;
		std::uint64_t FirstDelta = 0, AckDelta = 0, RetryDelta = 0;
	};
	struct Grant {
		std::uint64_t Token = 0, Bytes = 0, StartedAtMicroseconds = 0;
		bool Retired = false, Qualified = false;
	};
	struct Peer {
		ConnectionId Id;
		detail::GnsServiceRecord Backend;
		Row Previous;
		bool HasPrevious = false, CampaignBaseline = false;
		std::array<Grant, MaximumGrantsPerPeer> Grants{};
		std::size_t GrantCount = 0;
		std::uint64_t BaselineFirst = 0, BaselineAck = 0, BaselineActiveUs = 0;
		std::uint64_t LastFirst = 0, LastAck = 0, LastActiveUs = 0, MaximumDeficit = 0;
		std::uint64_t Retry = 0, QualifiedGrants = 0;
	};
	std::vector<Row> Rows;
	std::array<Peer, PeerCount> Peers{};
	std::uint64_t CampaignStartedAt = 0, PoolQualifiedUs = 0;
	std::uint64_t PoolAfterFirstBoundaryUs = 0, PoolAfterSecondBoundaryUs = 0, PoolEpisodes = 0;
	unsigned Demand = 0;
	bool Overflow = false, Invalid = false, FloorFailure = false, ProducerStarved = false, Installed = false;
	detail::PooledServiceSink Sink{this, Record};
	detail::GnsServiceSink BackendSink{this, BackendRecord, nullptr};
	detail::PooledServiceSink *PreviousSink = nullptr;
	detail::GnsServiceSink *PreviousBackend = nullptr;
	void Prepare() { Rows.reserve(Capacity); }
	void Install() {
		PreviousSink = detail::ActivePooledService; PreviousBackend = detail::ActiveGnsService;
		detail::ActivePooledService = &Sink; detail::ActiveGnsService = &BackendSink; Installed = true;
	}
	void Restore() {
		if (!Installed) return;
		detail::ActivePooledService = PreviousSink; detail::ActiveGnsService = PreviousBackend; Installed = false;
	}
	Peer *GetPeer(ConnectionId Id) noexcept {
		if (!Id.IsValid()) { Invalid = true; return nullptr; }
		for (auto &P : Peers) if (P.Id == Id) return &P;
		for (auto &P : Peers) if (!P.Id.IsValid()) { P.Id = Id; return &P; }
		Invalid = true; return nullptr;
	}
	static void BackendRecord(void *Context, detail::GnsServiceRecord Value, std::span<const std::byte>) noexcept {
		auto &Self = *static_cast<Trace *>(Context);
		if (auto *P = Self.GetPeer(Value.Connection)) P->Backend = Value;
	}
	Grant *FindGrant(Peer &P, std::uint64_t Token, bool Create) noexcept {
		for (std::size_t I = 0; I < P.GrantCount; ++I)
			if (P.Grants[I].Token == Token) return &P.Grants[I];
		if (!Create || !Token || P.GrantCount == P.Grants.size()) { Invalid = true; return nullptr; }
		return &P.Grants[P.GrantCount++];
	}
	static void Record(void *Context, const detail::PooledServiceRecord &S) noexcept {
		auto &Self = *static_cast<Trace *>(Context);
		if (Self.Rows.size() == Capacity) { Self.Overflow = true; return; }
		auto *P = Self.GetPeer(S.Connection); if (!P) return;
		Row R{S, P->Backend};
		if (S.Admission.ActiveDrainGrants > PeerCount) Self.Invalid = true;
		if (Self.Demand && !Self.CampaignStartedAt) Self.CampaignStartedAt = S.NowMicroseconds;
		if (Self.CampaignStartedAt) {
			if (!S.Feedback || !S.Result.Valid || !S.Result.Available ||
				!S.Feedback->CountersValid || S.Feedback->State != ConnectionState::Connected ||
				S.Feedback->ObservedAtMicroseconds > S.NowMicroseconds ||
				S.NowMicroseconds - S.Feedback->ObservedAtMicroseconds > FreshnessUs)
				Self.Invalid = true;
			if (S.Feedback) {
				const auto &F = *S.Feedback;
				if (F.StructuralPayloadBytesAcked > F.StructuralPayloadBytesFirstSent ||
					F.StructuralPayloadBytesFirstSent - F.StructuralPayloadBytesAcked > GroupBytes ||
					F.StructuralMaximumDeficitByteMicroseconds > DeficitBound || F.StructuralServiceFailed)
					Self.FloorFailure = true;
				if (!P->CampaignBaseline) {
					P->BaselineFirst = F.StructuralPayloadBytesFirstSent;
					P->BaselineAck = F.StructuralPayloadBytesAcked;
					P->BaselineActiveUs = F.StructuralQualifiedActiveMicroseconds;
					P->CampaignBaseline = true;
				}
				if (P->HasPrevious && P->Previous.Sample.Feedback) {
					const auto &Before = *P->Previous.Sample.Feedback;
					if (F.StructuralPayloadBytesFirstSent < Before.StructuralPayloadBytesFirstSent ||
						F.StructuralPayloadBytesAcked < Before.StructuralPayloadBytesAcked ||
						F.StructuralQualifiedActiveMicroseconds < Before.StructuralQualifiedActiveMicroseconds ||
						F.StructuralMaximumDeficitByteMicroseconds < Before.StructuralMaximumDeficitByteMicroseconds ||
						F.ReliableStreamBytesRetransmitted < Before.ReliableStreamBytesRetransmitted)
						Self.Invalid = true;
					else {
						R.FirstDelta = F.StructuralPayloadBytesFirstSent - Before.StructuralPayloadBytesFirstSent;
						R.AckDelta = F.StructuralPayloadBytesAcked - Before.StructuralPayloadBytesAcked;
						R.RetryDelta = F.ReliableStreamBytesRetransmitted - Before.ReliableStreamBytesRetransmitted;
					}
				}
				P->LastFirst = F.StructuralPayloadBytesFirstSent;
				P->LastAck = F.StructuralPayloadBytesAcked;
				P->LastActiveUs = F.StructuralQualifiedActiveMicroseconds;
				P->MaximumDeficit = F.StructuralMaximumDeficitByteMicroseconds;
				P->Retry += R.RetryDelta;
			}
			if (S.DebtToken) {
				auto *G = Self.FindGrant(*P, S.DebtToken, true);
				if (G && G->Token && (G->Bytes != S.DebtBytes || G->Retired)) Self.Invalid = true;
				if (G) {
					if (!G->Token) { G->Token = S.DebtToken; G->Bytes = S.DebtBytes; }
					if (!G->Qualified && S.Feedback && S.Feedback->StructuralActiveGrantStartedAtMicroseconds) {
						G->Qualified = true;
						G->StartedAtMicroseconds = S.Feedback->StructuralActiveGrantStartedAtMicroseconds;
						++P->QualifiedGrants;
					}
				}
			}
			if (S.Result.RetiredBytes) {
				auto *G = Self.FindGrant(*P, S.Result.RetiredToken, false);
				if (!G || G->Retired || G->Bytes != S.Result.RetiredBytes) Self.Invalid = true;
				else G->Retired = true;
			}
		}
		P->Previous = R; P->HasPrevious = true; Self.Rows.push_back(R);
	}
	void Analyze() {
		PoolQualifiedUs = PoolAfterFirstBoundaryUs = PoolAfterSecondBoundaryUs = PoolEpisodes = 0;
		if (!CampaignStartedAt) return;
		std::uint64_t LastEnd = 0;
		std::array<std::uint64_t, PeerCount> PreviousTokens{};
		for (std::size_t Index = 0; Index < Rows.size();) {
			const auto Step = Rows[Index].Sample.SimulationTick;
			std::array<const Row *, PeerCount> Current{};
			while (Index < Rows.size() && Rows[Index].Sample.SimulationTick == Step) {
				const Row *Value = &Rows[Index++];
				for (unsigned P = 0; P < PeerCount; ++P)
					if (Value->Sample.Connection == Peers[P].Id) Current[P] = Value;
			}
			std::uint64_t Begin = 0, End = std::numeric_limits<std::uint64_t>::max();
			std::array<std::uint64_t, PeerCount> Tokens{};
			bool Common = true;
			for (unsigned P = 0; P < PeerCount; ++P) {
				const Row *Value = Current[P];
				if (!Value || !Value->Sample.Feedback || !Value->Sample.DebtToken ||
					!Value->Sample.Result.Available ||
					!Value->Sample.Feedback->StructuralActiveGrantStartedAtMicroseconds) {
					Common = false; break;
				}
				Tokens[P] = Value->Sample.DebtToken;
				Begin = std::max(Begin, Value->Sample.Feedback->StructuralActiveGrantStartedAtMicroseconds);
				End = std::min(End, Value->Sample.Feedback->ObservedAtMicroseconds);
			}
			if (!Common || End <= Begin || End <= LastEnd) continue;
			const auto NewBegin = std::max(Begin, LastEnd);
			const auto Duration = End - NewBegin;
			PoolQualifiedUs += Duration;
			if (End > CampaignStartedAt + 1'000'000)
				PoolAfterFirstBoundaryUs += End - std::max(NewBegin, CampaignStartedAt + 1'000'000);
			if (End > CampaignStartedAt + 2'000'000)
				PoolAfterSecondBoundaryUs += End - std::max(NewBegin, CampaignStartedAt + 2'000'000);
			if (Tokens != PreviousTokens) { ++PoolEpisodes; PreviousTokens = Tokens; }
			LastEnd = End;
		}
	}
	bool Passed() const {
		if (Overflow || Invalid || FloorFailure || ProducerStarved || !CampaignStartedAt ||
			!PoolQualifiedUs || !PoolAfterFirstBoundaryUs || !PoolAfterSecondBoundaryUs || PoolEpisodes < 3)
			return false;
		for (const auto &P : Peers) {
			if (!P.Id.IsValid() || !P.CampaignBaseline || P.QualifiedGrants < 3 ||
				P.LastFirst <= P.BaselineFirst || P.LastAck != P.LastFirst ||
				P.LastActiveUs <= P.BaselineActiveUs || P.MaximumDeficit > DeficitBound ||
				!P.HasPrevious || !P.Previous.Sample.Feedback ||
				P.Previous.Sample.Feedback->PendingReliableStreamBytes ||
				P.Previous.Sample.Feedback->StructuralActiveGrantBytes)
				return false;
			for (std::size_t I = 0; I < P.GrantCount; ++I) if (!P.Grants[I].Retired) return false;
		}
		return true;
	}
	void Dump(const char *Path) const {
		std::ofstream Out(Path); Out.exceptions(std::ios::failbit | std::ios::badbit);
		Out << "simulation_step,slot,generation,consumed_us,feedback_us,present,valid,available,qualified,debt_token,debt_bytes,journal_lag,accepted_structural,structural_first,structural_ack,active_us,current_deficit_byte_us,max_deficit_byte_us,service_failed,active_grant_started_us,active_grant_bytes,first_sent_in_grant,retry,pending,unacked,retirement_sequence,retired_token,result_retired_bytes,grants,total_debt,created,retired,terminal,delta_first,delta_ack,delta_retry,backend_ns,queue_us,rate\n";
		for (const auto &R : Rows) {
			const auto &S = R.Sample; const auto F = S.Feedback.value_or(detail::ReliableServiceFeedback{});
			const auto &M = S.Admission; const auto &B = R.Backend;
			Out << S.SimulationTick << ',' << S.Connection.Slot << ',' << S.Connection.Generation << ',' << S.NowMicroseconds << ',' << F.ObservedAtMicroseconds << ',' << bool(S.Feedback)
				<< ',' << S.Result.Valid << ',' << S.Result.Available << ',' << S.Result.Qualified << ',' << S.DebtToken << ',' << S.DebtBytes << ',' << S.StructuralJournalLag
				<< ',' << S.Accepted.Structural << ',' << F.StructuralPayloadBytesFirstSent << ',' << F.StructuralPayloadBytesAcked
				<< ',' << F.StructuralQualifiedActiveMicroseconds << ',' << F.StructuralCurrentDeficitByteMicroseconds
				<< ',' << F.StructuralMaximumDeficitByteMicroseconds << ',' << F.StructuralServiceFailed
				<< ',' << F.StructuralActiveGrantStartedAtMicroseconds << ',' << F.StructuralActiveGrantBytes
				<< ',' << F.StructuralActiveGrantFirstSentBytes << ',' << F.ReliableStreamBytesRetransmitted
				<< ',' << F.PendingReliableStreamBytes << ',' << F.SentUnackedReliableStreamBytes
				<< ',' << F.AttributedRetirementSequence << ',' << F.LastAttributedRetirementToken << ',' << S.Result.RetiredBytes
				<< ',' << M.ActiveDrainGrants << ',' << M.OutstandingBytes << ',' << M.AcceptedBytes
				<< ',' << M.VerifiedAttributedRetirement << ',' << M.TerminalReleasedBytes
				<< ',' << R.FirstDelta << ',' << R.AckDelta << ',' << R.RetryDelta
				<< ',' << B.Nanoseconds << ',' << B.QueueUs << ',' << B.Rate << '\n';
		}
	}
};
}

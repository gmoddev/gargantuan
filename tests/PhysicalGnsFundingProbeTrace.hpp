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
constexpr std::uint64_t RunningDeficitBound = PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds;
constexpr std::uint64_t PeerDrainRate = 16'777'216;
constexpr std::uint64_t FiniteLatencyByteMicroseconds =
	PeerDrainRate * 6'000 + 1'248'000'000;
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
		std::uint64_t FirstSendAtMicroseconds = 0, CompletedAtMicroseconds = 0;
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
		std::uint64_t PoolEpisodes = 0;
	unsigned Demand = 0;
	bool Overflow = false, Invalid = false, FloorFailure = false, Installed = false;
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
					F.StructuralMaximumDeficitByteMicroseconds > RunningDeficitBound || F.StructuralServiceFailed)
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
			const auto GrantToken = S.DebtToken ? S.DebtToken : S.Result.RetiredToken;
			const auto GrantBytes = S.DebtToken ? S.DebtBytes : S.Result.RetiredBytes;
			if (GrantToken) {
				auto *G = Self.FindGrant(*P, GrantToken, S.DebtToken != 0);
				if (G && G->Token && (G->Bytes != GrantBytes || G->Retired)) Self.Invalid = true;
				if (G) {
					if (!G->Token) { G->Token = GrantToken; G->Bytes = GrantBytes; }
					if (S.Feedback) {
						const auto &F = *S.Feedback;
						const bool Completed = F.StructuralLastCompletedGrantToken == G->Token;
						const auto Started = Completed ? F.StructuralLastCompletedGrantActivatedAtMicroseconds :
							F.StructuralActiveGrantStartedAtMicroseconds;
						const auto First = Completed ? F.StructuralLastCompletedGrantFirstSendAtMicroseconds :
							F.StructuralGrantFirstSendAtMicroseconds;
						const auto Finish = Completed ? F.StructuralLastCompletedGrantCompletedAtMicroseconds :
							F.StructuralGrantCompletedAtMicroseconds;
						if (Completed && F.StructuralLastCompletedGrantBytes != G->Bytes) Self.Invalid = true;
						if (Completed && (F.StructuralLastCompletedGrantFailed ||
							F.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds > RunningDeficitBound))
							Self.FloorFailure = true;
						if (Started && !G->Qualified) {
							G->Qualified = true; G->StartedAtMicroseconds = Started; ++P->QualifiedGrants;
						} else if (Started && G->StartedAtMicroseconds != Started) Self.Invalid = true;
						if (First && (!G->FirstSendAtMicroseconds || G->FirstSendAtMicroseconds == First))
							G->FirstSendAtMicroseconds = First;
						else if (First) Self.Invalid = true;
						if (Finish && (!G->CompletedAtMicroseconds || G->CompletedAtMicroseconds == Finish))
							G->CompletedAtMicroseconds = Finish;
						else if (Finish) Self.Invalid = true;
						if (G->CompletedAtMicroseconds && (!G->Qualified || !G->FirstSendAtMicroseconds ||
							G->StartedAtMicroseconds > G->FirstSendAtMicroseconds ||
							G->FirstSendAtMicroseconds > G->CompletedAtMicroseconds ||
							G->CompletedAtMicroseconds > F.ObservedAtMicroseconds ||
							PeerDrainRate * (G->CompletedAtMicroseconds - G->StartedAtMicroseconds) >
								FiniteLatencyByteMicroseconds + G->Bytes * 1'000'000)) Self.FloorFailure = true;
					}
				}
			}
			if (S.Result.RetiredBytes) {
				auto *G = Self.FindGrant(*P, S.Result.RetiredToken, false);
				if (!G || G->Retired || G->Bytes != S.Result.RetiredBytes || !G->CompletedAtMicroseconds) Self.Invalid = true;
				else G->Retired = true;
			}
		}
		P->Previous = R; P->HasPrevious = true; Self.Rows.push_back(R);
	}
	void Analyze() {
		PoolQualifiedUs = PoolEpisodes = 0;
		if (!CampaignStartedAt) return;
		// Native all-subinterval running-min checks for each grant imply the
		// 64 MiB/s / 4*B_run pool inequality by summing on each real common
		// first-send-backlogged interval. Debt/ACK overlap is not service overlap.
		struct Event { std::uint64_t At; unsigned Peer; bool Start; };
		std::vector<Event> Events;
		for (unsigned Index = 0; Index < PeerCount; ++Index)
			for (std::size_t G = 0; G < Peers[Index].GrantCount; ++G) {
				const auto &Value = Peers[Index].Grants[G];
				if (Value.FirstSendAtMicroseconds && Value.CompletedAtMicroseconds > Value.FirstSendAtMicroseconds) {
					Events.push_back({Value.FirstSendAtMicroseconds, Index, true});
					Events.push_back({Value.CompletedAtMicroseconds, Index, false});
				}
			}
		std::sort(Events.begin(), Events.end(), [](const Event &A, const Event &B) {
			return A.At != B.At ? A.At < B.At : A.Start < B.Start;
		});
		std::array<bool, PeerCount> Active{};
		std::uint64_t PreviousAt = 0;
		bool CommonBefore = false;
		for (const auto &Value : Events) {
			if (CommonBefore && Value.At > PreviousAt) PoolQualifiedUs += Value.At - PreviousAt;
			Active[Value.Peer] = Value.Start;
			const bool CommonAfter = std::all_of(Active.begin(), Active.end(), [](bool On) { return On; });
			if (CommonAfter && !CommonBefore) ++PoolEpisodes;
			CommonBefore = CommonAfter;
			PreviousAt = Value.At;
		}
	}
	bool Passed() const {
		if (Overflow || Invalid || FloorFailure || !CampaignStartedAt ||
			!PoolQualifiedUs || PoolEpisodes < 3)
			return false;
		for (const auto &P : Peers) {
			if (!P.Id.IsValid() || !P.CampaignBaseline || P.QualifiedGrants < 3 ||
				P.LastFirst <= P.BaselineFirst || P.LastAck != P.LastFirst ||
				P.LastActiveUs <= P.BaselineActiveUs || P.MaximumDeficit > RunningDeficitBound ||
				!P.HasPrevious || !P.Previous.Sample.Feedback ||
				P.Previous.Sample.Feedback->PendingReliableStreamBytes ||
				P.Previous.Sample.Feedback->StructuralActiveGrantBytes)
				return false;
			for (std::size_t I = 0; I < P.GrantCount; ++I)
				if (!P.Grants[I].Retired || !P.Grants[I].Qualified ||
					P.Grants[I].Bytes != GroupBytes || !P.Grants[I].FirstSendAtMicroseconds ||
					!P.Grants[I].CompletedAtMicroseconds) return false;
		}
		return true;
	}
	void Dump(const char *Path) const {
		std::ofstream Out(Path); Out.exceptions(std::ios::failbit | std::ios::badbit);
		Out << "simulation_step,slot,generation,consumed_us,feedback_us,present,valid,available,qualified,debt_token,debt_bytes,journal_lag,accepted_structural,structural_first,structural_ack,running_us,current_run_deficit_byte_us,max_run_deficit_byte_us,service_failed,active_grant_started_us,active_grant_bytes,first_sent_in_grant,grant_first_us,grant_complete_us,last_completed_token,last_completed_bytes,last_completed_activated_us,last_completed_first_us,last_completed_finish_us,last_completed_max_run_deficit_byte_us,last_completed_failed,retry,pending,unacked,retirement_sequence,retired_token,result_retired_bytes,grants,total_debt,created,retired,terminal,delta_first,delta_ack,delta_retry,backend_ns,queue_us,rate\n";
		for (const auto &R : Rows) {
			const auto &S = R.Sample; const auto F = S.Feedback.value_or(detail::ReliableServiceFeedback{});
			const auto &M = S.Admission; const auto &B = R.Backend;
			Out << S.SimulationTick << ',' << S.Connection.Slot << ',' << S.Connection.Generation << ',' << S.NowMicroseconds << ',' << F.ObservedAtMicroseconds << ',' << bool(S.Feedback)
				<< ',' << S.Result.Valid << ',' << S.Result.Available << ',' << S.Result.Qualified << ',' << S.DebtToken << ',' << S.DebtBytes << ',' << S.StructuralJournalLag
				<< ',' << S.Accepted.Structural << ',' << F.StructuralPayloadBytesFirstSent << ',' << F.StructuralPayloadBytesAcked
				<< ',' << F.StructuralQualifiedActiveMicroseconds << ',' << F.StructuralCurrentDeficitByteMicroseconds
				<< ',' << F.StructuralMaximumDeficitByteMicroseconds << ',' << F.StructuralServiceFailed
				<< ',' << F.StructuralActiveGrantStartedAtMicroseconds << ',' << F.StructuralActiveGrantBytes
				<< ',' << F.StructuralActiveGrantFirstSentBytes
				<< ',' << F.StructuralGrantFirstSendAtMicroseconds << ',' << F.StructuralGrantCompletedAtMicroseconds
				<< ',' << F.StructuralLastCompletedGrantToken << ',' << F.StructuralLastCompletedGrantBytes
				<< ',' << F.StructuralLastCompletedGrantActivatedAtMicroseconds
				<< ',' << F.StructuralLastCompletedGrantFirstSendAtMicroseconds
				<< ',' << F.StructuralLastCompletedGrantCompletedAtMicroseconds
				<< ',' << F.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
				<< ',' << F.StructuralLastCompletedGrantFailed << ',' << F.ReliableStreamBytesRetransmitted
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

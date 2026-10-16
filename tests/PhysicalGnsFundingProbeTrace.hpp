#pragma once
#include "../src/network/PooledServiceDiagnostics.hpp"
#include "../src/network/GnsServiceDiagnostics.hpp"
#include "../src/network/FiniteGrantServiceCurve.hpp"
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
		bool Cleanup = false;
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
		Row NativePrevious;
		bool HasNativePrevious = false;
		bool HasPrevious = false, CampaignBaseline = false;
		std::array<Grant, MaximumGrantsPerPeer> Grants{};
		std::size_t GrantCount = 0;
		std::uint64_t BaselineFirst = 0, BaselineAck = 0, BaselineActiveUs = 0;
		std::uint64_t LastFirst = 0, LastAck = 0, LastActiveUs = 0, MaximumDeficit = 0;
		std::uint64_t Retry = 0, QualifiedGrants = 0;
		std::uint64_t AcceptedBytes = 0, LastAcceptedToken = 0;
		bool AcceptedQualified = false;
	};
	struct Admission {
		ConnectionId Connection;
		std::uint64_t Token = 0, Bytes = 0, Activated = 0;
		bool Certified = false, Retired = false;
	};
	std::array<Admission, PeerCount * MaximumGrantsPerPeer> Admissions{};
	std::size_t AdmissionCount = 0;
	std::vector<Row> Rows;
	std::array<Peer, PeerCount> Peers{};
	std::uint64_t CampaignStartedAt = 0, PoolQualifiedUs = 0;
		std::uint64_t PoolEpisodes = 0;
	unsigned Demand = 0;
	bool Overflow = false, Invalid = false, FloorFailure = false, Installed = false;
	bool Hybrid = false, NativeCompletionInvalid = false;
	bool CleanupStarted = false;
	std::uint64_t LiveCutOrdinal = 0;
	const char *Contract() const noexcept { return Hybrid ? "HYBRID_F1_V1" : "F1"; }
	static bool NativeCompletionPass(const detail::ReliableServiceFeedback &F) noexcept {
		NativeGrantCompletionCurve Curve;
		if (!F.CountersValid || F.StructuralCompletionServiceFailed || F.StructuralLastCompletedCompletionFailed ||
			F.StructuralCompletionEvidenceInvalid || !F.StructuralCompletedGrantSequence ||
			!Curve.Activate(F.StructuralLastCompletedGrantToken, F.StructuralLastCompletedGrantBytes,
				F.StructuralLastCompletedGrantActivatedAtMicroseconds)) return false;
		const auto Count = F.LastCompletedStructuralSegmentEventCount;
		if (!Count || Count > F.LastCompletedStructuralSegmentEvents.size()) return false;
		for (std::size_t I = 0; I < Count; ++I) {
			const auto &Event = F.LastCompletedStructuralSegmentEvents[I];
			if (!Curve.FirstSend(F.StructuralLastCompletedGrantToken, Event.PayloadBytes, Event.AtMicroseconds)) return false;
		}
		return !Curve.Invalid && !Curve.GrantFailed && Curve.FirstSentBytes == Curve.GrantBytes &&
			Curve.FirstSentAtMicroseconds == F.StructuralLastCompletedGrantFirstSendAtMicroseconds &&
			Curve.CompletedAtMicroseconds == F.StructuralLastCompletedGrantCompletedAtMicroseconds &&
			Curve.CompletedAtMicroseconds <= F.ObservedAtMicroseconds;
	}
	detail::PooledServiceSink Sink{this, Record, AcceptedGrant};
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
	static void AcceptedGrant(void *Context, ConnectionId Id, std::uint64_t Token,
		std::uint64_t Bytes, std::uint64_t Activated) noexcept {
		auto &Self = *static_cast<Trace *>(Context);
		auto *P = Self.GetPeer(Id); if (!P) return;
		if (Self.AdmissionCount == Self.Admissions.size()) { Self.Overflow = true; return; }
		const bool Bootstrap = Activated == std::numeric_limits<std::uint64_t>::max();
		if (!Token || Token <= P->LastAcceptedToken || !Bytes || Bytes > GroupBytes || !Activated ||
			(Bootstrap && P->AcceptedQualified) || Bytes > std::numeric_limits<std::uint64_t>::max() - P->AcceptedBytes ||
			std::any_of(Self.Admissions.begin(), Self.Admissions.begin() + Self.AdmissionCount,
				[&](const Admission &A) { return A.Connection == Id && !A.Retired; })) {
			Self.Invalid = true; return;
		}
		P->LastAcceptedToken = Token; P->AcceptedBytes += Bytes; P->AcceptedQualified |= !Bootstrap;
		Self.Admissions[Self.AdmissionCount++] = Admission{Id, Token, Bytes, Activated};
	}
	Admission *FindAdmission(ConnectionId Id, std::uint64_t Token) noexcept {
		for (std::size_t I = 0; I < AdmissionCount; ++I)
			if (Admissions[I].Connection == Id && Admissions[I].Token == Token) return &Admissions[I];
		return nullptr;
	}
	void ObserveAdmission(const detail::PooledServiceRecord &S, const Peer &P, bool Native = true) noexcept {
		if (S.Accepted.Structural != P.AcceptedBytes) Invalid = true;
		if (Native && S.Feedback && S.Feedback->StructuralLastCompletedGrantToken) {
			const auto &F = *S.Feedback;
			auto *A = FindAdmission(S.Connection, F.StructuralLastCompletedGrantToken);
			if (!A || A->Activated == std::numeric_limits<std::uint64_t>::max() ||
				A->Bytes != F.StructuralLastCompletedGrantBytes || A->Activated != F.StructuralLastCompletedGrantActivatedAtMicroseconds)
				Invalid = true;
			else {
				if (Hybrid && !NativeCompletionPass(F)) NativeCompletionInvalid = FloorFailure = true;
				if ((!Hybrid && (F.StructuralLastCompletedGrantFailed || F.StructuralServiceFailed ||
					F.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds > RunningDeficitBound ||
					PeerDrainRate * (F.StructuralLastCompletedGrantCompletedAtMicroseconds - A->Activated) >
						FiniteLatencyByteMicroseconds + A->Bytes * 1'000'000)) ||
					F.StructuralLastCompletedGrantFirstSendAtMicroseconds < A->Activated ||
					F.StructuralLastCompletedGrantCompletedAtMicroseconds < F.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
					F.StructuralLastCompletedGrantCompletedAtMicroseconds > F.ObservedAtMicroseconds) FloorFailure = true;
				A->Certified = true;
			}
		}
		if (S.Result.RetiredBytes) {
			auto *A = FindAdmission(S.Connection, S.Result.RetiredToken);
			if ((Hybrid && !Native) || !A || A->Retired || A->Bytes != S.Result.RetiredBytes ||
				A->Certified == (A->Activated == std::numeric_limits<std::uint64_t>::max())) Invalid = true;
			else A->Retired = true;
		}
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
	void RecordHybrid(const detail::PooledServiceRecord &S) noexcept {
		if (Rows.size() == Capacity) { Overflow = true; return; }
		auto *P = GetPeer(S.Connection); if (!P) return;
		Row R{S, P->Backend}; R.Cleanup = CleanupStarted;
		if ((!Rows.empty() && S.NowMicroseconds < Rows.back().Sample.NowMicroseconds) ||
			!S.Result.Valid || S.InvalidAfter || S.ExecutionRejected || S.DiagnosticOnly ||
			!S.AcceptanceAvailable || S.Admission.ActiveDrainGrants > PeerCount ||
			S.Admission.TerminalReleasedBytes) Invalid = true;
		if (S.Result.ExecutionFailed || S.Result.MainOpportunityFailed || S.Result.TurnoverFailed ||
			S.Result.QueuedOrdinaryEverFailed || S.Result.QueuedQualifiedOrdinaryEverFailed) FloorFailure = true;
		if (Demand && !CampaignStartedAt) CampaignStartedAt = S.NowMicroseconds;
		const bool Native = S.Feedback && S.Result.Available && S.Feedback->State == ConnectionState::Connected;
		ObserveAdmission(S, *P, Native && !CleanupStarted);
		if (CleanupStarted && (S.DebtToken || S.DebtBytes || S.Result.RetiredBytes ||
			S.Admission.OutstandingBytes || S.Admission.ActiveDrainGrants ||
			!P->HasNativePrevious || S.Accepted.Structural != P->NativePrevious.Sample.Accepted.Structural)) Invalid = true;
		if (!S.Feedback) {
			// A valid owner-accounting boundary is not a native observation. Keep
			// its exact ownership row without inventing service or a clock frontier.
			if (S.Result.Available || S.Result.Qualified || S.Result.Terminal || S.Result.RetiredBytes) Invalid = true;
		} else {
			const auto &F = *S.Feedback;
			if (F.Connection != S.Connection || F.ObservedAtMicroseconds > S.NowMicroseconds ||
				S.NowMicroseconds - F.ObservedAtMicroseconds > FreshnessUs) Invalid = true;
			if (F.StructuralCompletionServiceFailed || F.StructuralLastCompletedCompletionFailed ||
				F.StructuralCompletionEvidenceInvalid || F.ProtectedOrdinaryEverFailed ||
				F.ProtectedOrdinaryActiveDeadlineFailed || F.ProtectedOrdinaryEvidenceInvalid) FloorFailure = true;
			if (!CleanupStarted && (!Native || !F.CountersValid || S.Result.Terminal)) Invalid = true;
			if (CleanupStarted && ((!Native && F.State != ConnectionState::Closing && F.State != ConnectionState::Closed) ||
				(S.Result.Terminal != (F.State == ConnectionState::Closed)))) Invalid = true;
			if (CleanupStarted && (Native ? !S.Result.Qualified : (S.Result.Available || S.Result.Qualified))) Invalid = true;
			if (CleanupStarted && (!F.CountersValid && !(F.State == ConnectionState::Closed && S.Result.Terminal))) Invalid = true;
			if (CleanupStarted && F.CountersValid && P->HasNativePrevious) {
				const auto &Cut = *P->NativePrevious.Sample.Feedback;
				if (F.StructuralPayloadBytesFirstSent != Cut.StructuralPayloadBytesFirstSent ||
					F.StructuralPayloadBytesAcked != Cut.StructuralPayloadBytesAcked ||
					F.StructuralActiveGrantBytes || F.PendingReliableStreamBytes) Invalid = true;
			}
			if (Native && !CleanupStarted) {
				if (!S.Result.Qualified) FloorFailure = true;
				if (F.StructuralPayloadBytesAcked > F.StructuralPayloadBytesFirstSent ||
					F.StructuralPayloadBytesFirstSent > S.Accepted.Structural ||
					F.StructuralPayloadBytesFirstSent - F.StructuralPayloadBytesAcked > GroupBytes) Invalid = true;
				if (!P->CampaignBaseline && CampaignStartedAt) {
					P->BaselineFirst = F.StructuralPayloadBytesFirstSent; P->BaselineAck = F.StructuralPayloadBytesAcked;
					P->BaselineActiveUs = F.StructuralQualifiedActiveMicroseconds; P->CampaignBaseline = true;
				}
				if (P->HasNativePrevious) {
					const auto &Before = *P->NativePrevious.Sample.Feedback;
					if (F.ObservedAtMicroseconds < Before.ObservedAtMicroseconds ||
						F.StructuralPayloadBytesFirstSent < Before.StructuralPayloadBytesFirstSent ||
						F.StructuralPayloadBytesAcked < Before.StructuralPayloadBytesAcked ||
						F.StructuralQualifiedActiveMicroseconds < Before.StructuralQualifiedActiveMicroseconds ||
						F.StructuralMaximumDeficitByteMicroseconds < Before.StructuralMaximumDeficitByteMicroseconds ||
						F.StructuralLastCompletedGrantToken < Before.StructuralLastCompletedGrantToken ||
						F.ReliableStreamBytesRetransmitted < Before.ReliableStreamBytesRetransmitted) Invalid = true;
					else { R.FirstDelta = F.StructuralPayloadBytesFirstSent - Before.StructuralPayloadBytesFirstSent;
						R.AckDelta = F.StructuralPayloadBytesAcked - Before.StructuralPayloadBytesAcked;
						R.RetryDelta = F.ReliableStreamBytesRetransmitted - Before.ReliableStreamBytesRetransmitted; }
				}
				P->LastFirst = F.StructuralPayloadBytesFirstSent; P->LastAck = F.StructuralPayloadBytesAcked;
				P->LastActiveUs = F.StructuralQualifiedActiveMicroseconds; P->MaximumDeficit = F.StructuralMaximumDeficitByteMicroseconds;
				P->Retry += R.RetryDelta;
				P->NativePrevious = R; P->HasNativePrevious = true;
			}
		}
		const auto Token = S.DebtToken ? S.DebtToken : (S.Result.RetiredBytes ? S.Result.RetiredToken : 0);
		if (CampaignStartedAt && !CleanupStarted && Token) {
			auto *G = FindGrant(*P, Token, S.DebtToken != 0);
			const auto Bytes = S.DebtToken ? S.DebtBytes : S.Result.RetiredBytes;
			if (G && G->Token && (G->Bytes != Bytes || G->Retired)) Invalid = true;
			if (G) {
				if (!G->Token) { G->Token = Token; G->Bytes = Bytes; }
				if (Native) {
					const auto &F = *S.Feedback; const bool Completed = F.StructuralLastCompletedGrantToken == Token;
					const auto Started = Completed ? F.StructuralLastCompletedGrantActivatedAtMicroseconds : F.StructuralActiveGrantStartedAtMicroseconds;
					const auto First = Completed ? F.StructuralLastCompletedGrantFirstSendAtMicroseconds : F.StructuralGrantFirstSendAtMicroseconds;
					const auto Finish = Completed ? F.StructuralLastCompletedGrantCompletedAtMicroseconds : F.StructuralGrantCompletedAtMicroseconds;
					const auto *A = FindAdmission(S.Connection, Token);
					if (!A || (Started && (Started != A->Activated || Started > F.ObservedAtMicroseconds)) ||
						(First && (First < Started || First > F.ObservedAtMicroseconds)) ||
						(Finish && (Finish < First || Finish > F.ObservedAtMicroseconds))) Invalid = true;
					if (!Completed && F.StructuralActiveGrantBytes &&
						(F.StructuralActiveGrantBytes != Bytes || F.StructuralActiveGrantFirstSentBytes > Bytes ||
						(!Started || (F.StructuralActiveGrantFirstSentBytes < Bytes && F.ObservedAtMicroseconds >= Started &&
						F.ObservedAtMicroseconds - Started > NativeGrantCompletionCurve::CompletionBudgetByteMicroseconds / PeerDrainRate)))) FloorFailure = true;
					if (Completed && F.StructuralLastCompletedGrantBytes != Bytes) Invalid = true;
					if (Started && !G->Qualified) { G->Qualified = true; G->StartedAtMicroseconds = Started; ++P->QualifiedGrants; }
					else if (Started && G->StartedAtMicroseconds != Started) Invalid = true;
					if (First && (!G->FirstSendAtMicroseconds || G->FirstSendAtMicroseconds == First)) G->FirstSendAtMicroseconds = First;
					else if (First) Invalid = true;
					if (Finish && (!G->CompletedAtMicroseconds || G->CompletedAtMicroseconds == Finish)) G->CompletedAtMicroseconds = Finish;
					else if (Finish) Invalid = true;
				}
				if (S.Result.RetiredBytes) {
					if (!Native || G->Retired || G->Bytes != S.Result.RetiredBytes || !G->CompletedAtMicroseconds) Invalid = true;
					else G->Retired = true;
				}
			}
		}
		P->Previous = R; P->HasPrevious = true; Rows.push_back(R);
	}
	static void Record(void *Context, const detail::PooledServiceRecord &S) noexcept {
		auto &Self = *static_cast<Trace *>(Context);
		if (Self.Hybrid) { Self.RecordHybrid(S); return; }
		if (Self.Rows.size() == Capacity) { Self.Overflow = true; return; }
		auto *P = Self.GetPeer(S.Connection); if (!P) return;
		Self.ObserveAdmission(S, *P);
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
					(!Self.Hybrid && (F.StructuralMaximumDeficitByteMicroseconds > RunningDeficitBound || F.StructuralServiceFailed)) ||
					(Self.Hybrid && (F.StructuralCompletionServiceFailed || F.StructuralCompletionEvidenceInvalid)))
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
						if (!Self.Hybrid && Completed && (F.StructuralLastCompletedGrantFailed ||
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
							(!Self.Hybrid && PeerDrainRate * (G->CompletedAtMicroseconds - G->StartedAtMicroseconds) >
								FiniteLatencyByteMicroseconds + G->Bytes * 1'000'000))) Self.FloorFailure = true;
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
		if (!AdmissionCount || std::any_of(Admissions.begin(), Admissions.begin() + AdmissionCount,
			[](const Admission &A) { return !A.Retired || A.Certified == (A.Activated == std::numeric_limits<std::uint64_t>::max()); }))
			return false;
		for (const auto &P : Peers) {
			const auto &Last = Hybrid ? P.NativePrevious : P.Previous;
			if (!P.Id.IsValid() || !P.CampaignBaseline || P.QualifiedGrants < 3 ||
				P.LastFirst <= P.BaselineFirst || P.LastAck != P.LastFirst ||
				P.LastActiveUs <= P.BaselineActiveUs || (!Hybrid && P.MaximumDeficit > RunningDeficitBound) ||
				!(Hybrid ? P.HasNativePrevious : P.HasPrevious) || !Last.Sample.Feedback ||
				Last.Sample.Feedback->PendingReliableStreamBytes || Last.Sample.Feedback->StructuralActiveGrantBytes)
				return false;
			for (std::size_t I = 0; I < P.GrantCount; ++I)
				if (!P.Grants[I].Retired || !P.Grants[I].Qualified ||
					P.Grants[I].Bytes != GroupBytes || !P.Grants[I].FirstSendAtMicroseconds ||
					!P.Grants[I].CompletedAtMicroseconds) return false;
		}
		return true;
	}
	void BeginCleanup() noexcept {
		if (!Hybrid) return;
		if (CleanupStarted || !Passed()) { Invalid = true; return; }
		CleanupStarted = true; LiveCutOrdinal = Rows.size();
	}
	void DumpAdmissions(const char *Path) const {
		std::ofstream Out(Path); Out.exceptions(std::ios::failbit | std::ios::badbit);
		Out << "slot,generation,token,bytes,activated_us\n";
		for (std::size_t I = 0; I < AdmissionCount; ++I) {
			const auto &A = Admissions[I];
			Out << A.Connection.Slot << ',' << A.Connection.Generation << ',' << A.Token << ',' << A.Bytes << ',' << A.Activated << '\n';
		}
	}
	void Dump(const char *Path) const {
		std::ofstream Out(Path); Out.exceptions(std::ios::failbit | std::ios::badbit);
		Out << "simulation_step,slot,generation,consumed_us,feedback_us,present,valid,available,qualified,debt_token,debt_bytes,journal_lag,accepted_structural,structural_first,structural_ack,running_us,current_run_deficit_byte_us,max_run_deficit_byte_us,service_failed,active_grant_started_us,active_grant_bytes,first_sent_in_grant,grant_first_us,grant_complete_us,last_completed_token,last_completed_bytes,last_completed_activated_us,last_completed_first_us,last_completed_finish_us,last_completed_max_run_deficit_byte_us,last_completed_failed,retry,pending,unacked,retirement_sequence,retired_token,result_retired_bytes,grants,total_debt,created,retired,terminal,delta_first,delta_ack,delta_retry,backend_ns,queue_us,rate";
		if (Hybrid) Out << ",completion_failed,last_completion_failed,completion_evidence_invalid,observation_ordinal,observation_point,native_state,counters_valid,cleanup,result_terminal,execution_rejected,invalid_after,execution_failed,main_failed,turnover_failed,ordinary_failed";
		Out << '\n';
		std::uint64_t Ordinal = 0;
		for (const auto &R : Rows) {
			++Ordinal;
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
				<< ',' << B.Nanoseconds << ',' << B.QueueUs << ',' << B.Rate;
			if (Hybrid) Out << ',' << F.StructuralCompletionServiceFailed << ',' << F.StructuralLastCompletedCompletionFailed
				<< ',' << F.StructuralCompletionEvidenceInvalid << ',' << Ordinal << ',' << unsigned(S.Point)
				<< ',' << (S.Feedback ? unsigned(F.State) : 255) << ',' << (S.Feedback && F.CountersValid)
				<< ',' << R.Cleanup << ',' << S.Result.Terminal << ',' << S.ExecutionRejected << ',' << S.InvalidAfter
				<< ',' << S.Result.ExecutionFailed << ',' << S.Result.MainOpportunityFailed << ',' << S.Result.TurnoverFailed
				<< ',' << (S.Result.QueuedOrdinaryEverFailed || S.Result.QueuedQualifiedOrdinaryEverFailed ||
					F.ProtectedOrdinaryEverFailed || F.ProtectedOrdinaryActiveDeadlineFailed || F.ProtectedOrdinaryEvidenceInvalid);
			Out << '\n';
		}
	}
};
}

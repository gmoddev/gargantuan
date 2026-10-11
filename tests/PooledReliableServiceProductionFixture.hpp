#pragma once
#include "../src/network/ReliableByteAdmission.hpp"
#include "../src/network/PooledReliableServiceFeedback.hpp"
#include "../src/network/FiniteGrantServiceCurve.hpp"
#include "PooledReliableServiceModelFixture.hpp"
#include "PooledReliableServiceRecoveryContractFixture.hpp"
#include <array>
#include <iostream>
#include <vector>

namespace gargantuan::test {
inline bool RunPooledReliableServiceProductionTests() {
	using namespace gargantuan::network;
	using Admission = detail::ReliableByteAdmission;
	using Ledger = detail::PooledReliableServiceFeedback;
	using Sample = detail::ReliableServiceFeedback;
	constexpr auto G = MaximumReliableServiceGroupBytes;
	constexpr ConnectionId Id{1, 1};
	std::size_t Passed = 0;
	auto Check = [](bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); };
	auto Test = [&](const char *Name, auto Body) {
		Body(); ++Passed; std::cout << "[Network:PooledProduction] " << Name << "=PASS\n";
	};
	auto Step = [&](Admission &A, std::uint64_t Time, bool Qualified = true, bool Available = true, std::uint64_t Ordinary = 0) {
		Check(A.BeginStep(Time), "begin");
		Check(A.ObserveService(Id, Admission::ServiceObservation{Time, Qualified, Available, Ordinary}), "observe");
		A.SetOrdinaryFunding(224 * 1024);
	};
	try {
		Test("ExplicitProfileAndFrozenBounds", [&] {
			Check(ReliableServiceProfile{}.Mode == ReliableServiceMode::FULL_RESERVATION, "default changed");
			auto P = ReliableServiceProfile::PooledService(); Check(P.IsValid() && P.IsLatencyCompatible(), "candidate");
			for (int Index = 0; Index < 18; ++Index) {
				auto Bad = P;
				switch (Index) {
				case 0: Bad.MaximumConnections = 31; break;
				case 1: Bad.NonQueueAllowanceMilliseconds = 101; break;
				case 2: --Bad.Pooled.BackendCap; break;
				case 3: ++Bad.Pooled.StructuralPool; break;
				case 4: --Bad.Pooled.GameplayReserve; break;
				case 5: --Bad.Pooled.ControlRealtimeReserve; break;
				case 6: --Bad.Pooled.RequiredTransportReserve; break;
				case 7: --Bad.Pooled.PeerDrainFloor; break;
				case 8: ++Bad.Pooled.PeerCreditRate; break;
				case 9: ++Bad.Pooled.GlobalCreditRate; break;
				case 10: ++Bad.Pooled.PeerBurstCap; break;
				case 11: ++Bad.Pooled.GlobalBurstCap; break;
				case 12: ++Bad.Pooled.PeerPendingCap; break;
				case 13: ++Bad.Pooled.GlobalPendingCap; break;
				case 14: ++Bad.Pooled.MaximumDrainGrants; break;
				case 15: ++Bad.Pooled.FeedbackFreshnessMicroseconds; break;
				case 16: --Bad.Pooled.RequalificationMicroseconds; break;
				case 17: Bad.Mode = static_cast<ReliableServiceMode>(255); break;
				}
				Check(!Bad.IsValid(), "invalid candidate accepted");
			}
		});
		Test("ReservationRollbackAndConservation", [&] {
			Admission A(ReliableServiceProfile::PooledService()); Step(A, 0); Step(A, 250'000);
			Check(!A.Reserve(Id, G + 1), "oversized group");
			auto R = A.Reserve(Id, G); Check(R && A.Rollback(*R) && !A.Rollback(*R), "exact rollback");
			R = A.Reserve(Id, G); Check(R && A.Commit(*R), "commit");
			Check(!A.Retire(Id, R->Token + 1, G) && !A.Retire(Id, R->Token, G - 1), "wrong receipt retired debt");
			A.Remove(Id); Check(A.Debt(Id) == G && A.PeerCount() == 1, "disconnect erased accepted debt");
			Check(A.Retire(Id, R->Token, G) && !A.Retire(Id, R->Token, G), "duplicate receipt");
			Step(A, 500'000); R = A.Reserve(Id, G); Check(R && A.Commit(*R), "second grant");
			Check(A.TerminalRelease(Id), "terminal");
			const auto &M = A.GetMetrics();
			Check(M.AcceptedBytes == M.VerifiedAttributedRetirement + M.TerminalReleasedBytes + M.OutstandingBytes &&
				M.VerifiedAttributedRetirement == G && M.TerminalReleasedBytes == G && !M.ActiveDrainGrants, "conservation");
		});
		Test("SlowMissingStaleAndRequalification", [&] {
			Admission A(ReliableServiceProfile::PooledService()); Step(A, 0, false); Step(A, 250'000, false);
			auto R = A.Reserve(Id, G); Check(R && A.Commit(*R), "first probe");
			Step(A, 600'000); Check(!A.Allowance(Id, 600'000), "non-draining regrant");
			Check(A.Retire(Id, R->Token, G), "retirement");
			Step(A, 700'000, false); Check(!A.Allowance(Id, 700'000), "slow peer skipped cooldown");
			Step(A, 1'250'000, false, false); Check(!A.Allowance(Id, 1'250'000), "missing feedback");
			Step(A, 1'300'000, false); Check(!A.Allowance(Id, 1'350'001), "stale feedback");
			Step(A, 1'400'000, false); R = A.Reserve(Id, G); Check(R && A.Commit(*R), "bounded requalification");
			Check(A.GetMetrics().QualificationGrants == 2, "qualification accounting");
		});
		Test("GrantRetainsOrdinaryFollowerAndTerminalGeneration", [&] {
			Admission A(ReliableServiceProfile::PooledService()); Step(A, 0); Step(A, 250'000);
			auto R = A.Reserve(Id, G); Check(R && A.Commit(*R) && A.Retire(Id, R->Token, G), "grant retirement");
			Step(A, 500'000, true, true, 100);
			Check(!A.Allowance(Id, 500'000) && A.GetMetrics().ActiveDrainGrants == 1, "ordinary follower lost grant");
			Check(!A.ObserveService({1, 2}, Admission::ServiceObservation{}), "generation reused before terminal");
			Check(A.TerminalRelease(Id) && A.ObserveService({1, 2}, Admission::ServiceObservation{500'000, true, true}), "generation reset");
			Check(!A.Allowance({1, 2}, 500'000), "reconnect inherited credit");
		});
		Test("FundedOverloadAndRecovery", [&] {
			Admission A(ReliableServiceProfile::PooledService()); Step(A, 0); Step(A, 250'000);
			A.SetOrdinaryFunding(5 * 1024 * 1024); Check(!A.Allowance(Id, 250'000), "unfunded debt admitted");
			Step(A, 500'000); Check(A.Allowance(Id, 500'000) == G && A.GetMetrics().FundedDeferrals, "funding recovery");
		});
		Test("ExactReceiptIgnoresGameplayAndRetransmission", [&] {
			Ledger L; Sample S{.Connection = Id, .State = ConnectionState::Connected};
			detail::ReliableServiceAcceptedBytes C{164, 100, 64};
			Check(L.Observe(Id, C, S, 0, 7, 100).Valid, "initial");
			S.ObservedAtMicroseconds = 1000; S.UniqueReliableStreamBytesFirstSent = 170;
			S.UniqueReliableStreamBytesAcked = 67; S.ReliablePayloadBytesAcked = 64;
			S.ReliableStreamBytesRetransmitted = 1000;
			auto R = L.Observe(Id, C, S, 1000, 7, 100);
			Check(R.Valid && !R.RetiredBytes && !L.OrdinaryDebt && !L.StructuralRetired, "unrelated ACK attribution");
			S.ObservedAtMicroseconds = 2000; S.UniqueReliableStreamBytesAcked = 170; S.ReliablePayloadBytesAcked = 164;
			S.AttributedRetirementSequence = 1; S.LastAttributedRetirementToken = 7;
			S.LastAttributedRetirementMessageNumber = 9; S.LastAttributedRetiredPayloadBytes = 100;
			R = L.Observe(Id, C, S, 2000, 7, 100); Check(R.Valid && R.RetiredBytes == 100, "matching receipt");
			R = L.Observe(Id, C, S, 2000, 0, 0); Check(R.Valid && !R.RetiredBytes, "duplicate snapshot");
			S.ReliablePayloadBytesAcked--; Check(!L.Observe(Id, C, S, 2000, 0, 0).Valid, "counter reversal");
			S.State = ConnectionState::Closed; S.CountersValid = false;
			R = L.Observe(Id, C, S, 2000, 0, 0); Check(R.Valid && R.Terminal && !R.RetiredBytes, "invalid counters purge only");
		});
		Test("FiniteGrantFirstSendHealthDoesNotRequireShortAckDelta", [&] {
			Ledger L;
			Sample S{.Connection = Id, .State = ConnectionState::Connected};
			Check(L.Observe(Id, {0, 0, 0}, S, 0, 0, 0).Valid, "initial healthy generation");
			S.ObservedAtMicroseconds = 5000;
			S.UniqueReliableStreamBytesFirstSent = 102;
			S.StructuralPayloadBytesFirstSent = 100;
			S.StructuralQualifiedActiveMicroseconds = 4000;
			S.StructuralMaximumDeficitByteMicroseconds = 10'000'000'000ULL;
			S.ActiveAttributedRetirementToken = 7;
			S.ActiveAttributedMessageNumber = 10;
			auto Result = L.Observe(Id, {100, 100, 0}, S, 5000, 7, 100);
			Check(Result.Valid && Result.Available && Result.Qualified &&
				!S.StructuralPayloadBytesAcked && L.ServiceEligible,
				"first-send service stays healthy with a zero-ACK interval");
			S.ObservedAtMicroseconds = 6000;
			S.StructuralMaximumDeficitByteMicroseconds =
				PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds + 1;
			S.StructuralServiceFailed = true;
			Result = L.Observe(Id, {100, 100, 0}, S, 6000, 7, 100);
			Check(Result.Valid && !Result.Qualified && !L.ServiceEligible,
				"persistent native deficit defeats fresh feedback");
			S.ObservedAtMicroseconds = 7000;
			S.UniqueReliableStreamBytesAcked = 102;
			S.ReliablePayloadBytesAcked = 100;
			S.StructuralPayloadBytesAcked = 100;
			S.AttributedRetirementSequence = 1;
			S.ActiveAttributedRetirementToken = 0;
			S.ActiveAttributedMessageNumber = 0;
			S.LastAttributedRetirementToken = 7;
			S.LastAttributedRetirementMessageNumber = 10;
			S.LastAttributedRetiredPayloadBytes = 100;
			Result = L.Observe(Id, {100, 100, 0}, S, 7000, 7, 100);
			Check(Result.Valid && Result.RetiredBytes == 100 && !Result.Qualified,
				"later ACK retires exactly but does not erase a failed grant");
		});
		Test("QueuedOfferChargesBeforeNativeAttribution", [&] {
			Ledger L; Sample S{.Connection = Id, .State = ConnectionState::Connected};
			Check(L.Observe(Id, {0, 0, 0}, S, 1'000'000, 0, 0).Valid, "baseline");
			Check(L.PublishOffer(7, 77, 1'000'000), "one exact accepted offer");
			S.ObservedAtMicroseconds = 1'006'075;
			auto R = L.Observe(Id, {77, 77, 0}, S, 1'006'075, 7, 77);
			Check(R.Valid && !R.Qualified && L.QueuedOfferFailed,
				"scheduler-held accepted bytes cannot evade finite latency");
			S.ActiveAttributedRetirementToken = 7; S.ActiveAttributedMessageNumber = 9;
			S.StructuralPayloadBytesFirstSent = 77; S.UniqueReliableStreamBytesFirstSent = 77;
			R = L.Observe(Id, {77, 77, 0}, S, 1'006'075, 7, 77);
			Check(R.Valid && !R.Qualified && !L.Offer,
				"late catch-up cannot erase an earlier queued-offer failure");
		});
		Test("IncompleteNativeResultPreservesEvidenceFrontier", [&] {
			Ledger L; Sample S{.Connection = Id, .State = ConnectionState::Connected};
			S.ObservedAtMicroseconds = 1'000'000;
			Check(L.Observe(Id, {0, 0, 0}, S, 1'000'000, 0, 0).Qualified,
				"complete baseline qualifies");
			Check(L.PublishOffer(7, 77, 1'000'000), "accepted offer");
			auto R = L.Observe(Id, {77, 77, 0}, {}, 1'001'000, 7, 77);
			Check(R.Valid && !R.Available && !R.Qualified && !L.ServiceEligible &&
				R.ObservedAtMicroseconds == 1'000'000 && L.Previous &&
				L.Previous->ObservedAtMicroseconds == 1'000'000 && L.Offer &&
				L.Accepted.Structural == 77 && !L.StructuralRetired,
				"pending native result cannot publish corrected service or release debt");
			R = L.Observe(Id, {77, 77, 0}, {}, 1'060'001, 7, 77);
			Check(R.Valid && !R.Available && !R.Qualified &&
				R.ObservedAtMicroseconds == 1'000'000 && L.Offer,
				"missing result cannot extend fifty-millisecond freshness");
			S.ObservedAtMicroseconds = 1'060'002;
			S.ActiveAttributedRetirementToken = 7; S.ActiveAttributedMessageNumber = 9;
			S.StructuralPayloadBytesFirstSent = 77; S.UniqueReliableStreamBytesFirstSent = 77;
			S.StructuralServiceFailed = true;
			R = L.Observe(Id, {77, 77, 0}, S, 1'060'002, 7, 77);
			Check(R.Valid && R.Available && !R.Qualified && !L.ServiceEligible && !L.Offer,
				"late native result preserves real failed F1 history");
		});
		Test("SuccessfulRequalificationCanRestoreGrantHealth", [&] {
			Ledger L; Sample S{.Connection = Id, .State = ConnectionState::Connected};
			Check(L.Observe(Id, {0, 0, 0}, S, 1'000'000, 0, 0).Valid, "baseline");
			S.ObservedAtMicroseconds = 1'001'000; S.StructuralServiceFailed = true;
			Check(!L.Observe(Id, {0, 0, 0}, S, 1'001'000, 0, 0).Qualified,
				"failed grant is unhealthy");
			S.ObservedAtMicroseconds = 2'001'000; S.StructuralServiceFailed = false;
			Check(L.Observe(Id, {0, 0, 0}, S, 2'001'000, 0, 0).Qualified,
				"new independent successful qualification can restore service health");
		});
		Test("OrdinarySplitBoundContainsEveryLegalHistory", [&] {
			for (std::uint64_t Total = 0; Total <= 512 * 1024; Total += 1024) {
				auto Bound = Ledger::FundedOrdinary(Total, 0, Total); Check(Bound.has_value(), "funding overflow");
				for (std::uint64_t Game = 0; Game <= Total; Game += 1024)
					Check(*Bound >= std::max(Game, PooledReliableServiceProfile::GameplayBurst) +
						std::max(Total - Game, PooledReliableServiceProfile::ControlBurst), "ordinary underfunding");
			}
		});
		Test("FourReachableMaximumGrantsDrainAndRetire", [&] {
			Admission Production(ReliableServiceProfile::PooledService());
			std::array<Admission::Reservation, 4> Receipts{};
			std::array<FiniteGrantServiceCurve, 4> Curves{};
			for (const std::uint64_t Time : {0ULL, 250'000ULL}) {
				Check(Production.BeginStep(Time), "production credit step");
				for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index)
					Check(Production.ObserveService({Index + 1, 1},
						Admission::ServiceObservation{Time, true, true}), "fresh production peer");
				Production.SetOrdinaryFunding(224 * 1024);
				Production.EndStep();
			}
			Check(Production.BeginStep(250'000), "grant step");
			for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index)
				Check(Production.ObserveService({Index + 1, 1},
					Admission::ServiceObservation{250'000, true, true}), "current-step feedback");
			Production.SetOrdinaryFunding(224 * 1024);
			for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index) {
				const ConnectionId Peer{Index + 1, 1};
				Check(Production.Allowance(Peer, 250'000) == G, "maximum group is eligible");
				auto Receipt = Production.Reserve(Peer, G);
				Check(Receipt && Production.Commit(*Receipt), "production accepts maximum group");
				Receipts[Index] = *Receipt;
				Check(Curves[Index].Activate(Receipt->Token, Receipt->Bytes, 250'000),
					"F1 binds exact production grant token and bytes");
			}
			Check(Production.GetMetrics().ActiveDrainGrants == 4 &&
				Production.GetMetrics().AcceptedBytes == 4 * G, "four production grant slots and bytes");
			Production.EndStep();
			// The first-send schedule is controlled; the grants and receipts above are
			// produced by the unchanged admission implementation, not fabricated offers.
			for (std::uint32_t Packet = 0; Packet < 32; ++Packet) {
				const std::uint64_t At = 250'100 + Packet * 900;
				std::uint64_t CommonDeficit = 0;
				for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index) {
					Check(Curves[Index].Observe(At), "pre-send interval remains healthy");
					CommonDeficit += Curves[Index].CurrentRunningDeficitByteMicroseconds;
				}
				Check(CommonDeficit <= PooledReliableServiceProfile::FourGrantPoolRunningBoundByteMicroseconds,
					"four simultaneously backlogged grants satisfy common pool running bound");
				for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index) {
					Check(Curves[Index].FirstSend(Receipts[Index].Token, 16 * 1024, At),
						"reachable grant satisfies finite and intra-grant curves");
				}
			}
			for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index) {
				Check(Curves[Index].FirstSentBytes == G && !Curves[Index].ServiceFailed &&
					(Curves[Index].CompletedAtMicroseconds - Curves[Index].ActivatedAtMicroseconds) *
						FiniteGrantServiceCurve::PeerRateBytesPerSecond <=
						FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds + G * 1'000'000,
					"maximum grant completes within F1 deadline");
				Check(Production.Retire({Index + 1, 1}, Receipts[Index].Token, G),
					"matching production receipt retires exactly once");
			}
			Check(Production.BeginStep(279'000), "post-ACK service observation");
			for (std::uint32_t Index = 0; Index < Receipts.size(); ++Index)
				Check(Production.ObserveService({Index + 1, 1},
					Admission::ServiceObservation{279'000, true, true}), "ACK-gated grant releases after debt retirement");
			Production.EndStep();
			Check(Production.GetMetrics().ActiveDrainGrants == 0 &&
				Production.GetMetrics().OutstandingBytes == 0 &&
				Production.GetMetrics().VerifiedAttributedRetirement == 4 * G,
				"production grants converge without debt or active slots");
		});
		Test("EqualSizeReplanReplacesDiagnosticDemand", [&] {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
					try { static_cast<EvidenceCapture *>(Context)->Events.push_back(Event); } catch (...) {}
				}
			} Evidence;
			Admission Production(ReliableServiceProfile::PooledService());
			const std::array<std::uint64_t, 2> FirstFingerprint{1, 2}, SecondFingerprint{3, 4};
			for (const auto [Time, Fingerprint] : std::array{
				std::pair{0ULL, FirstFingerprint}, std::pair{250'000ULL, FirstFingerprint},
				std::pair{260'000ULL, SecondFingerprint}, std::pair{270'000ULL, SecondFingerprint}}) {
				Check(Production.BeginStep(Time) && Production.ObserveService(Id,
					Admission::ServiceObservation{Time, true, true}), "exact candidate step");
				Production.SetOrdinaryFunding(224 * 1024);
				Production.DeferSize(Id, G, Fingerprint);
				Production.EndStep();
			}
			std::uint64_t FirstDemand = 0, SecondDemand = 0;
			unsigned Exact = 0, Replaced = 0, EligibleFirst = 0, EligibleSecond = 0;
			for (const auto &Event : Evidence.Events) {
				if (Event.Kind == detail::AdmissionEvidenceKind::ExactDemand) {
					(++Exact == 1 ? FirstDemand : SecondDemand) = Event.DemandId;
					Check(Event.ExactBytes == G, "same-size candidates retain exact byte basis");
				}
				if (Event.Kind == detail::AdmissionEvidenceKind::DemandDisposed &&
					Event.Reason == detail::AdmissionEvidenceReason::Replaced) {
					++Replaced; Check(Event.DemandId == FirstDemand, "old candidate is disposed before replacement");
				}
				if (Event.Kind == detail::AdmissionEvidenceKind::CreditEligible) {
					if (Event.DemandId == FirstDemand) {
						++EligibleFirst; Check(Event.AtMicroseconds == 250'000, "first candidate eligibility");
					} else if (Event.DemandId == SecondDemand) {
						++EligibleSecond; Check(Event.AtMicroseconds == 260'000, "replan starts fresh eligibility");
					}
				}
			}
			Check(Exact == 2 && FirstDemand && SecondDemand && FirstDemand != SecondDemand &&
				Replaced == 1 && EligibleFirst == 1 && EligibleSecond == 1,
				"identical retry keeps one demand; distinct same-size replan starts another");
		});
		Test("GrantLifecycleEvidenceIsTokenAndGenerationScoped", [&] {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
				try { static_cast<EvidenceCapture *>(Context)->Events.push_back(Event); } catch (...) {}
				}
			} Evidence;
			Admission Production(ReliableServiceProfile::PooledService());
			Step(Production, 0);
			Production.DeferSize(Id, G, {1, 2});
			Production.EndStep();
			Step(Production, 250'000);
			Production.DeferSize(Id, G, {1, 2});
			auto First = Production.Reserve(Id, G);
			Check(First && Production.Commit(*First), "first exact grant accepted");
			Check(Production.Retire(Id, First->Token, G), "matching first receipt retired");
			Check(Production.RefreshService(Id, Admission::ServiceObservation{250'000, true, true}),
				"first ACK-gated grant released");
			Production.EndStep();
			Step(Production, 500'000);
			Production.DeferSize(Id, G, {3, 4});
			auto Second = Production.Reserve(Id, G);
			Check(Second && Production.Commit(*Second) && Production.TerminalRelease(Id),
				"second exact grant has a terminal release");
			std::vector<detail::AdmissionEvidenceEvent> Lifecycle;
			for (const auto &Event : Evidence.Events)
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantAccepted ||
					Event.Kind == detail::AdmissionEvidenceKind::GrantRetired ||
					Event.Kind == detail::AdmissionEvidenceKind::GrantReleased ||
					Event.Kind == detail::AdmissionEvidenceKind::GrantTerminalReleased)
					Lifecycle.push_back(Event);
			Check(Lifecycle.size() == 5 &&
				Lifecycle[0].Kind == detail::AdmissionEvidenceKind::GrantAccepted &&
				Lifecycle[1].Kind == detail::AdmissionEvidenceKind::GrantRetired &&
				Lifecycle[2].Kind == detail::AdmissionEvidenceKind::GrantReleased &&
				Lifecycle[3].Kind == detail::AdmissionEvidenceKind::GrantAccepted &&
				Lifecycle[4].Kind == detail::AdmissionEvidenceKind::GrantTerminalReleased,
				"exact accept-retire-release and terminal chronology");
			for (std::size_t Index = 0; Index < Lifecycle.size(); ++Index) {
				const auto &Event = Lifecycle[Index];
				Check(Event.Connection == Id && Event.ExactBytes == G &&
					Event.GrantToken == (Index < 3 ? First->Token : Second->Token) &&
					Event.DemandId == (Index < 3 ? Lifecycle[0].DemandId : Lifecycle[3].DemandId),
					"lifecycle preserves generation, exact bytes, token and demand identity");
			}
			Check(Lifecycle[0].DemandId && Lifecycle[3].DemandId != Lifecycle[0].DemandId &&
				Lifecycle[0].ActiveGrants == 1 && Lifecycle[1].ActiveGrants == 1 &&
				Lifecycle[2].ActiveGrants == 0 && Lifecycle[3].ActiveGrants == 1 &&
				Lifecycle[4].ActiveGrants == 0 &&
				Lifecycle[4].Reason == detail::AdmissionEvidenceReason::TerminalRelease,
				"native active count and positive-debt terminal release follow exact lifecycle");
		});
		Test("ZeroDebtTerminalOwnerReleaseDoesNotReleaseBytes", [&] {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
				try { static_cast<EvidenceCapture *>(Context)->Events.push_back(Event); } catch (...) {}
				}
			} Evidence;
			Admission Production(ReliableServiceProfile::PooledService());
			Step(Production, 0);
			Production.DeferSize(Id, G, {5, 6});
			Production.EndStep();
			Step(Production, 250'000);
			Production.DeferSize(Id, G, {5, 6});
			const auto Grant = Production.Reserve(Id, G);
			Check(Grant && Production.Commit(*Grant) && Production.Retire(Id, Grant->Token, G) &&
				Production.TerminalRelease(Id), "retired grant may clear owned slot at teardown");
			Check(Production.GetMetrics().TerminalReleasedBytes == 0 &&
				Production.GetMetrics().VerifiedAttributedRetirement == G,
				"zero-debt terminal owner cleanup preserves byte conservation");
			unsigned Retired = 0, Terminal = 0;
			for (const auto &Event : Evidence.Events) {
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantRetired) ++Retired;
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantTerminalReleased) {
					++Terminal;
					Check(Event.Reason == detail::AdmissionEvidenceReason::None &&
						Event.GrantToken == Grant->Token && Event.ActiveGrants == 0,
						"terminal owner release is distinct from terminal byte release");
				}
			}
			Check(Retired == 1 && Terminal == 1, "zero-debt lifecycle emits both transitions");
		});
		Test("CreditEligibilityEvidenceExcludesWarmupAndStaleFeedback", [&] {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
					try { static_cast<EvidenceCapture *>(Context)->Events.push_back(Event); } catch (...) {}
				}
			} Evidence;
			Admission Production(ReliableServiceProfile::PooledService());
			const std::array<std::uint64_t, 2> FrameFingerprint{0x1234, 0x5678};
			Check(Production.BeginStep(0) && Production.ObserveService(Id,
				Admission::ServiceObservation{0, true, true}), "initial evidence step");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Id, G, FrameFingerprint);
			Production.EndStep();
			Check(Production.BeginStep(250'000) && Production.ObserveService(Id,
				Admission::ServiceObservation{250'000, false, false}), "unavailable feedback step");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Id, G, FrameFingerprint);
			Production.EndStep();
			Check(Production.BeginStep(260'000) && Production.ObserveService(Id,
				Admission::ServiceObservation{260'000, true, true}), "restored feedback step");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Id, G, FrameFingerprint);
			auto Receipt = Production.Reserve(Id, G);
			Check(Receipt && Production.Commit(*Receipt), "restored credit-eligible grant");
			Production.EndStep();
			std::uint64_t Demand = 0;
			unsigned Exact = 0, Eligible = 0, Accepted = 0;
			for (const auto &Event : Evidence.Events) {
				if (Event.Kind == detail::AdmissionEvidenceKind::ExactDemand) { Demand = Event.DemandId; ++Exact; }
				if (Event.Kind == detail::AdmissionEvidenceKind::CreditEligible) {
					Check(Event.DemandId == Demand && Event.AtMicroseconds == 260'000 &&
						Event.CreditThresholdAtMicroseconds == 250'000,
						"credit crossing and feedback restoration remain separate clocks");
					++Eligible;
				}
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantAccepted) {
					Check(Event.DemandId == Demand && Event.GrantToken == Receipt->Token &&
						Event.EligibleSinceMicroseconds == 260'000 &&
						Event.ExactCandidateFingerprint == FrameFingerprint,
						"grant joins qualified eligibility, not cold demand");
					++Accepted;
				}
			}
			Check(Exact == 1 && Eligible == 1 && Accepted == 1,
				"one exact demand, one qualified eligibility, one grant");
		});
		Test("UnexaminedDemandAndReconnectDoNotLaunderEligibility", [&] {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
					try { static_cast<EvidenceCapture *>(Context)->Events.push_back(Event); } catch (...) {}
				}
			} Evidence;
			Admission Production(ReliableServiceProfile::PooledService());
			Check(Production.BeginStep(0) && Production.ObserveService(Id,
				Admission::ServiceObservation{0, true, true}), "old generation begins");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Id, G);
			Production.EndStep();
			Check(Production.BeginStep(10'000) && Production.ObserveService(Id,
				Admission::ServiceObservation{10'000, true, true}), "old generation remains observed");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.EndStep();
			Production.Remove(Id);
			const ConnectionId Reconnected{1, 2};
			Check(Production.BeginStep(250'000) && Production.ObserveService(Reconnected,
				Admission::ServiceObservation{250'000, true, true}), "new generation begins");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Reconnected, G);
			Production.EndStep();
			Check(Production.BeginStep(500'000) && Production.ObserveService(Reconnected,
				Admission::ServiceObservation{500'000, true, true}), "new generation earns its own credit");
			Production.SetOrdinaryFunding(224 * 1024);
			Production.DeferSize(Reconnected, G);
			auto Receipt = Production.Reserve(Reconnected, G);
			Check(Receipt && Production.Commit(*Receipt), "new generation accepts independently");
			Production.EndStep();
			std::uint64_t OldDemand = 0, NewDemand = 0;
			bool OldUnexamined = false, NewEligible = false, NewAccepted = false;
			for (const auto &Event : Evidence.Events) {
				if (Event.Kind == detail::AdmissionEvidenceKind::ExactDemand) {
					if (Event.Connection == Id) OldDemand = Event.DemandId;
					if (Event.Connection == Reconnected) NewDemand = Event.DemandId;
				}
				if (Event.Kind == detail::AdmissionEvidenceKind::DemandDisposed &&
					Event.Connection == Id && Event.Reason == detail::AdmissionEvidenceReason::Unexamined)
					OldUnexamined = Event.DemandId == OldDemand;
				if (Event.Kind == detail::AdmissionEvidenceKind::CreditEligible && Event.Connection == Reconnected)
					NewEligible = Event.DemandId == NewDemand && Event.AtMicroseconds == 500'000 &&
						Event.CreditThresholdAtMicroseconds == 500'000;
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantAccepted && Event.Connection == Reconnected)
					NewAccepted = Event.DemandId == NewDemand && Event.GrantToken == Receipt->Token;
			}
			Check(OldDemand && NewDemand && OldDemand != NewDemand && OldUnexamined && NewEligible && NewAccepted,
				"unexamined work is visible and a reconnect cannot inherit eligibility");
		});
		Test("ThirtyTwoPeerReferenceDifferential", [&] {
			using namespace pooled_service_model;
			for (const bool Stalled : {false, true}) {
			struct EvidenceCapture {
				std::vector<detail::AdmissionEvidenceEvent> Events;
				bool Overflow = false;
				detail::AdmissionEvidenceSink Sink{this, Record};
				detail::AdmissionEvidenceSink *Previous = detail::ActiveAdmissionEvidence;
				EvidenceCapture() { detail::ActiveAdmissionEvidence = &Sink; }
				~EvidenceCapture() { detail::ActiveAdmissionEvidence = Previous; }
				static void Record(void *Context, const detail::AdmissionEvidenceEvent &Event) noexcept {
					auto &Self = *static_cast<EvidenceCapture *>(Context);
					try { Self.Events.push_back(Event); } catch (...) { Self.Overflow = true; }
				}
			} Evidence;
			Model Reference; All(Reference);
			Admission Production(ReliableServiceProfile::PooledService());
			std::array<std::uint64_t, 32> Tokens{};
			std::array<bool, 32> Pending{}; Pending.fill(true);
			for (std::uint32_t Slot = 1; Slot <= 32; ++Slot) Check(Reference.OfferStructural(Slot, 1, G) == Offer::Queued, "offer");
			std::uint32_t Cursor = 0; std::size_t Completed = 0;
			const std::size_t ExpectedCompletions = Stalled ? 31 : 32;
			for (std::uint64_t Time = 0; Time <= 650'000 && Completed != ExpectedCompletions; Time += 500) {
				if (Time) Reference.Service(500);
				FreshAll(Reference);
				Check(Production.BeginStep(Time), "production step");
				for (std::uint32_t Slot = 1; Slot <= 32; ++Slot) {
					if (Tokens[Slot - 1] && !Reference.At(Slot).structDebt) {
						Check(Production.Retire({Slot, 1}, Tokens[Slot - 1], G), "differential receipt"); Tokens[Slot - 1] = 0; ++Completed;
					}
					Check(Production.ObserveService({Slot, 1}, Admission::ServiceObservation{Time, true, true}), "peer sample");
				}
				Production.SetOrdinaryFunding(224 * 1024);
				for (;;) {
					auto Expected = Reference.Reserve(); std::optional<Admission::Reservation> Actual;
					for (std::uint32_t Offset = 0; Offset < 32; ++Offset) {
						const auto Index = (Cursor + Offset) % 32; if (!Pending[Index]) continue;
						const ConnectionId Peer{Index + 1, 1}; Production.DeferSize(Peer, G);
						if (Production.Allowance(Peer, Time) < G) continue;
						Actual = Production.Reserve(Peer, G); break;
					}
					Check(bool(Expected) == bool(Actual), "model/production admission mismatch");
					if (!Actual) break;
					Check(Expected->slot == Actual->Connection.Slot && Expected->bytes == Actual->Bytes, "fairness mismatch");
					Check(Reference.Accept(*Expected) && Production.Commit(*Actual), "accept");
					if (Stalled && Expected->slot == 1) Check(Reference.DrainRate(1, 1, 0), "non-draining peer");
					const auto Index = Actual->Connection.Slot - 1; Tokens[Index] = Actual->Token; Pending[Index] = false; Cursor = (Index + 1) % 32;
				}
				Production.EndStep();
				Check(Production.GetMetrics().ActiveDrainGrants == Reference.Grants() &&
					Production.GlobalCredit() == Reference.GlobalCredit(), "grant/credit differential");
			}
			Check(Completed == ExpectedCompletions && Production.GetMetrics().VerifiedAttributedRetirement == ExpectedCompletions * G &&
				Production.GetMetrics().OutstandingBytes == (Stalled ? G : 0) && Production.LogicalBytes() < 64 * 1024, "convergence/resource bounds");
			Check(!Evidence.Overflow, "admission evidence did not overflow");
			std::array<std::uint64_t, 32> EligibleAt{};
			std::array<std::uint64_t, 32> EligibleDemand{};
			std::array<unsigned, 32> AcceptedCounts{};
			std::uint64_t MaximumEligibleWait = 0;
			for (const auto &Event : Evidence.Events) {
				Check(Event.Connection.Generation == 1 && Event.Connection.Slot >= 1 && Event.Connection.Slot <= 32,
					"admission evidence retains peer generation");
				const auto Index = Event.Connection.Slot - 1;
				if (Event.Kind == detail::AdmissionEvidenceKind::CreditEligible) {
					Check(Event.ExactBytes == G && Event.CreditThresholdAtMicroseconds <= Event.AtMicroseconds,
						"credit eligibility uses the exact maximum group and monotonic crossing");
					EligibleAt[Index] = Event.AtMicroseconds;
					EligibleDemand[Index] = Event.DemandId;
				}
				if (Event.Kind == detail::AdmissionEvidenceKind::GrantAccepted) {
					Check(Event.GrantToken && Event.ExactBytes == G && EligibleAt[Index] &&
						EligibleDemand[Index] == Event.DemandId && Event.AtMicroseconds >= EligibleAt[Index],
						"accepted grant joins one exact same-generation eligible demand");
					MaximumEligibleWait = std::max(MaximumEligibleWait, Event.AtMicroseconds - EligibleAt[Index]);
					++AcceptedCounts[Index];
				}
			}
			for (const auto Count : AcceptedCounts) Check(Count == 1, "every 32-peer demand has one accepted grant");
			for (const auto At : EligibleAt) Check(At == 250'000,
				"all 32 exact groups become credit-eligible even while four grant slots are occupied");
			if (!Stalled) Check(MaximumEligibleWait <= 220'500,
				"500-us reference model's conditional seven-wave fairness bound");
			std::cout << "[Network:PooledProduction] Differential stalled=" << Stalled << " completed=" << Completed
				<< " outstanding=" << Production.GetMetrics().OutstandingBytes << " logical_bytes=" << Production.LogicalBytes()
				<< " eligible_grant_wait_max_us=" << MaximumEligibleWait << '\n';
			}
		});
	} catch (const std::exception &Error) {
		std::cerr << "[Network:PooledProduction] FAIL " << Error.what() << '\n'; return false;
	}
	std::cout << "[Network:PooledProduction] cases=" << Passed << " PASS\n";
	return Passed == 18;
}

// Candidate-only deterministic schedules. The real admission accountant is
// reused; Qualified=true supplies the most favorable proposed health input.
// Neither production F1 nor GNS is invoked or changed by these schedules.
namespace HybridCapacityCandidate {
using U = std::uint64_t;
using Admission = network::detail::ReliableByteAdmission;
inline constexpr U G = network::MaximumReliableServiceGroupBytes;
inline constexpr U FrameUs = 16'667;
inline constexpr U Rate = network::PooledReliableServiceProfile::PeerActiveGrantDrainCapacityFloor;
inline constexpr U NativeAdvance = network::PooledReliableServiceProfile::ServiceQuantumBytes + 1'300;
inline constexpr U CompletionNumerator = Rate * 6'000 + (G + 1'248) * 1'000'000;
inline constexpr U CapacityLatencyNumerator = Rate * (250'000 + 220'500) + CompletionNumerator;

struct Input {
	U Bytes = G, GroupsPerPeer = 32, NativeRate = network::ReliableServiceProfile::PooledService().BackendSendRate();
	U AckDelayUs = 0, OrdinaryHoldUs = 0, SourceIntervalUs = 0;
	U ProbeUs = 6'000'000;
	bool Unfair = false, RollbackOnce = false, Mixed = false;
};
struct Result {
	U Accepted = 0, Sent = 0, Acked = 0, Retired = 0, CompletedAt = 0;
	U ProbeSent = 0, ProbeAt = 0, ProbeOutstanding = 0;
	U InitialWaitMax = 0, NativeDurationMax = 0, DebtHigh = 0, SlotsHigh = 0;
	U RolledBack = 0, MinimumFirstWaveCount = 0;
	bool CapacityMeasured = false, CapacityFailed = false, FairnessFailed = false;
	bool CompletionFailed = false, BurstFailed = false, Converged = false;
};
inline Result Run(Input Configuration) {
	Admission Account(network::ReliableServiceProfile::PooledService());
	struct Peer {
		U AcceptedGroups = 0, FirstAcceptedAt = 0, Activated = 0, CompleteAt = 0, NativeSent = 0;
		bool Active = false, HasFirst = false, Retired = false;
		Admission::Reservation Receipt;
	};
	std::array<Peer, 32> Peers{};
	std::array<U, 32> Sent{};
	Result Output;
	U Cursor = 0;
	bool RolledBack = false, Probed = false;
	const auto Require = [](bool Value) { if (!Value) throw std::runtime_error("hybrid admission schedule invariant"); };
	Require(Configuration.Bytes >= 77 && Configuration.Bytes <= G && Configuration.NativeRate > 0);
	Require(Account.BeginStep(0));
	for (std::uint32_t Index = 0; Index < 32; ++Index)
		Require(Account.ObserveService({Index + 1, 1}, Admission::ServiceObservation{0, true, true}));
	Account.SetOrdinaryFunding(224 * 1024); Account.EndStep();
	// Precredit is reachable after 250 ms. Thereafter observations are strictly
	// periodic: no favorable jitter, extra polls, CPU cost or scheduler catch-up.
	for (U Frame = 0; Frame < 50'000; ++Frame) {
		const U Now = 250'000 + Frame * FrameUs;
		Require(Account.BeginStep(Now));
		for (std::uint32_t Index = 0; Index < 32; ++Index) {
			auto &Value = Peers[Index];
			U OrdinaryDebt = 0;
			if (Value.Active) {
				// All configured wire bytes are granted to unique structural data;
				// even initial credit and final-packet overdraw are free. This fluid
				// upper service budget is deliberately more favorable than GNS.
				const U Progress = std::min(Value.Receipt.Bytes, NativeAdvance +
					Configuration.NativeRate * (Now - Value.Activated) / 1'000'000);
				Require(Progress >= Value.NativeSent);
				Sent[Index] += Progress - Value.NativeSent;
				Output.Sent += Progress - Value.NativeSent; Value.NativeSent = Progress;
				if (Now >= Value.CompleteAt && !Value.Retired &&
					Now - Value.CompleteAt >= Configuration.AckDelayUs) {
					Require(Value.NativeSent == Value.Receipt.Bytes);
					Require(Account.Retire({Index + 1, 1}, Value.Receipt.Token, Value.Receipt.Bytes));
					Require(!Account.Retire({Index + 1, 1}, Value.Receipt.Token, Value.Receipt.Bytes));
					Output.Acked += Value.Receipt.Bytes; Value.Retired = true;
				}
				if (Now < Value.CompleteAt + Configuration.AckDelayUs + Configuration.OrdinaryHoldUs)
					OrdinaryDebt = Configuration.OrdinaryHoldUs ? 1 : 0;
			}
			Require(Account.ObserveService({Index + 1, 1}, Admission::ServiceObservation{Now, true, true, OrdinaryDebt}));
			if (Value.Active && Value.Retired && !OrdinaryDebt) Value.Active = false;
		}
		Account.SetOrdinaryFunding(224 * 1024);
		const U Start = Configuration.Unfair ? 0 : Cursor;
		for (U Offset = 0; Offset < 32; ++Offset) {
			const auto Index = static_cast<std::uint32_t>((Start + Offset) % 32);
			auto &Value = Peers[Index]; const network::ConnectionId Id{Index + 1, 1};
			if (Value.Active || Value.AcceptedGroups == Configuration.GroupsPerPeer) { Account.NoWork(Id); continue; }
			if (Configuration.SourceIntervalUs && Now - 250'000 < Value.AcceptedGroups * Configuration.SourceIntervalUs) {
				Account.NoWork(Id); continue;
			}
			const std::array<U, 3> MixedSizes{77, 64 * 1024, G};
			const U Bytes = Configuration.Mixed ? MixedSizes[(Index + Value.AcceptedGroups) % 3] : Configuration.Bytes;
			Account.DeferSize(Id, Bytes);
			if (Configuration.Unfair && Index == 31) continue;
			auto Receipt = Account.Reserve(Id, Bytes);
			if (!Receipt) continue;
			if (Configuration.RollbackOnce && !RolledBack) {
				Require(Account.Rollback(*Receipt)); RolledBack = true; continue;
			}
			Require(Account.Commit(*Receipt)); Value.Receipt = *Receipt;
			Value.Active = true; Value.Retired = false; Value.Activated = Now;
			Value.NativeSent = std::min(Bytes, NativeAdvance);
			Sent[Index] += Value.NativeSent; Output.Sent += Value.NativeSent;
			const U Remaining = Bytes - Value.NativeSent;
			const U Duration = pooled_service_model::ceilUs(Remaining, Configuration.NativeRate);
			Value.CompleteAt = Now + Duration;
			Output.NativeDurationMax = std::max(Output.NativeDurationMax, Duration);
			Output.CompletionFailed |= Duration * Rate > CompletionNumerator;
			if (Bytes == G) Output.BurstFailed |= Duration * Rate > G * 1'000'000;
			if (!Value.HasFirst) { Value.HasFirst = true; Value.FirstAcceptedAt = Now; }
			++Value.AcceptedGroups; Cursor = (Index + 1) % 32;
		}
		Account.EndStep();
		const auto Metrics = Account.GetMetrics();
		Output.Accepted = Metrics.AcceptedBytes; Output.Retired = Metrics.VerifiedAttributedRetirement;
		Output.DebtHigh = std::max(Output.DebtHigh, Metrics.OutstandingBytes);
		Output.SlotsHigh = std::max(Output.SlotsHigh, Metrics.ActiveDrainGrants);
		Require(Metrics.OutstandingBytes <= 4 * G && Metrics.ActiveDrainGrants <= 4);
		Require(Output.Retired == Output.Acked && Output.Acked <= Output.Sent && Output.Sent <= Output.Accepted);
		Require(Output.Accepted == Output.Retired + Metrics.OutstandingBytes && !Metrics.TerminalReleasedBytes);
		Require(Account.GlobalCredit() <= 4 * G);
		if (!Probed && Now >= Configuration.ProbeUs) {
			Probed = true; Output.ProbeSent = Output.Sent; Output.ProbeAt = Now;
			Output.ProbeOutstanding = Metrics.OutstandingBytes;
			Output.CapacityMeasured = Configuration.Bytes == G && !Configuration.Mixed && !Configuration.SourceIntervalUs &&
				std::ranges::all_of(Peers, [&](const Peer &Value) { return Value.AcceptedGroups < Configuration.GroupsPerPeer; });
			if (Output.CapacityMeasured && Now * Rate > CapacityLatencyNumerator)
				for (const U Bytes : Sent)
					Output.CapacityFailed |= Bytes * 8 * 1'000'000 < Now * Rate - CapacityLatencyNumerator;
		}
		bool Finished = !Metrics.ActiveDrainGrants;
		for (const auto &Value : Peers) Finished &= Value.AcceptedGroups == Configuration.GroupsPerPeer;
		if (Finished) { Output.Converged = true; Output.CompletedAt = Now; break; }
		if (Configuration.Unfair && Probed) { Output.CompletedAt = Now; break; }
	}
	U FirstWaveCount = 0;
	for (const auto &Value : Peers) {
		if (Value.HasFirst) {
			++FirstWaveCount;
			Output.InitialWaitMax = std::max(Output.InitialWaitMax, Value.FirstAcceptedAt - 250'000);
		} else Output.InitialWaitMax = std::max(Output.InitialWaitMax, Output.CompletedAt - 250'000);
	}
	Output.MinimumFirstWaveCount = FirstWaveCount;
	Output.FairnessFailed = Output.InitialWaitMax > 220'500;
	Output.RolledBack = Account.GetMetrics().RolledBackBytes;
	return Output;
}

inline bool RunTests() {
	U Passed = 0;
	const auto Check = [](bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); };
	const auto Test = [&](const char *Name, auto Body) {
		Body(); ++Passed; std::cout << "[Network:HybridCapacity] " << Name << "=PASS\n";
	};
	try {
		Test("CodecProducesMaximumNineOperationBarrierGroup", [&] {
			network::ReplicationFrame Frame{
				.Version = network::ReplicationProtocolVersion, .Kind = network::ReplicationMessageKind::Incremental,
				.Epoch = network::ReplicationEpoch(1), .Sequence = network::ReliableReplicationSequence(1)};
			for (std::uint32_t Index = 0; Index < 8; ++Index)
				Frame.Operations.push_back({Frame.Epoch, network::PropertyReplicationUpdate{
					.Object = {Index + 1, 1}, .PropertyName = "Name", .Value = std::string(64 * 1024 - 256, 'a')}});
			Frame.Operations.push_back({Frame.Epoch, network::AttributeReplicationUpdate{
				.Object = {1, 1}, .AttributeName = "Fence", .Value = true}});
			auto Encoded = network::EncodeReplicationFrame(Frame);
			Check(bool(Encoded) && Encoded->size() + 32 < G, "legal maximum-shape seed encodes");
			U Gap = G - 32 - Encoded->size();
			for (std::uint32_t Index = 0; Index < 8 && Gap; ++Index) {
				auto &Update = std::get<network::PropertyReplicationUpdate>(Frame.Operations[Index].Intent);
				auto &Name = std::get<std::string>(Update.Value);
				const U Added = std::min<U>(64 * 1024 - Name.size(), Gap);
				Name.append(Added, 'a'); Gap -= Added;
			}
			Encoded = network::EncodeReplicationFrame(Frame);
			Check(!Gap && bool(Encoded) && Encoded->size() + 32 == G &&
				bool(network::DecodeReplicationFrame(*Encoded)), "maximum complete group is real GRPL, not fabricated bytes");
			// The codec establishes legal shape, not a replay of 1280 live source
			// revisions. Attribute barriers are the existing ordered-history seam.
		});
		Test("OptimisticMaximumGrantsMissNominalCapacityAndFairness", [&] {
			const auto Value = Run({});
			Check(Value.Converged && Value.MinimumFirstWaveCount == 32 && !Value.CompletionFailed && !Value.BurstFailed,
				"timely grants and complete 32-peer service");
			Check(Value.CapacityMeasured && Value.CapacityFailed && Value.FairnessFailed && Value.InitialWaitMax == 233'338,
				"real cadence cannot satisfy retained capacity/fairness in the optimistic schedule");
			Check(Value.Accepted == 32 * 32 * G && Value.Sent == Value.Accepted && Value.Retired == Value.Accepted,
				"failure is not false conservation or unbounded debt");
			std::cout << "[Network:HybridCapacity] nominal frame_us=" << FrameUs
				<< " native_duration_us=" << Value.NativeDurationMax << " slot_cycle_us=" << 2 * FrameUs
				<< " eligible_wait_us=" << Value.InitialWaitMax << " probe_at_us=" << Value.ProbeAt
				<< " probe_unique_bytes=" << Value.ProbeSent << " final_bytes=" << Value.Accepted << '\n';
		});
		Test("AckDelayAndOrdinaryDebtKeepSlotsOwned", [&] {
			const auto Fast = Run({.GroupsPerPeer = 2, .NativeRate = Rate});
			const auto Delayed = Run({.GroupsPerPeer = 2, .NativeRate = Rate, .AckDelayUs = 3'000});
			const auto Ordinary = Run({.GroupsPerPeer = 2, .NativeRate = Rate, .OrdinaryHoldUs = 3'000});
			Check(Fast.Converged && Delayed.Converged && Ordinary.Converged &&
				Delayed.InitialWaitMax == 350'007 && Ordinary.InitialWaitMax == 350'007,
				"ACK or ordinary debt delays release to third Main observation");
			Check(Delayed.CompletedAt > Fast.CompletedAt && Ordinary.CompletedAt > Fast.CompletedAt,
				"native completion cannot prematurely release a slot");
			std::cout << "[Network:HybridCapacity] delayed native_duration_us=" << Delayed.NativeDurationMax
				<< " ack_delay_us=3000 eligible_wait_us=" << Delayed.InitialWaitMax << '\n';
		});
		Test("FourteenMiBSenderIsRejectedDespiteTimelyCompletion", [&] {
			const auto Value = Run({.GroupsPerPeer = 2, .NativeRate = 14 * 1024 * 1024});
			Check(Value.Converged && !Value.CompletionFailed && Value.BurstFailed && Value.FairnessFailed,
				"a 14-MiB/s sender may pass H1 but must fail burst capability");
		});
		Test("CorruptRotationCannotHideStarvedPeer", [&] {
			const auto Value = Run({.GroupsPerPeer = 64, .Unfair = true});
			Check(!Value.Converged && Value.MinimumFirstWaveCount == 31 && Value.FairnessFailed,
				"one omitted ready peer fails regardless of aggregate progress");
		});
		Test("TinyAndMediumGroupsRespectGrantCountCeiling", [&] {
			for (const U Bytes : {77ULL, 64ULL * 1024}) {
				const auto Value = Run({.Bytes = Bytes, .GroupsPerPeer = 16});
				Check(Value.Converged && !Value.CapacityMeasured && !Value.CompletionFailed &&
					Value.InitialWaitMax == 7 * FrameUs && Value.Sent == 32 * 16 * Bytes,
					"small legal groups cannot be mislabeled a 64-MiB/s capacity offer");
			}
		});
		Test("InsufficientSourceProductionCannotQualifyCapacity", [&] {
			const auto Value = Run({.GroupsPerPeer = 8, .SourceIntervalUs = 1'000'000});
			Check(Value.Converged && !Value.CapacityMeasured && Value.Sent == 32 * 8 * G,
				"credit or a low offered rate cannot certify physical capacity");
		});
		Test("MixedLegalGroupsConserveAndServeEveryPeer", [&] {
			const auto Value = Run({.GroupsPerPeer = 16, .Mixed = true});
			Check(Value.Converged && Value.MinimumFirstWaveCount == 32 && !Value.CapacityMeasured &&
				!Value.CompletionFailed && !Value.BurstFailed && !Value.FairnessFailed &&
				Value.Accepted == Value.Sent && Value.Sent == Value.Retired,
				"mixed grant sizes preserve exact accounting without a fabricated universal byte-rate claim");
		});
		Test("RollbackAndExactRetirementPreserveDebt", [&] {
			const auto Value = Run({.GroupsPerPeer = 1, .RollbackOnce = true});
			Check(Value.Converged && Value.RolledBack == G && Value.Accepted == 32 * G &&
				Value.Retired == Value.Accepted && Value.DebtHigh == 4 * G && Value.SlotsHigh == 4,
				"preaccept backpressure neither loses work nor fabricates retirement");
		});
		Test("NativeDeadlinesAloneDoNotImplyByteOnlyRecovery", [&] {
			const auto Value = Run({.GroupsPerPeer = 1280});
			const U Bound = pooled_recovery_contract::ConvergenceBoundUs(1280 * G, 32 * 1280 * G);
			Check(Value.Converged && !Value.CompletionFailed && !Value.BurstFailed &&
				Value.CapacityFailed && Value.FairnessFailed && Value.CompletedAt > Bound,
				"finite full-grant work can miss the inherited byte-only deadline at nominal cadence");
			// Legal single-frame shape plus count arithmetic is not a complete
			// source-history replay or a measured cessation quote. H3 already
			// fails in this schedule; this is not a jointly healthy H4 failure.
			Check(1280 * 9 < DefaultChangeJournalCapacity, "hypothetical nine-record group count fits the ceiling");
			std::cout << "[Network:HybridCapacity] finite_admission_schedule complete_us=" << Value.CompletedAt
				<< " bound_us=" << Bound << " peer_bytes=" << 1280 * G << " total_bytes=" << Value.Accepted
				<< " hypothetical_source_records=" << 1280 * 9 << '\n';
		});
	} catch (const std::exception &Error) {
		std::cerr << "[Network:HybridCapacity] FAIL " << Error.what() << '\n'; return false;
	}
	std::cout << "[Network:HybridCapacity] cases=" << Passed << " candidate_qualified=0\n";
	return Passed == 10;
}
}
}

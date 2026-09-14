#pragma once
#include "../src/network/ReliableByteAdmission.hpp"
#include "../src/network/PooledReliableServiceFeedback.hpp"
#include "PooledReliableServiceModelFixture.hpp"
#include <array>
#include <iostream>

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
		Test("OrdinarySplitBoundContainsEveryLegalHistory", [&] {
			for (std::uint64_t Total = 0; Total <= 512 * 1024; Total += 1024) {
				auto Bound = Ledger::FundedOrdinary(Total, 0, Total); Check(Bound.has_value(), "funding overflow");
				for (std::uint64_t Game = 0; Game <= Total; Game += 1024)
					Check(*Bound >= std::max(Game, PooledReliableServiceProfile::GameplayBurst) +
						std::max(Total - Game, PooledReliableServiceProfile::ControlBurst), "ordinary underfunding");
			}
		});
		Test("ThirtyTwoPeerReferenceDifferential", [&] {
			using namespace pooled_service_model;
			for (const bool Stalled : {false, true}) {
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
			std::cout << "[Network:PooledProduction] Differential stalled=" << Stalled << " completed=" << Completed
				<< " outstanding=" << Production.GetMetrics().OutstandingBytes << " logical_bytes=" << Production.LogicalBytes() << '\n';
			}
		});
	} catch (const std::exception &Error) {
		std::cerr << "[Network:PooledProduction] FAIL " << Error.what() << '\n'; return false;
	}
	std::cout << "[Network:PooledProduction] cases=" << Passed << " PASS\n";
	return Passed == 8;
}
}

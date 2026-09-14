#pragma once

#include "PooledReliableServiceModelFixture.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace gargantuan::test::pooled_service_model {
namespace hardening {

inline void Require(bool Condition, std::string_view Message) {
	if (!Condition) throw std::runtime_error(std::string(Message));
}

inline U OutstandingDebt(const Model &Value) {
	U Result = Value.Committed();
	Require(!addOv(Result, Value.GameDebt(), Result), "gameplay debt addition overflowed");
	Require(!addOv(Result, Value.ControlDebt(), Result), "control debt addition overflowed");
	return Result;
}

inline bool LegacyFullReservationValid(U Peers, U PeerRate, U AggregateRate, U BackendRate) {
	if (Peers == 0 || PeerRate == 0 || AggregateRate == 0 || BackendRate == 0) return false;
	U RequiredAggregate = 0;
	U RequiredBackend = 0;
	if (mulOv(Peers, PeerRate, RequiredAggregate) || RequiredAggregate > AggregateRate) return false;
	if (mulOv(PeerRate, U{2}, RequiredBackend) || RequiredBackend > BackendRate) return false;
	return true;
}

inline bool RunPooledReliableServiceProofHardeningTests() {
	std::size_t Passed = 0;
	auto Test = [&](const char *Name, auto Body) {
		try {
			Body();
			++Passed;
			std::cout << "[PooledServiceHardening] " << Name << "=pass\n";
		} catch (const std::exception &Error) {
			std::cerr << "[PooledServiceHardening] " << Name << "=FAIL " << Error.what() << '\n';
			throw;
		}
	};

	try {
		Test("FreshFeedbackAllowsAdmission", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Fresh(1, 1), "connect/fresh failed");
			Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "G offer failed");
			ModelValue.Advance(250'000);
			Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			auto ReservationValue = ModelValue.Reserve();
			Require(ReservationValue && ModelValue.Accept(*ReservationValue), "fresh feedback did not admit G");
			Require(ModelValue.Committed() == G && ModelValue.M().known == 1, "accepted G debt/Known mismatch");
		});

		Test("DelayedFeedbackWithinLimitAllowsAdmission", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Fresh(1, 1), "connect/fresh failed");
			Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "G offer failed");
			ModelValue.Advance(250'000);
			Require(!ModelValue.Reserve(), "stale feedback unexpectedly admitted");
			Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			ModelValue.Advance(40'000);
			auto ReservationValue = ModelValue.Reserve();
			Require(ReservationValue && ModelValue.Accept(*ReservationValue), "40 ms delayed feedback did not admit");
			Require(ModelValue.Committed() == G, "delayed-feedback admission debt mismatch");
		});

		Test("DebtConservation", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Fresh(1, 1), "connect/fresh failed");
			Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "G offer failed");
			ModelValue.Advance(250'000);
			Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			auto ReservationValue = ModelValue.Reserve();
			Require(ReservationValue && ModelValue.Accept(*ReservationValue), "accept failed");
			Require(ModelValue.Gameplay(1, 1, 20 * KiB), "gameplay debt offer failed");
			Require(ModelValue.Control(1, 1, 32 * KiB), "control debt offer failed");

			U CreatedDebt = G + 20 * KiB + 32 * KiB;
			U VerifiedDrainedDebt = 0;
			U TerminalReleasedDebt = 0;
			Require(CreatedDebt == OutstandingDebt(ModelValue), "initial debt equation failed");

			ModelValue.Advance(20'000);
			Require(CreatedDebt == OutstandingDebt(ModelValue), "elapsed time erased physical debt");

			const U BeforeService = OutstandingDebt(ModelValue);
			ModelValue.Service(10'000);
			const U AfterService = OutstandingDebt(ModelValue);
			Require(AfterService <= BeforeService, "verified service increased debt");
			VerifiedDrainedDebt += BeforeService - AfterService;
			Require(CreatedDebt == VerifiedDrainedDebt + TerminalReleasedDebt + AfterService,
				"verified-drain debt equation failed");

			Require(ModelValue.Disconnect(1, 1), "disconnect failed");
			const U BeforeTeardown = OutstandingDebt(ModelValue);
			Require(BeforeTeardown == AfterService, "disconnect silently released accepted debt");
			Require(ModelValue.FinishTeardown(1, 1), "terminal teardown failed");
			TerminalReleasedDebt += BeforeTeardown;
			Require(OutstandingDebt(ModelValue) == 0, "terminal teardown left debt");
			Require(CreatedDebt == VerifiedDrainedDebt + TerminalReleasedDebt,
				"terminal debt-conservation equation failed");
			std::cout << "[PooledServiceHardening] DebtConservation created=" << CreatedDebt
				<< " verified_drained=" << VerifiedDrainedDebt
				<< " terminal_released=" << TerminalReleasedDebt << '\n';
		});

		Test("SlowPeerLosesFurtherGrantEligibility", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Fresh(1, 1), "connect/fresh failed");
			Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "first G offer failed");
			ModelValue.Advance(250'000);
			Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			auto ReservationValue = ModelValue.Reserve();
			Require(ReservationValue && ModelValue.Accept(*ReservationValue), "first grant failed");
			Require(ModelValue.DrainRate(1, 1, 1 * MiB), "slow-rate injection failed");
			while (ModelValue.Committed() != 0) ModelValue.Service(StepUs);
			Require(ModelValue.OfferStructural(1, 1, 4 * KiB) == Offer::Queued, "second offer failed");
			ModelValue.Advance(10'000);
			Require(ModelValue.Fresh(1, 1), "slow feedback refresh failed");
			Require(!ModelValue.Reserve(), "below-floor slow peer received another grant");
			Require(ModelValue.Pending() == 4 * KiB && ModelValue.Committed() == 0,
				"slow-peer deferral did not retain bounded pending work");
		});

		Test("MixedSmallLargeFairness", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Connect(2, 1), "connect failed");
			Require(ModelValue.Fresh(1, 1) && ModelValue.Fresh(2, 1), "fresh failed");
			for (int Round = 0; Round < 6; ++Round) {
				const U LargeBefore = ModelValue.At(1).completions;
				const U TinyBefore = ModelValue.At(2).completions;
				Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "large offer failed");
				Require(ModelValue.OfferStructural(2, 1, 4 * KiB) == Offer::Queued, "tiny offer failed");
				const U Deadline = ModelValue.Now() + 600'000;
				while ((ModelValue.At(1).completions == LargeBefore || ModelValue.At(2).completions == TinyBefore) &&
					ModelValue.Now() < Deadline) Healthy(ModelValue);
				Require(ModelValue.At(1).completions > LargeBefore, "large group starved");
				Require(ModelValue.At(2).completions > TinyBefore, "tiny group starved");
			}
			Require(ModelValue.At(1).completions == 6 && ModelValue.At(2).completions == 6,
				"mixed fairness completion count mismatch");
		});

		Test("GameplayBurstAfterAdmission", [] {
			Model ModelValue;
			Require(ModelValue.Connect(1, 1) && ModelValue.Fresh(1, 1), "connect/fresh failed");
			Require(ModelValue.OfferStructural(1, 1, G) == Offer::Queued, "G offer failed");
			ModelValue.Advance(250'000);
			Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			auto ReservationValue = ModelValue.Reserve();
			Require(ReservationValue && ModelValue.Accept(*ReservationValue), "G admission failed");
			Require(ModelValue.Gameplay(1, 1, 20 * KiB), "post-admission gameplay burst failed");
			while (ModelValue.GameDebt() != 0) {
				ModelValue.Service(StepUs);
				Require(ModelValue.Fresh(1, 1), "feedback refresh failed");
			}
			Require(ModelValue.M().maxGameDelay <= 33'000, "same-FIFO gameplay delay exceeded model bound");
			Require(ModelValue.M().maxGameDelay + 100'000 <= 150'000,
				"same-FIFO gameplay exceeded accepted p95 envelope");
		});

		Test("FullReservationCompatibility", [] {
			constexpr U Peers = 32;
			constexpr U PeerRate = 8 * MiB;
			constexpr U AggregateRate = 256 * MiB;
			constexpr U BackendRate = 16 * MiB;
			Require(LegacyFullReservationValid(Peers, PeerRate, AggregateRate, BackendRate),
				"unchanged full-reservation arithmetic rejected");
			Require(!LegacyFullReservationValid(Peers, PeerRate, AggregateRate - 1, BackendRate),
				"underfunded full-reservation aggregate accepted");
			Require(!LegacyFullReservationValid(Peers, PeerRate, AggregateRate, BackendRate - 1),
				"underfunded full-reservation backend accepted");
			Profile PooledValidator;
			PooledValidator.mode = Mode::FullReservation;
			Require(!Valid(PooledValidator), "pooled validator silently reinterpreted full-reservation mode");
		});

		Test("ThirtyTwoMaximumGroupsFirstServiceReport", [] {
			Model ModelValue;
			All(ModelValue);
			for (std::uint32_t Slot = 1; Slot <= 32; ++Slot)
				Require(ModelValue.OfferStructural(Slot, 1, G) == Offer::Queued, "G wave offer failed");
			ModelValue.Advance(250'000);
			FreshAll(ModelValue);
			Admit(ModelValue);
			while (!ModelValue.Converged()) Healthy(ModelValue);

			std::array<U, 32> FirstService{};
			for (std::uint32_t Slot = 1; Slot <= 32; ++Slot) {
				Require(ModelValue.At(Slot).firstService.has_value(), "peer never received structural service");
				FirstService[Slot - 1] = *ModelValue.At(Slot).firstService;
			}
			const U Maximum = *std::max_element(FirstService.begin(), FirstService.end());
			Require(Maximum <= 471'000, "first-service opportunity exceeded finite fairness bound");
			Require(ModelValue.M().maxCommitted == 4 * G, "committed debt exceeded MaxConcurrentDrainDebt");
			Require(ModelValue.M().maxPending == 32 * G, "pending high-water exceeded global pending cap");
			std::cout << "[PooledServiceHardening] ThirtyTwoMaximumGroupsFirstServiceReport first_service_us=";
			for (std::size_t Index = 0; Index < FirstService.size(); ++Index) {
				if (Index != 0) std::cout << ',';
				std::cout << FirstService[Index];
			}
			std::cout << " max=" << Maximum << " committed_high=" << ModelValue.M().maxCommitted
				<< " pending_high=" << ModelValue.M().maxPending << '\n';
		});

		Test("AggregateDebtInvariantIncludesTransportReserve", [] {
			Model ModelValue;
			All(ModelValue);
			for (std::uint32_t Slot = 1; Slot <= 5; ++Slot)
				Require(ModelValue.OfferStructural(Slot, 1, G) == Offer::Queued, "G offer failed");
			ModelValue.Advance(250'000);
			FreshAll(ModelValue);
			Admit(ModelValue);
			Require(ModelValue.Grants() == 4 && ModelValue.Committed() == 4 * G,
				"four-grant structural debt bound not reached exactly");
			Require(ModelValue.Invariant(), "funded aggregate invariant rejected admitted debt");
			Require(!ModelValue.Reserve(), "fifth concurrent drain grant was admitted");
			Require(ModelValue.At(5).credit == G && ModelValue.Pending() == 5 * G,
				"credit/service separation or pending accounting mismatch");
		});
	} catch (...) {
		return false;
	}

	std::cout << "[PooledServiceHardening] complete tests=" << Passed
		<< " MaxConcurrentDrainDebt=" << 4 * G
		<< " MaxConcurrentDrainGrants=4\n";
	return Passed == 9;
}

} // namespace hardening
} // namespace gargantuan::test::pooled_service_model

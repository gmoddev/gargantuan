#pragma once

#include "PooledReliableServiceModelFixture.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace gargantuan::test::pooled_recovery_contract {
	using U = std::uint64_t;

	inline constexpr U KiB = 1024;
	inline constexpr U MiB = 1024 * KiB;
	inline constexpr U GiB = 1024 * MiB;
	inline constexpr U AdapterEnvelopeBytes = 32;
	inline constexpr U CompleteGroupBytes = pooled_service_model::G;
	inline constexpr U PeerCreditRate = 2 * MiB;
	inline constexpr U StructuralPool = 64 * MiB;
	inline constexpr U PeerGameplayBurst = 20 * KiB;
	inline constexpr U PeerDrainFloor = 16 * MiB;
	inline constexpr U ServiceRecoveryDeadlineUs = 20'000'000;
	inline constexpr U MaximumFirstGrantFairnessUs = 220'500;
	inline constexpr U MaximumFreshCreditWarmupUs = 250'000;

	constexpr U CeilDiv(U Numerator, U Denominator) {
		return Numerator / Denominator + (Numerator % Denominator != 0);
	}

	constexpr U ServiceUs(U Bytes, U BytesPerSecond) {
		return CeilDiv(Bytes * 1'000'000, BytesPerSecond);
	}

	constexpr U ConvergenceBoundUs(U MaximumPeerWork, U AggregateWork) {
		const U PeerBound = MaximumFreshCreditWarmupUs + MaximumFirstGrantFairnessUs +
			ServiceUs(MaximumPeerWork, PeerCreditRate);
		const U AggregateBound = ServiceUs(AggregateWork, StructuralPool);
		return ServiceRecoveryDeadlineUs + std::max(PeerBound, AggregateBound);
	}

	inline bool RunPooledReliableServiceRecoveryContractTests() {
		int Failures = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) {
				std::cerr << "[PooledRecoveryContract] FAIL: " << Message << '\n';
				++Failures;
			}
		};

		static_assert(DefaultChangeJournalCapacity == 16'384);
		static_assert(network::MaximumPeerDesiredObjects == 65'536);
		static_assert(network::MaximumStructuralPendingTransitions == 1'048'576);
		static_assert(CompleteGroupBytes == 512 * KiB);
		static_assert(32 * PeerCreditRate == StructuralPool);

		// The pooled admission model bounds only the next serialized offer/debt.
		// Current 3J semantics independently retain bounded semantic work in the
		// per-peer transition set and in the authoritative journal window.
		constexpr U WorstPeerJournalWork = U(DefaultChangeJournalCapacity) * CompleteGroupBytes;
		constexpr U WorstPeerTransitionWork = U(network::MaximumPeerDesiredObjects) * CompleteGroupBytes;
		constexpr U WorstPeerRetainedWork = WorstPeerJournalWork + WorstPeerTransitionWork;
		constexpr U WorstGlobalJournalWork = U(DefaultChangeJournalCapacity) * 32 * CompleteGroupBytes;
		constexpr U WorstGlobalTransitionWork = U(network::MaximumStructuralPendingTransitions) * CompleteGroupBytes;
		constexpr U WorstGlobalRetainedWork = WorstGlobalJournalWork + WorstGlobalTransitionWork;
		constexpr U WorstCaseBound = ConvergenceBoundUs(WorstPeerRetainedWork, WorstGlobalRetainedWork);

		Check(WorstPeerJournalWork == 8 * GiB, "journal-window service bound is 8 GiB per peer");
		Check(WorstPeerTransitionWork == 32 * GiB, "pending-transition service bound is 32 GiB per peer");
		Check(WorstPeerRetainedWork == 40 * GiB, "combined retained service bound is 40 GiB per peer");
		Check(WorstGlobalJournalWork == 256 * GiB, "journal fanout service bound is 256 GiB at 32 peers");
		Check(WorstGlobalTransitionWork == 512 * GiB, "global pending-transition service bound is 512 GiB");
		Check(WorstGlobalRetainedWork == 768 * GiB, "combined aggregate retained service bound is 768 GiB");
		Check(ServiceUs(WorstPeerRetainedWork, PeerCreditRate) == 20'480'000'000ULL,
			"peer credit makes the hard retained-work bound multi-hour rather than 20 seconds");
		Check(ServiceUs(WorstGlobalRetainedWork, StructuralPool) == 12'288'000'000ULL,
			"aggregate structural pool independently bounds total retained service");
		Check(WorstCaseBound > ServiceRecoveryDeadlineUs,
			"fixed service recovery cannot also be the hard complete-convergence deadline");

		// Canonical workload: 480 opportunities * 16 already-known objects *
		// 24 KiB Name values = exactly 180 MiB of raw journal value history.
		constexpr U Opportunities = 480;
		constexpr U Objects = 16;
		constexpr U NameBytes = 24 * KiB;
		constexpr U CanonicalRecords = Opportunities * Objects;
		constexpr U CanonicalRawBytes = CanonicalRecords * NameBytes;
		constexpr U CanonicalRemainingRecords = 7'248;
		constexpr U CanonicalRemainingBytes = CanonicalRemainingRecords * NameBytes;
		Check(CanonicalRecords == 7'680 && CanonicalRecords < DefaultChangeJournalCapacity,
			"canonical overload is finite and remains within the retained journal window");
		Check(CanonicalRawBytes == 180 * MiB,
			"canonical workload raw value history is exactly 180 MiB");
		Check(ServiceUs(CanonicalRawBytes, PeerCreditRate) == 90'000'000,
			"raw canonical history requires 90 seconds at the accepted peer credit rate");
		Check(ServiceUs(CanonicalRemainingBytes, PeerCreditRate) == 84'937'500,
			"the measured 7,248-record remainder cannot drain in the old 20-second deadline");

		// Existing 3J semantics permit ordinary current-state property projection.
		// For this Name-only workload, one final update per object is semantically
		// sufficient. Encode and decode that exact final-state representation
		// through the real GRPL codec so this proves semantic convergence rather
		// than only a smaller queue.
		network::ReplicationFrame FinalState{
			.Version = network::ReplicationProtocolVersion,
			.Kind = network::ReplicationMessageKind::Incremental,
			.Epoch = network::ReplicationEpoch(1),
			.Sequence = network::ReliableReplicationSequence(1),
		};
		FinalState.Operations.reserve(Objects);
		for (U Index = 0; Index < Objects; ++Index) {
			network::PropertyReplicationUpdate Update{
				.Object = ObjectId{static_cast<std::uint32_t>(Index + 1), 1},
				.PropertyName = "Name",
				.Value = std::string(NameBytes, static_cast<char>('a' + (Index % 26))),
			};
			FinalState.Operations.push_back({FinalState.Epoch, std::move(Update)});
		}
		auto Encoded = network::EncodeReplicationFrame(FinalState);
		Check(static_cast<bool>(Encoded), "canonical coalesced final-state frame encodes");
		U CoalescedCompleteBytes = std::numeric_limits<U>::max();
		if (Encoded) {
			CoalescedCompleteBytes = static_cast<U>(Encoded->size()) + AdapterEnvelopeBytes;
			Check(CoalescedCompleteBytes <= CompleteGroupBytes,
				"canonical coalesced final-state frame fits one accepted complete group");
			Check(ServiceUs(CoalescedCompleteBytes, PeerCreditRate) < 1'000'000,
				"canonical semantically necessary final state drains within one second of peer credit");
			auto Decoded = network::DecodeReplicationFrame(*Encoded);
			Check(static_cast<bool>(Decoded), "canonical coalesced final-state frame decodes");
			if (Decoded) {
				Check(Decoded->Operations.size() == Objects,
					"canonical decoded final-state frame contains one update per object");
				for (U Index = 0; Index < Decoded->Operations.size(); ++Index) {
					const auto *Update = std::get_if<network::PropertyReplicationUpdate>(&Decoded->Operations[Index].Intent);
					Check(Update != nullptr, "canonical decoded operation remains a property update");
					if (!Update) continue;
					Check(Update->Object == ObjectId{static_cast<std::uint32_t>(Index + 1), 1},
						"canonical decoded property update retains the exact object identity");
					Check(Update->PropertyName == "Name",
						"canonical decoded property update retains Name semantics");
					const auto *Value = std::get_if<std::string>(&Update->Value);
					const std::string Expected(NameBytes, static_cast<char>('a' + (Index % 26)));
					Check(Value && *Value == Expected,
						"canonical decoded property update retains the exact current final value");
				}
			}
		}

		U CanonicalRecoveryUs = 0;
		U FairRecoveryUs = 0;
		U FairRecoveryGameplayDelayUs = 0;
		U FairRecoveryGrantGapUs = 0;
		if (CoalescedCompleteBytes <= CompleteGroupBytes) {
			// Demand has stopped; this is the finite semantic remainder for one
			// canonical peer. The accepted admission model must converge it within
			// the workload-derived bound, while pending/committed debt stays bounded.
			pooled_service_model::Model Canonical;
			Check(Canonical.Connect(1, 1) && Canonical.Fresh(1, 1),
				"canonical recovery peer connects with fresh feedback");
			const U CanonicalStop = Canonical.Now();
			Check(Canonical.OfferStructural(1, 1, CoalescedCompleteBytes) == pooled_service_model::Offer::Queued,
				"canonical semantic remainder enters bounded pooled admission");
			while (!Canonical.Converged()) {
				pooled_service_model::Healthy(Canonical);
				Check(Canonical.Pending() <= CompleteGroupBytes && Canonical.Committed() <= 4 * CompleteGroupBytes,
					"canonical recovery preserves pending and committed debt bounds");
				if (Canonical.Now() - CanonicalStop > ConvergenceBoundUs(CoalescedCompleteBytes, CoalescedCompleteBytes)) break;
			}
			CanonicalRecoveryUs = Canonical.Now() - CanonicalStop;
			Check(Canonical.Converged(), "canonical semantic remainder converges after demand cessation");
			Check(CanonicalRecoveryUs <= ConvergenceBoundUs(CoalescedCompleteBytes, CoalescedCompleteBytes),
				"canonical convergence stays inside its workload-derived bound");

			// Four retained groups per peer model finite semantic overload after
			// offered demand stops. RetainedCounts is proof-local workload input,
			// not a proposed production queue. The accepted Option C model owns all
			// actual pending/reserved/committed admission state.
			pooled_service_model::Model Fair;
			pooled_service_model::All(Fair);
			std::array<unsigned, 32> RetainedCounts{};
			RetainedCounts.fill(4);
			constexpr U FairPeerWork = 4 * CompleteGroupBytes;
			constexpr U FairAggregateWork = 32 * FairPeerWork;
			const U FairStop = Fair.Now();
			U NextGameplayUs = 100'000;
			while (true) {
				bool AnyRetained = false;
				for (std::uint32_t Slot = 1; Slot <= 32; ++Slot) {
					auto &Remaining = RetainedCounts[Slot - 1];
					if (!Remaining) continue;
					AnyRetained = true;
					const auto &Peer = Fair.At(Slot);
					if (!Peer.pending && !Peer.reserved && !Peer.grant &&
						Fair.OfferStructural(Slot, 1, CompleteGroupBytes) == pooled_service_model::Offer::Queued)
						--Remaining;
				}
				pooled_service_model::FreshAll(Fair);
				pooled_service_model::Admit(Fair);
				if (Fair.Now() >= NextGameplayUs) {
					for (std::uint32_t Slot = 1; Slot <= 8; ++Slot)
						(void)Fair.Gameplay(Slot, 1, PeerGameplayBurst);
					NextGameplayUs += 100'000;
				}
				pooled_service_model::Healthy(Fair);
				if (!AnyRetained && Fair.Converged() && !Fair.GameDebt() && !Fair.ControlDebt()) break;
				if (Fair.Now() - FairStop > ConvergenceBoundUs(FairPeerWork, FairAggregateWork)) break;
			}
			FairRecoveryUs = Fair.Now() - FairStop;
			FairRecoveryGameplayDelayUs = Fair.M().maxGameDelay;
			FairRecoveryGrantGapUs = Fair.M().maxGrantGap;
			Check(Fair.Converged() && !Fair.GameDebt() && !Fair.ControlDebt(),
				"32-peer retained semantic workload converges after demand cessation");
			Check(FairRecoveryUs <= ConvergenceBoundUs(FairPeerWork, FairAggregateWork),
				"32-peer recovery remains inside the workload-derived convergence bound");
			Check(Fair.M().maxPending <= 32 * CompleteGroupBytes && Fair.M().maxCommitted <= 4 * CompleteGroupBytes,
				"32-peer recovery keeps pooled admission state bounded");
			for (std::uint32_t Slot = 1; Slot <= 32; ++Slot)
				Check(Fair.At(Slot).completions == 4,
					"every conforming peer receives all retained structural service opportunities");
			Check(FairRecoveryGrantGapUs <= MaximumFreshCreditWarmupUs + MaximumFirstGrantFairnessUs + 50'000,
				"recovery grant rotation preserves the accepted credit-plus-fairness envelope");
			Check(FairRecoveryGameplayDelayUs + 100'000 <= 150'000,
				"qualified gameplay remains inside the accepted nonqueue plus RPC p95 envelope during recovery");

			// Repeated finite overload/recovery cycles must not accumulate admission
			// debt or deprive any peer. Each cycle begins only after the preceding
			// semantic remainder has converged.
			pooled_service_model::Model Cycles;
			pooled_service_model::All(Cycles);
			for (int Cycle = 0; Cycle < 8; ++Cycle) {
				for (std::uint32_t Slot = 1; Slot <= 32; ++Slot)
					Check(Cycles.OfferStructural(Slot, 1, CompleteGroupBytes) == pooled_service_model::Offer::Queued,
						"repeated recovery cycle admits one bounded semantic group per peer");
				while (!Cycles.Converged()) pooled_service_model::Healthy(Cycles);
				Check(!Cycles.Pending() && !Cycles.Reserved() && !Cycles.Committed() && !Cycles.Grants(),
					"repeated recovery cycle leaves no pooled admission carryover");
			}
			for (std::uint32_t Slot = 1; Slot <= 32; ++Slot)
				Check(Cycles.At(Slot).completions == 8,
					"repeated recovery cycles preserve finite service opportunity for every peer");
		}

		// The old gameplay/FIFO proof remains independent of historical retained
		// work because no peer may own more than one accepted structural grant.
		const U GameplayFollowerUs = ServiceUs(CompleteGroupBytes + PeerGameplayBurst, PeerDrainFloor);
		Check(GameplayFollowerUs <= 50'000,
			"one admitted complete group plus qualified gameplay burst stays inside the funded queue window");

		std::cout << "[PooledRecoveryContract] canonical_raw_bytes=" << CanonicalRawBytes
			<< " canonical_remaining_bytes=" << CanonicalRemainingBytes
			<< " canonical_coalesced_complete_bytes=" << CoalescedCompleteBytes
			<< " canonical_recovery_us=" << CanonicalRecoveryUs
			<< " fair_recovery_us=" << FairRecoveryUs
			<< " fair_gameplay_delay_us=" << FairRecoveryGameplayDelayUs
			<< " fair_grant_gap_us=" << FairRecoveryGrantGapUs
			<< " worst_peer_retained_bytes=" << WorstPeerRetainedWork
			<< " worst_global_retained_bytes=" << WorstGlobalRetainedWork
			<< " worst_convergence_bound_us=" << WorstCaseBound
			<< " gameplay_follower_us=" << GameplayFollowerUs << '\n';
		std::cout << "[PooledRecoveryContract] " << (Failures == 0 ? "PASS" : "FAIL") << '\n';
		return Failures == 0;
	}
}

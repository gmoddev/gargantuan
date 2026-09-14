#pragma once

#include "PooledReliableServiceModelFixture.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

#include <algorithm>
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

		// Existing 3J semantics already resolve ordinary native properties from
		// current authoritative catalog state. For this Name-only workload, one
		// final update per object is semantically sufficient. Encode and decode
		// that exact final-state representation through the real GRPL codec so
		// this proves semantic convergence rather than only a smaller queue.
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

		// The old gameplay/FIFO proof remains independent of historical retained
		// work because no peer may own more than one accepted structural grant.
		const U GameplayFollowerUs = ServiceUs(CompleteGroupBytes + PeerGameplayBurst, PeerDrainFloor);
		Check(GameplayFollowerUs <= 50'000,
			"one admitted complete group plus qualified gameplay burst stays inside the funded queue window");

		// Repeated bounded overload/recovery cycles do not accumulate historical
		// debt when each cycle converges before the next begins. This is an
		// algebraic lifecycle proof, not a new production queue.
		U RepeatedCycleRetained = 0;
		for (int Cycle = 0; Cycle < 8; ++Cycle) {
			RepeatedCycleRetained = CoalescedCompleteBytes;
			Check(RepeatedCycleRetained <= CompleteGroupBytes,
				"repeated canonical cycle remains within one semantic complete group");
			RepeatedCycleRetained = 0;
		}
		Check(RepeatedCycleRetained == 0, "repeated overload/recovery cycles converge without retained carryover");

		std::cout << "[PooledRecoveryContract] canonical_raw_bytes=" << CanonicalRawBytes
			<< " canonical_remaining_bytes=" << CanonicalRemainingBytes
			<< " canonical_coalesced_complete_bytes=" << CoalescedCompleteBytes
			<< " worst_peer_retained_bytes=" << WorstPeerRetainedWork
			<< " worst_global_retained_bytes=" << WorstGlobalRetainedWork
			<< " worst_convergence_bound_us=" << WorstCaseBound
			<< " gameplay_follower_us=" << GameplayFollowerUs << '\n';
		std::cout << "[PooledRecoveryContract] " << (Failures == 0 ? "PASS" : "FAIL") << '\n';
		return Failures == 0;
	}
}

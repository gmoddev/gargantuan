#pragma once

#include "PooledReliableServiceModelFixture.hpp"
#include "../src/network/FiniteGrantServiceCurve.hpp"
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

	// Retained numerical canonical outcome. The new turnover class does not
	// imply this formula for every legal group shape or live relevance history.
	constexpr U ConvergenceBoundUs(U MaximumPeerWork, U AggregateWork) {
		const U PeerBound = MaximumFreshCreditWarmupUs + MaximumFirstGrantFairnessUs +
			ServiceUs(MaximumPeerWork, PeerCreditRate);
		const U AggregateBound = ServiceUs(AggregateWork, StructuralPool);
		return ServiceRecoveryDeadlineUs + std::max(PeerBound, AggregateBound);
	}

	// Proposed product-class oracle, not a production health implementation.
	// The combined turnover budget is newly selected from the existing 50-ms
	// queue plus 100-ms nonqueue product allocation. It is not an inferred ACK
	// guarantee. A completed opportunity includes reconciliation AND admission.
	namespace ProductStructuralClass {
		inline constexpr U TurnoverBudgetUs = 150'000;
		inline constexpr U OpportunityBudgetUs = 100'000;
		inline constexpr U DrainSlots = 4;
		inline constexpr U GlobalWarmupUs = ServiceUs(4 * CompleteGroupBytes, StructuralPool);
		inline constexpr U ReadyFairWaitUs = OpportunityBudgetUs + GlobalWarmupUs +
			(32 / DrainSlots - 1) * TurnoverBudgetUs;
		inline constexpr U NativeBudgetNumerator = PeerDrainFloor * 6'000 +
			(CompleteGroupBytes + 1'248) * 1'000'000;

		struct Turnover {
			U Activated = 0, NativeCompleted = 0, Acked = 0, MainRetired = 0, OpportunityCompleted = 0;
			U Bytes = 0, NativeBytes = 0, AckedBytes = 0, RetiredBytes = 0;
			bool EvidenceComplete = true;
		};
		inline bool CompletedTurnoverFits(const Turnover &Value) {
			if (!Value.EvidenceComplete || !Value.Activated || !Value.Bytes || Value.Bytes > CompleteGroupBytes ||
				Value.NativeBytes != Value.Bytes || Value.AckedBytes != Value.Bytes || Value.RetiredBytes != Value.Bytes ||
				Value.NativeCompleted < Value.Activated || Value.Acked < Value.NativeCompleted ||
				Value.MainRetired < Value.Acked || Value.OpportunityCompleted < Value.MainRetired)
				return false;
			const U NativeElapsed = Value.NativeCompleted - Value.Activated;
			return NativeElapsed <= std::numeric_limits<U>::max() / PeerDrainFloor &&
				NativeElapsed * PeerDrainFloor <= NativeBudgetNumerator &&
				Value.OpportunityCompleted - Value.Activated <= TurnoverBudgetUs;
		}
		inline bool CompletedOpportunityFits(U PreviousCompleted, U Completed) {
			return Completed != 0 && Completed >= PreviousCompleted &&
				Completed - PreviousCompleted <= OpportunityBudgetUs;
		}
		constexpr U FirstAdmissionBoundUs(U Rank) {
			return OpportunityBudgetUs + GlobalWarmupUs + (Rank / DrainSlots) * TurnoverBudgetUs;
		}
	}

	// Hypothetical product-policy review only. These supplied schedules do not
	// change D03 production health, establish host timing, or qualify a source.
	// Keep its existing 32-peer fair outcome as a direct entitlement, not a
	// derivation from a 100-ms Main opportunity and seven 150-ms slot lifetimes.
	namespace ProductProgressReview {
		inline constexpr U FairWaitUs = ProductStructuralClass::ReadyFairWaitUs;
		struct Episode {
			// Symbolic oldest qualifying unit and NoFrame fence are supplied by
			// this review. This is not an implementation of 3J unit identification,
			// relevance cancellation, current-state coverage or exact byte quoting.
			U SourceReadyAt = 0, CreditReadyAt = 0, Frontier = 0, OldestUnit = 0;
			U CoveredThrough = 0, ResolvedAt = 0, LastObservedAt = 0;
			bool Invalid = false, Failed = false;
			bool PartialNoFrame(U Through) {
				if (Through < CoveredThrough) { Invalid = true; return false; }
				CoveredThrough = Through;
				return true; // Cursor advance is not complete source discharge.
			}
			bool NoFrame(U At, U Through, bool RelevanceComplete) {
				if (ResolvedAt || !At || At < SourceReadyAt || At < LastObservedAt || !PartialNoFrame(Through) ||
					Through < Frontier || !RelevanceComplete) return false;
				ResolvedAt = At; return true;
			}
			bool Accepted(U At, U CompleteBytes, U AcceptedUnit) {
				if (!OldestUnit || AcceptedUnit != OldestUnit || !CompleteBytes ||
					CompleteBytes > CompleteGroupBytes || ResolvedAt || At < CreditReadyAt || At < LastObservedAt) return false;
				ResolvedAt = At; return true; // Caller supplies an actual complete scheduler acceptance.
			}
			bool Fits(U ObservedAt) {
				if (Invalid || !SourceReadyAt || CreditReadyAt < SourceReadyAt ||
					CreditReadyAt - SourceReadyAt > MaximumFreshCreditWarmupUs || ObservedAt < LastObservedAt) {
					Invalid = true; return false;
				}
				LastObservedAt = ObservedAt;
				const U End = ResolvedAt ? ResolvedAt : ObservedAt;
				if (End < SourceReadyAt || End > ObservedAt) { Invalid = true; return false; }
				Failed |= End >= CreditReadyAt && End - CreditReadyAt > FairWaitUs;
				return !Failed;
			}
		};
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
		{
			using Legacy = network::FiniteGrantServiceCurve;
			using Native = network::NativeGrantCompletionCurve;
			using namespace ProductStructuralClass;
			constexpr U Epoch = 1'000'000, Bytes = 65'536;
			Legacy Curve; Native Completion;
			Check(Curve.Activate(1, Bytes, Epoch) && Completion.Activate(1, Bytes, Epoch) &&
				Curve.FirstSend(1, Bytes, Epoch + 100) && Completion.FirstSend(1, Bytes, Epoch + 100),
				"D03 counterexamples begin with truthful native BODY and healthy legacy packet service");
			Check(!Curve.GrantFailed && Curve.MaximumRunningDeficitByteMicroseconds == 0,
				"completed grants stop the legacy running clock without claiming future product progress");
			const Turnover Timely{Epoch, Epoch + 100, Epoch + 1'000, Epoch + 2'000, Epoch + 3'000,
				Bytes, Bytes, Bytes, Bytes};
			Check(CompletedTurnoverFits(Timely), "timely exact native/ACK/retirement baseline");
			auto AckHeld = Timely;
			AckHeld.Acked = Epoch + 149'000; AckHeld.MainRetired = Epoch + 150'000;
			AckHeld.OpportunityCompleted = Epoch + 150'001;
			Check(Curve.Observe(Epoch + 150'001) && !CompletedTurnoverFits(AckHeld),
				"healthy legacy packet service cannot hide 150001-us exact permit turnover");
			Check(Curve.Observe(Epoch + 200'001) && !CompletedOpportunityFits(Epoch + 100'000, Epoch + 200'001),
				"idle native curve cannot hide a 100001-us completed Main opportunity gap with ready source work");
			std::array<U, 32> FirstAdmission{};
			for (U Peer = 0; Peer < 4; ++Peer) FirstAdmission[Peer] = Epoch;
			// Repeatedly servicing these same four peers leaves the other 28
			// independently Ready peers unaccepted, regardless of their packet gaps.
			Check(Curve.Observe(Epoch + ReadyFairWaitUs + 1) &&
				!std::ranges::all_of(FirstAdmission, [&](U At) { return At >= Epoch && At - Epoch <= ReadyFairWaitUs; }),
				"acceptable packet gaps on admitted peers cannot certify 32-peer fair admission");
			constexpr U OrdinaryAccepted = Epoch + 200'000, OrdinaryCompleted = OrdinaryAccepted + 50'001;
			Legacy FifoLegacy;
			Check(FifoLegacy.Activate(1, Bytes, Epoch) && FifoLegacy.FirstSend(1, Bytes, Epoch + 100) &&
				FifoLegacy.Observe(OrdinaryCompleted) && OrdinaryCompleted - OrdinaryAccepted > 50'000,
				"native structural legacy PASS cannot grant another 50-ms FIFO allowance to an accepted ordinary follower");
			Legacy DelayedFirst; Native LateNative;
			Check(DelayedFirst.Activate(1, Bytes, Epoch) && LateNative.Activate(1, Bytes, Epoch), "late first-send activation");
			(void)DelayedFirst.FirstSend(1, Bytes, Epoch + 37'325);
			Check(!LateNative.FirstSend(1, Bytes, Epoch + 37'325) && LateNative.GrantFailed &&
				DelayedFirst.MaximumRunningDeficitByteMicroseconds == 0 && DelayedFirst.HasFirstSend,
				"zero legacy running gap cannot hide a missed 37324.3865966796875-us native completion");
			Legacy RecoveryLegacy;
			Check(RecoveryLegacy.Activate(1, Bytes, Epoch) && RecoveryLegacy.FirstSend(1, Bytes, Epoch + 100) &&
				RecoveryLegacy.Observe(Epoch + ServiceRecoveryDeadlineUs + 1), "healthy accepted-grant packet chronology in recovery");
			const U Retained = MiB, Required = ConvergenceBoundUs(Retained, 32 * Retained);
			const U SourceProduced = Retained - 1, SourceFinished = Epoch + Required + 1;
			Check(SourceProduced < Retained && SourceFinished - Epoch > Required &&
				ServiceRecoveryDeadlineUs + 1 > ServiceRecoveryDeadlineUs,
				"prompt accepted-grant service cannot hide unproduced immutable recovery bytes or a missed service-return deadline");
			Legacy Diagnostic; Native Hybrid;
			Check(Diagnostic.Activate(1, Bytes, Epoch) && Hybrid.Activate(1, Bytes, Epoch) &&
				Diagnostic.FirstSend(1, 100, Epoch + 100) && Hybrid.FirstSend(1, 100, Epoch + 100), "same positive native prefix");
			(void)Diagnostic.FirstSend(1, Bytes - 100, Epoch + 5'000);
			const Turnover HybridTurn{Epoch, Epoch + 5'000, Epoch + 6'000, Epoch + 7'000, Epoch + 8'000,
				Bytes, Bytes, Bytes, Bytes};
			Check(Diagnostic.GrantFailed && Hybrid.FirstSend(1, Bytes - 100, Epoch + 5'000) &&
				CompletedTurnoverFits(HybridTurn) && CompletedOpportunityFits(Epoch, Epoch + 8'000),
				"legacy running failure alone does not invalidate exact timely D03 native/ACK/retirement/opportunity service");
			for (U Peer = 0; Peer < 32; ++Peer) FirstAdmission[Peer] = Epoch + (Peer / 4) * 8'000;
			Check(std::ranges::all_of(FirstAdmission, [&](U At) { return At >= Epoch && At - Epoch <= ReadyFairWaitUs; }) &&
				(Epoch + 9'000) - (Epoch + 5'000) <= 50'000 &&
				8 * 8'000 <= ServiceRecoveryDeadlineUs && 8 * 8'000 <= ConvergenceBoundUs(Bytes, 32 * Bytes),
				"otherwise timely supplied HYBRID schedule retains direct FIFO, all32 admission and independent recovery budgets");
			std::cout << "[PooledRecoveryContract] contract=HYBRID_F1_V1 independent_legacy_gap_counterexamples=PASS\n";
		}
		{
			using namespace ProductProgressReview;
			using Native = network::NativeGrantCompletionCurve;
			constexpr U Epoch = 1'000'000, Frontier = 900;
			// A gap before acceptance is different from withholding an already
			// accepted ordinary message. Supply positive BODY completions and exact
			// ACK/retirement times; none is a measured GNS or OS bound.
			for (const bool SlowFirstAck : {false, true}) {
				const U InitialOpportunity = SlowFirstAck ? 0 : 110'000;
				const U Bytes = SlowFirstAck ? 1'057 : CompleteGroupBytes;
				const U NativeUs = SlowFirstAck ? 1'000 : 31'250;
				std::array<Episode, 32> Episodes;
				std::array<Native, 32> Curves;
				U AcceptedBytes = 0, NativeBytes = 0, AckedBytes = 0, RetiredBytes = 0;
				U CompletionAt = Epoch, WaveAt = Epoch + InitialOpportunity;
				for (U Wave = 0; Wave < 8; ++Wave) {
					const U TurnoverUs = SlowFirstAck && Wave == 0 ? 170'000 : 50'000;
					for (U Lane = 0; Lane < 4; ++Lane) {
						const U Index = Wave * 4 + Lane;
						Episodes[Index] = {Epoch, Epoch, Frontier, Index + 1};
						Check(Episodes[Index].Accepted(WaveAt, Bytes, Index + 1) && Episodes[Index].Fits(WaveAt),
							"hypothetical source-ready fairness covers all 32 complete group acceptances");
						Check(Curves[Index].Activate(Index + 1, Bytes, WaveAt) &&
							Curves[Index].FirstSend(Index + 1, Bytes, WaveAt + NativeUs) &&
							Curves[Index].CompletedAtMicroseconds == WaveAt + NativeUs,
							"actual native completion oracle retains unchanged positive BODY deadline");
						AcceptedBytes += Bytes; NativeBytes += Curves[Index].FirstSentBytes;
						// ACKs are supplied separately, strictly after native completion.
						Check(TurnoverUs >= NativeUs, "ACK cannot precede native BODY completion");
						AckedBytes += Bytes; RetiredBytes += Bytes;
					}
					CompletionAt = WaveAt + TurnoverUs;
					WaveAt = CompletionAt; // Next four slots wait for the exact prior receipts.
				}
				Check(AcceptedBytes == NativeBytes && NativeBytes == AckedBytes && AckedBytes == RetiredBytes &&
					CompletionAt - Epoch <= ServiceRecoveryDeadlineUs &&
					CompletionAt - Epoch <= ConvergenceBoundUs(Bytes, 32 * Bytes),
					"supplied finite source conserves exact BODY/ACK/debt and retains both recovery outcomes");
				// These accepted ordinary messages complete at positive native send;
				// a 110-ms handler wait is included in invocation latency, not retimed.
				const U RequestAccepted = Epoch, RequestNative = Epoch + 1'000;
				const U ReplyAccepted = Epoch + 110'000, ReplyNative = Epoch + 112'000;
				Check(RequestNative - RequestAccepted <= 50'000 && ReplyNative - ReplyAccepted <= 50'000 &&
					ReplyNative - RequestAccepted <= 150'000 && ReplyNative - RequestAccepted <= 250'000 &&
					ReplyNative - RequestAccepted <= 500'000,
					"supplied positive ordinary BODY and end-to-end RPC/Event/action clocks stay independent");
				Check(SlowFirstAck ? 170'000 > ProductStructuralClass::TurnoverBudgetUs :
					InitialOpportunity > ProductStructuralClass::OpportunityBudgetUs,
					"healthy supplied product outcomes do not imply either selected internal D03 timer");
			}
			std::array<Episode, 32> Held;
			for (U Index = 0; Index < Held.size(); ++Index) Held[Index] = {Epoch, Epoch, Frontier, Index + 1};
			for (U Index = 0; Index < 4; ++Index) Check(Held[Index].Accepted(Epoch, 1'057, Index + 1), "first four accepted");
			const U FairDeadline = Epoch + FairWaitUs;
			for (U Index = 4; Index < 32; ++Index)
				Check(Held[Index].Fits(FairDeadline) && !Held[Index].Fits(FairDeadline + 1),
					"all-slot ACK hold cannot hide the 28 waiting source episodes");
			Episode OwnNext{Epoch, Epoch, Frontier, 2};
			Check(!OwnNext.Fits(FairDeadline + 1), "a prior owned slot cannot exempt its own next source work");
			// No pending source episode exists for a quiet ACK-owned grant. Its
			// bounded bytes and exact references remain owned; no timer retires them.
			Episode Unfair{Epoch, Epoch, Frontier, 1};
			Check(!Unfair.Fits(FairDeadline + 1), "rotating other peers never resets an excluded peer's source clock");
			Episode Unprepared{Epoch, Epoch + MaximumFreshCreditWarmupUs, Frontier, 1};
			const U WarmDeadline = Epoch + MaximumFreshCreditWarmupUs + FairWaitUs;
			Check(Unprepared.Fits(WarmDeadline) && !Unprepared.Fits(WarmDeadline + 1),
				"fixed source readiness and separate credit warmup cannot be delayed until encoding or fresh feedback");
			const U TinyCreditUs = ServiceUs(1'057, PeerCreditRate);
			Episode ColdTiny{Epoch, Epoch + TinyCreditUs, Frontier, 1};
			Check(TinyCreditUs < MaximumFreshCreditWarmupUs &&
				!ColdTiny.Accepted(Epoch + TinyCreditUs - 1, 1'057, 1) &&
				ColdTiny.Accepted(Epoch + TinyCreditUs, 1'057, 1) && ColdTiny.Fits(Epoch + TinyCreditUs),
				"actual complete-unit credit cost, not a uniform extra G warmup, precedes tiny admission");
			constexpr U SuppliedRecoveryTailBytes = 40 * MiB; // Bytes are not journal cursor units.
			Episode Oldest{Epoch, Epoch, Frontier, 1};
			Check(!Oldest.Accepted(Epoch + 10'000, 77, 99) && !Oldest.Fits(FairDeadline + 1),
				"a tiny unrelated new relevance group cannot clear the oldest owed semantic unit");
			Check(!Oldest.Accepted(Epoch + 20'000, 1'057, 1) && !Oldest.Fits(FairDeadline + 1),
				"an earlier-looking receipt cannot repair an already observed source wait breach");
			Episode MatchingOldest{Epoch, Epoch, Frontier, 1};
			Check(SuppliedRecoveryTailBytes > CompleteGroupBytes &&
				MatchingOldest.Accepted(Epoch + 20'000, 1'057, 1) && MatchingOldest.Fits(FairDeadline + 1),
				"matching complete-unit progress need not deliver an entire 40-MiB recovery fence in one fair wait");
			Episode Filtered{Epoch, Epoch, Frontier};
			Check(Filtered.PartialNoFrame(100) && !Filtered.NoFrame(FairDeadline, 100, true) &&
				!Filtered.Fits(FairDeadline + 1), "partial irrelevant prefix cannot discharge a captured relevant suffix");
			Episode NoFrame{Epoch, Epoch, Frontier};
			Check(!NoFrame.NoFrame(Epoch + 20'000, Frontier, false) &&
				NoFrame.NoFrame(Epoch + 30'000, Frontier, true) && NoFrame.Fits(FairDeadline + 1),
				"complete semantic NoFrame discharge can resolve irrelevant work without manufacturing a grant");
			Episode ColdNoFrame{Epoch, Epoch + MaximumFreshCreditWarmupUs, Frontier};
			Check(ColdNoFrame.NoFrame(Epoch + 30'000, Frontier, true) && ColdNoFrame.Fits(FairDeadline + 1),
				"semantic irrelevant-source discharge needs no fabricated complete-group credit");
			Episode CoveredNoFrame{Epoch, Epoch, Frontier};
			Check(CoveredNoFrame.NoFrame(Epoch + 30'000, Frontier + 1, true) && CoveredNoFrame.Fits(FairDeadline + 1),
				"a complete semantic prefix beyond the frozen fence covers that fence without needing exact equality");
			Episode LateDischarge{Epoch, Epoch, Frontier};
			Check(LateDischarge.NoFrame(FairDeadline + 1, Frontier, true) && !LateDischarge.Fits(FairDeadline + 1),
				"eventual semantic discharge cannot erase an earlier product wait breach");
			Check(!LateDischarge.NoFrame(Epoch + 10'000, Frontier, true) && !LateDischarge.Fits(FairDeadline + 1),
				"a second earlier receipt cannot retrospectively repair a closed late source episode");
			// This finite hypothetical reference has 64 complete groups per peer,
			// all ready at cessation. Complete group acceptance resolves one source
			// episode and immediately exposes its next ready remainder. NoFrame or
			// partial filtering cannot do that. Fair progress alone is insufficient.
			constexpr U GroupsPerPeer = 64, SmallBytes = 1'057, SlowWaveUs = 100'000;
			std::array<Native, 32> SlowCurves;
			std::array<U, 32> NextReady; NextReady.fill(Epoch);
			U SlowAccepted = 0, SlowNative = 0, SlowRetired = 0, SlowCompleted = Epoch;
			for (U Round = 0; Round < GroupsPerPeer; ++Round) {
				for (U Wave = 0; Wave < 8; ++Wave) {
					const U At = Epoch + (Round * 8 + Wave) * SlowWaveUs;
					for (U Lane = 0; Lane < 4; ++Lane) {
						const U Index = Wave * 4 + Lane, Token = Round * 32 + Index + 1;
						Episode Ready{NextReady[Index], NextReady[Index], Frontier, Token};
						Check(Ready.Accepted(At, SmallBytes, Token) && Ready.Fits(At) &&
							SlowCurves[Index].Activate(Token, SmallBytes, At) &&
							SlowCurves[Index].FirstSend(Token, SmallBytes, At + 1'000),
							"every slow finite-reference group has fair source progress and timely positive native BODY");
						SlowAccepted += SmallBytes; SlowNative += SlowCurves[Index].FirstSentBytes;
						SlowRetired += SmallBytes; // Supplied exact ACK at the end of this wave.
						NextReady[Index] = At;
					}
					SlowCompleted = At + SlowWaveUs;
				}
			}
			const U ReferenceBound = ConvergenceBoundUs(GroupsPerPeer * SmallBytes, 32 * GroupsPerPeer * SmallBytes);
			Check(SlowAccepted == 32 * GroupsPerPeer * SmallBytes && SlowAccepted == SlowNative &&
				SlowNative == SlowRetired && SlowCompleted - Epoch > ReferenceBound,
				"fair bounded group progress and exact terminal conservation cannot hide a late immutable reference prefix");
			std::cout << "[PooledRecoveryContract:ProductProgressReview] hypothetical_only=1 peers=32 fair_wait_us="
				<< FairWaitUs << " production_policy_unchanged=1\n";
		}

		// This is a declared finite, fully ready 32-peer workload. Its supplied
		// opportunities and exact receipts establish the count/fairness seam, not
		// WCET, native sends, or a bound for live C3/C5 frame counts under motion.
		{
			using namespace ProductStructuralClass;
			static_assert(GlobalWarmupUs == 31'250 && ReadyFairWaitUs == 1'181'250);
			// Capacity negatives follow an actual shared deadline, not a retained
			// scalar delivered-rate entitlement. All four G grants share activation.
			constexpr U SlowAggregateRate = 14 * MiB;
			constexpr U SingleGrantAtFourteenUs = ServiceUs(CompleteGroupBytes, SlowAggregateRate);
			constexpr U FourGrantsAtFourteenAggregateUs = ServiceUs(4 * CompleteGroupBytes, SlowAggregateRate);
			constexpr U NativeBudgetUs = NativeBudgetNumerator / PeerDrainFloor;
			Check(SingleGrantAtFourteenUs <= NativeBudgetUs &&
				ServiceUs(CompleteGroupBytes + PeerGameplayBurst, SlowAggregateRate) <= 50'000,
				"14 MiB per-peer arithmetic is feasible when actual hard completion and FIFO outcomes pass");
			Check(FourGrantsAtFourteenAggregateUs > NativeBudgetUs,
				"14 MiB aggregate cannot complete four simultaneous maximum grants within their shared hard deadline");
			constexpr U Epoch = 1'000'000;
			const Turnover Boundary{Epoch, Epoch + 31'250, Epoch + 149'000,
				Epoch + 150'000, Epoch + 150'000, CompleteGroupBytes,
				CompleteGroupBytes, CompleteGroupBytes, CompleteGroupBytes};
			Check(CompletedTurnoverFits(Boundary), "new exact 150-ms combined turnover boundary passes");
			auto Late = Boundary; ++Late.OpportunityCompleted;
			Check(!CompletedTurnoverFits(Late), "later admission cannot receive another opportunity budget after turnover");
			auto MissingAck = Boundary;
			MissingAck.Acked = MissingAck.MainRetired = MissingAck.OpportunityCompleted = 0;
			MissingAck.AckedBytes = MissingAck.RetiredBytes = 0;
			const U ObservedAfterDeadline = Epoch + 151'000;
			Check(!CompletedTurnoverFits(MissingAck) && ObservedAfterDeadline - Epoch > TurnoverBudgetUs &&
				MissingAck.Bytes - MissingAck.RetiredBytes == CompleteGroupBytes,
				"withheld ACK fails turnover at 151 ms without manufacturing retirement or releasing debt");
			auto PendingEvidence = Boundary; PendingEvidence.EvidenceComplete = false;
			Check(!CompletedTurnoverFits(PendingEvidence), "incomplete feedback cannot certify combined turnover");
			auto Reversed = Boundary; Reversed.MainRetired = Reversed.Acked - 1;
			Check(!CompletedTurnoverFits(Reversed), "retirement before ACK cannot qualify");
			Check(CompletedOpportunityFits(Epoch, Epoch + 100'000) &&
				!CompletedOpportunityFits(Epoch, Epoch + 101'000) &&
				!CompletedOpportunityFits(Epoch, Epoch - 1) && !CompletedOpportunityFits(Epoch, 0),
				"completed Main opportunities have an exact 100-ms maximum, independent of ACK timing");

			std::array<U, 32> FirstAdmission{};
			// The global warmup can complete during the initial opportunity in
			// this supplied schedule. The sum in ReadyFairWaitUs remains a
			// conservative class budget, not a claimed tight source execution.
			U GlobalCredit = std::min(4 * CompleteGroupBytes,
				StructuralPool * OpportunityBudgetUs / 1'000'000);
			U ExactRetired = 0;
			for (U Wave = 0; Wave < 8; ++Wave) {
				const U Activated = Epoch + OpportunityBudgetUs + Wave * TurnoverBudgetUs;
				Check(Activated - Epoch <= FirstAdmissionBoundUs(Wave * DrainSlots),
					"initial global refill and a completed opportunity precede fair admission");
				Check(GlobalCredit >= DrainSlots * CompleteGroupBytes, "ready wave is globally credited");
				GlobalCredit -= DrainSlots * CompleteGroupBytes;
				for (U Lane = 0; Lane < DrainSlots; ++Lane) {
					const U Rank = Wave * DrainSlots + Lane;
					FirstAdmission[Rank] = Activated;
					const Turnover Value{Activated, Activated + 31'250, Activated + 149'000,
						Activated + TurnoverBudgetUs, Activated + TurnoverBudgetUs, CompleteGroupBytes,
						CompleteGroupBytes, CompleteGroupBytes, CompleteGroupBytes};
					Check(CompletedTurnoverFits(Value), "each ready peer consumes one exact turnover");
					ExactRetired += Value.RetiredBytes;
				}
				// An intermediate completed pass keeps opportunities bounded even
				// while the current receipts remain outstanding. No debt is released.
				Check(CompletedOpportunityFits(Activated, Activated + TurnoverBudgetUs / 2) &&
					CompletedOpportunityFits(Activated + TurnoverBudgetUs / 2, Activated + TurnoverBudgetUs),
					"receipt waits do not pause the Main opportunity clock");
				GlobalCredit = std::min(4 * CompleteGroupBytes,
					GlobalCredit + StructuralPool * TurnoverBudgetUs / 1'000'000);
			}
			Check(FirstAdmission.back() - Epoch <= ReadyFairWaitUs && ExactRetired == 32 * CompleteGroupBytes,
				"32-peer count-aware first admission and exact retirement are finite under the new premises");
			FirstAdmission.back() = 0; // Corrupted rotation never admits the last ready peer.
			Check(!std::ranges::all_of(FirstAdmission, [&](U At) {
				return At >= Epoch && At - Epoch <= ReadyFairWaitUs;
			}), "aggregate progress cannot conceal an excluded ready peer");

			// The original numerical formula stays an additional hard canonical
			// outcome. Class-compliant individual turnovers do not imply that outcome
			// for every supplied finite group stream, including this large count.
			constexpr U SuppliedGroupsPerPeer = 1'280;
			constexpr U SuppliedPeerBytes = SuppliedGroupsPerPeer * CompleteGroupBytes;
			constexpr U SuppliedAggregateBytes = 32 * SuppliedPeerBytes;
			constexpr U SuppliedCompleteUs = OpportunityBudgetUs +
				CeilDiv(32 * SuppliedGroupsPerPeer, DrainSlots) * TurnoverBudgetUs;
			Check(SuppliedCompleteUs > ConvergenceBoundUs(SuppliedPeerBytes, SuppliedAggregateBytes),
				"timely 150-ms turnovers do not imply the unchanged canonical byte-reference outcome");
			Check(8 * TurnoverBudgetUs >= MaximumFreshCreditWarmupUs,
				"a supplied repeated-wave peer can replenish its G credit between turns");

			std::cout << "[PooledRecoveryContract:ProductClass] turnover_us=" << TurnoverBudgetUs
				<< " opportunity_us=" << OpportunityBudgetUs << " global_warmup_us=" << GlobalWarmupUs
				<< " ready_first_admission_bound_us=" << ReadyFairWaitUs
				<< " repeated_count_complete_us=" << SuppliedCompleteUs
				<< " independent_reference_bound_us=" << ConvergenceBoundUs(SuppliedPeerBytes, SuppliedAggregateBytes)
				<< " source_shape=SUPPLIED qualified_live_count_bound=0\n";
		}

		// Historical pre-adoption candidate discriminators. These finite supplied schedules
		// are not a new recovery formula, actual GRPL source replay or evidence of
		// a numerical ACK/slot-service maximum. Keep the existing scalar proof and
		// qualified causal recovery verdicts in their original scope.
		constexpr U NominalMainStepUs = 16'667;
		constexpr U DrainSlots = 4;
		constexpr U CompletionNumerator = PeerDrainFloor * 6'000 +
			(CompleteGroupBytes + 1'248) * 1'000'000;
		constexpr U MaximumIntegerCompletionUs = CompletionNumerator / PeerDrainFloor;
		constexpr U NativeCompleteUs = ServiceUs(CompleteGroupBytes, PeerDrainFloor);
		Check(MaximumIntegerCompletionUs == 37'324 && NativeCompleteUs <= MaximumIntegerCompletionUs,
			"candidate maximum-grant native completion can meet H1 without supplying ACK or slot release");
		constexpr U AckReadyAfterServiceRecoveryUs = NativeCompleteUs + ServiceRecoveryDeadlineUs;
		constexpr U NativeWaveBytes = DrainSlots * CompleteGroupBytes;
		Check(32 * PeerCreditRate == StructuralPool && NativeWaveBytes == 4 * CompleteGroupBytes &&
			AckReadyAfterServiceRecoveryUs > ServiceRecoveryDeadlineUs,
			"unchanged funding and native completion cannot bound a supplied ACK-held slot schedule");
		// In this schedule all four accepted groups have native-completed, but no
		// matching ACK exists before the observation deadline. Ownership must keep
		// four slots and 4G debt charged; native completion cannot authorize a fifth.
		constexpr U OutstandingBeforeAck = NativeWaveBytes;
		constexpr U RetiredBeforeAck = 0;
		constexpr U FreshFeedbackObservedUs = ServiceRecoveryDeadlineUs;
		Check(OutstandingBeforeAck == 4 * CompleteGroupBytes && RetiredBeforeAck == 0 &&
			NativeWaveBytes < ServiceRecoveryDeadlineUs * StructuralPool / 1'000'000,
			"one timely native wave does not imply sustained structural service or exact retirement");
		Check(FreshFeedbackObservedUs == ServiceRecoveryDeadlineUs &&
			AckReadyAfterServiceRecoveryUs > FreshFeedbackObservedUs && RetiredBeforeAck == 0,
			"a current coherent feedback timestamp is not evidence that structural ACK or retirement occurred");

		// Optimistic opportunity/count ceilings: zero payload, encoding, OS, ACK
		// and ordinary-debt cost; one grant per peer and at most four global grants
		// per nominal Main step, with retirement/regrant on the following step.
		// Actual source work and nonzero slot holds can only increase these costs
		// within this declared schedule. Catch-up/within-step phases are not modeled.
		constexpr U TinyCompleteBytes = 77;
		constexpr U TinyGrantsPerPeer = DefaultChangeJournalCapacity;
		constexpr U SamePeerBytes = TinyCompleteBytes * TinyGrantsPerPeer;
		constexpr U SameAggregateBytes = 32 * SamePeerBytes;
		constexpr U PackedGrantsPerPeer = CeilDiv(SamePeerBytes, CompleteGroupBytes);
		constexpr U TinyPeerCountUs = TinyGrantsPerPeer * NominalMainStepUs;
		constexpr U TinyGlobalCountUs = CeilDiv(32 * TinyGrantsPerPeer, DrainSlots) * NominalMainStepUs;
		constexpr U PackedGlobalCountUs = CeilDiv(32 * PackedGrantsPerPeer, DrainSlots) * NominalMainStepUs;
		constexpr U ByteOnlyBoundUs = ConvergenceBoundUs(SamePeerBytes, SameAggregateBytes);
		Check(SamePeerBytes == 1'261'568 && PackedGrantsPerPeer == 3,
			"equal-byte finite inputs can require 16,384 tiny grants or three packed groups per peer");
		Check(TinyPeerCountUs == 273'072'128 && TinyGlobalCountUs == 2'184'577'024 &&
			PackedGlobalCountUs == 400'008 && ByteOnlyBoundUs == 21'072'063,
			"nominal grant opportunities impose a count cost absent from the unchanged byte-only bound");
		Check(TinyGlobalCountUs > ByteOnlyBoundUs && PackedGlobalCountUs < ByteOnlyBoundUs,
			"same bytes and credit do not give tiny and packed schedules the same feasible convergence time");
		// The journal capacity supplies only a finite count for this arithmetic.
		// It does not establish that actual journal operations force these exact
		// 77-byte GRPL groups, nor that source coalescing can produce the packed case.

		constexpr U MediumCompleteBytes = 64 * KiB;
		constexpr U MediumGrantsPerPeer = CompleteGroupBytes / MediumCompleteBytes;
		constexpr U MixedGrantsPerPeer = MediumGrantsPerPeer + 1;
		constexpr U MixedPeerBytes = CompleteGroupBytes + TinyCompleteBytes;
		constexpr U PackedMixedGrantsPerPeer = CeilDiv(MixedPeerBytes, CompleteGroupBytes);
		Check(MediumGrantsPerPeer == 8 && MediumGrantsPerPeer * MediumCompleteBytes == CompleteGroupBytes,
			"eight medium groups carry the same complete bytes as one maximum group");
		Check(MixedGrantsPerPeer == 9 && PackedMixedGrantsPerPeer == 2 &&
			CeilDiv(32 * MixedGrantsPerPeer, DrainSlots) * NominalMainStepUs >
				CeilDiv(32 * PackedMixedGrantsPerPeer, DrainSlots) * NominalMainStepUs,
			"mixed size distribution changes slot-opportunity work even with equal exact byte totals");

		// A supplied per-grant slot cost is independent of byte funding. Doubling
		// it doubles this lower-bound count component. No ACK constant is selected:
		// neither a gameplay Event maximum nor a shutdown timeout bounds structural
		// ACK, Main observation, ordinary-debt release or source readiness.
		auto CountCostUs = [](U Grants, U SuppliedSlotCostUs) {
			return CeilDiv(Grants, DrainSlots) * SuppliedSlotCostUs;
		};
		Check(CountCostUs(32 * MixedGrantsPerPeer, 2 * NominalMainStepUs) ==
			2 * CountCostUs(32 * MixedGrantsPerPeer, NominalMainStepUs),
			"count-sensitive slot cost cannot be inferred from bytes or native completion alone");
		std::cout << "[PooledRecoveryContract:CandidateDiscriminator] groups=4 qualified_contract=0"
			<< " historical_scope=1"
			<< " same_peer_bytes=" << SamePeerBytes << " tiny_groups_per_peer=" << TinyGrantsPerPeer
			<< " packed_groups_per_peer=" << PackedGrantsPerPeer << " byte_only_bound_us=" << ByteOnlyBoundUs
			<< " tiny_count_floor_us=" << TinyGlobalCountUs << " packed_count_floor_us=" << PackedGlobalCountUs
			<< " ack_slot_maximum=NOT_SELECTED source_group_shape=NOT_MEASURED\n";

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

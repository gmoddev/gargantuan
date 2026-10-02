#pragma once

#include "ReliableEnvelopeContractFixture.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/network/ReliableByteAdmissionDiagnostics.hpp"
#include "../src/network/RecoveryCausalEvidence.hpp"

namespace gargantuan::test {
inline void TestStructuralCausalEvidence() {
	using namespace network;
	using namespace network::detail;
	struct SavedEvent {
		StructuralCausalEvent Value;
		std::array<StructuralPendingIdentity, 16> Pending{};
		std::array<ObjectId, 16> Entering{}, Leaving{};
		std::size_t PendingCount = 0, EnterCount = 0, LeaveCount = 0;
	};
	struct Recorder {
		std::array<SavedEvent, 128> Events{};
		std::size_t Count = 0;
		bool Overflow = false;
		StructuralCausalEvidenceSink Sink{this, [](void *Context, const StructuralCausalEvent &Event) noexcept {
			auto &Self = *static_cast<Recorder *>(Context);
			if (Self.Count == Self.Events.size() || Event.ResolvedPending.size() > 16 || Event.Entering.size() > 16 || Event.Leaving.size() > 16) {
				Self.Overflow = true; return;
			}
			auto &Saved = Self.Events[Self.Count++];
			Saved.Value = Event;
			Saved.PendingCount = Event.ResolvedPending.size(); Saved.EnterCount = Event.Entering.size(); Saved.LeaveCount = Event.Leaving.size();
			std::ranges::copy(Event.ResolvedPending, Saved.Pending.begin());
			std::ranges::copy(Event.Entering, Saved.Entering.begin());
			std::ranges::copy(Event.Leaving, Saved.Leaving.begin());
			Saved.Value.ResolvedPending = {}; Saved.Value.Entering = {}; Saved.Value.Leaving = {};
		}};
		StructuralCausalEvidenceSink *Previous = ActiveStructuralCausalEvidence;
		Recorder() { ActiveStructuralCausalEvidence = &Sink; }
		~Recorder() { ActiveStructuralCausalEvidence = Previous; }
		const SavedEvent &Last(StructuralCausalKind Kind) const {
			for (std::size_t Index = Count; Index != 0; --Index)
				if (Events[Index - 1].Value.Kind == Kind) return Events[Index - 1];
			throw std::runtime_error("Missing source causal evidence event");
		}
	} Evidence;
	auto World = std::make_shared<DataModel>();
	auto Object = std::make_shared<Folder>(); Object->SetParent(World);
	const ConnectionId Connection{112, 7};
	PeerRelevanceSelection RootOnly{.RequiredObjects = {World->GetObjectId()}, .DesiredObjects = {World->GetObjectId()}};
	PeerRelevanceSelection WithObject = RootOnly; WithObject.DesiredObjects.push_back(Object->GetObjectId());
	std::ranges::sort(WithObject.DesiredObjects);
	ReplicationCoordinator Coordinator(World);
	std::uint64_t Tick = 0;
	auto Plan = [&](const PeerRelevanceSelection &Selection) {
		EnvelopeRequire(Coordinator.RequestPlanning(Connection, std::make_shared<const PeerRelevanceSelection>(Selection), ++Tick).Succeeded(),
			"causal source planning request succeeds");
		for (int Attempt = 0; Attempt < 2000 && !Coordinator.IsPlanningReady(Connection); ++Attempt)
			Coordinator.ProcessPlanning(++Tick);
	};
	EnvelopeRequire(Coordinator.RegisterPeerPlanned(Connection, ReplicationEpoch(1),
		std::make_shared<const PeerRelevanceSelection>(RootOnly)).Succeeded(), "causal source registers generation");
	for (int Attempt = 0; Attempt < 2000 && !Coordinator.IsPlanningReady(Connection); ++Attempt) Coordinator.ProcessPlanning(++Tick);
	auto Baseline = Coordinator.ProducePendingBaseline(Connection, 8, ++Tick);
	EnvelopeRequire(Baseline.Frame.has_value(), "causal baseline is prepared");
	const auto FirstIdentity = Evidence.Last(StructuralCausalKind::Prepared);
	EnvelopeRequire(FirstIdentity.Value.Connection == Connection && FirstIdentity.Value.SourceScope == World->GetObjectId() &&
		FirstIdentity.Value.Sequence == Baseline.Frame->Sequence.Value() &&
		FirstIdentity.Value.CompleteBytes == Baseline.EncodedFrame.size() + ReliableServiceEnvelopeBytes &&
		FirstIdentity.Value.Fingerprint == ExactCandidateFingerprint(Baseline.EncodedFrame) &&
		Baseline.DiagnosticFingerprint == FirstIdentity.Value.Fingerprint &&
		FirstIdentity.PendingCount == 1 && FirstIdentity.EnterCount == 1,
		"prepared causal identity includes exact nonzero bytes, generation, Enter and pending token without admission sink");
	EnvelopeRequire(Coordinator.DiscardSchedulerPreparation(Connection, Baseline.Frame->Sequence).Succeeded() &&
		Evidence.Last(StructuralCausalKind::Rejected).Value.Sequence == FirstIdentity.Value.Sequence,
		"discard emits rejection and preserves the sequence");
	Baseline = Coordinator.ProducePendingBaseline(Connection, 8, ++Tick);
	EnvelopeRequire(Baseline.Frame && Evidence.Last(StructuralCausalKind::Prepared).Value.Fingerprint == FirstIdentity.Value.Fingerprint &&
		Coordinator.CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded(), "retry preserves exact candidate identity");

	Object->SetName("irrelevant-causal-update");
	const auto ExpectedTail = ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence;
	const auto NoFrame = Coordinator.ProduceIncremental(Connection);
	const auto &Coverage = Evidence.Last(StructuralCausalKind::NoFrame).Value;
	EnvelopeRequire(!NoFrame.Frame && Coordinator.GetJournalLag(Connection) == 0 && Coverage.CursorAfter == ExpectedTail &&
		Coverage.CursorAfter > Coverage.CursorBefore && Coverage.Connection == Connection &&
		Coverage.Reason == StructuralCausalReason::FilteredOrAlreadyCovered,
		"source-validated irrelevant no-frame advance is explicit");

	const auto PlanningStart = Evidence.Count;
	EnvelopeRequire(Coordinator.RequestPlanning(Connection, std::make_shared<const PeerRelevanceSelection>(WithObject), ++Tick).Succeeded(),
		"R7 new current relevance input can remain uninstalled at cessation");
	RecoveryCausalEvidence PlanningFence(32, 1000, 100, 4);
	EnvelopeRequire(PlanningFence.CapturePeer({Connection, ExpectedTail, ExpectedTail,
		Baseline.Frame->Sequence.Value() + 1, 1, 0, {}, {}, true}) && !PlanningFence.Represented(),
		"R7 uninstalled relevance cannot falsely seal an otherwise caught-up source fence");
	for (int Attempt = 0; Attempt < 2000 && !Coordinator.IsPlanningReady(Connection); ++Attempt) Coordinator.ProcessPlanning(++Tick);
	for (auto Index = PlanningStart; Index < Evidence.Count; ++Index) {
		const auto &Recorded = Evidence.Events[Index];
		if (Recorded.Value.Kind == StructuralCausalKind::PendingAdded)
			EnvelopeRequire(PlanningFence.ObservePending(Connection, Recorded.Value.Pending.Token), "R7 actual source pending token joins evidence");
		if (Recorded.Value.Kind == StructuralCausalKind::PlanningInstalled) {
			std::vector<std::uint64_t> Tokens;
			for (std::size_t Pending = 0; Pending < Recorded.PendingCount; ++Pending) Tokens.push_back(Recorded.Pending[Pending].Token);
			EnvelopeRequire(PlanningFence.ObservePlanningInstalled(Connection, Tokens), "R7 actual source install closes only planning barrier");
		}
	}
	EnvelopeRequire(!PlanningFence.Represented() && PlanningFence.Snapshot(Connection)->UnresolvedBaselineTokens == 1 &&
		!PlanningFence.Snapshot(Connection)->HasUnresolvedPlanning,
		"R7 installed necessary Enter inherits baseline obligation and still requires accepted coverage");
	const auto FirstPending = Evidence.Last(StructuralCausalKind::PendingAdded).Value.Pending;
	EnvelopeRequire(FirstPending.Object == Object->GetObjectId() && FirstPending.Enter, "actual installed Enter has causal token");
	Plan(WithObject);
	const auto Replacement = Evidence.Last(StructuralCausalKind::PendingReplaced).Value;
	EnvelopeRequire(Replacement.Pending.Token == FirstPending.Token && Replacement.ReplacementToken > FirstPending.Token,
		"production replanning explicitly links the same obligation to its new token");
	Plan(RootOnly);
	EnvelopeRequire(Evidence.Last(StructuralCausalKind::PendingCancelled).Value.Pending.Token == Replacement.ReplacementToken,
		"relevance cancellation disposes the actual replacement token");
	Plan(WithObject);
	const auto MotionTail = ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence;
	auto Enter = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(Enter.Frame && Evidence.Last(StructuralCausalKind::Prepared).EnterCount == 1 &&
		Evidence.Last(StructuralCausalKind::Prepared).Entering[0] == Object->GetObjectId() &&
		Enter.DiagnosticFingerprint == ExactCandidateFingerprint(Enter.EncodedFrame) &&
		Coordinator.CommitSchedulerAcceptance(Connection, Enter.Frame->Sequence).Succeeded(), "causal Enter prepares and commits");
	Plan(RootOnly);
	auto Leave = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(Leave.Frame && Evidence.Last(StructuralCausalKind::Prepared).LeaveCount == 1 &&
		Evidence.Last(StructuralCausalKind::Prepared).PendingCount == 1 &&
		Evidence.Last(StructuralCausalKind::Prepared).Value.Fingerprint == ExactCandidateFingerprint(Leave.EncodedFrame) &&
		Coordinator.CommitSchedulerAcceptance(Connection, Leave.Frame->Sequence).Succeeded(),
		"causal Leave has exact independent frame identity and resolves its own token");
	EnvelopeRequire(ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence == MotionTail &&
		!Coordinator.GetView(Connection)->Knows(Object->GetObjectId()),
		"R2 R3 R4 accepted Enter then Leave changes peer materialization without inventing source journal history");

	// R5: repeated real planning/materialization changes exercise current-state
	// cancellation, per-generation tokens, and exact frame identity. No synthetic
	// journal mutation or stationary-workload substitution is used.
	for (int Cycle = 0; Cycle < 8; ++Cycle) {
		Plan(WithObject);
		auto MovingEnter = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
		EnvelopeRequire(MovingEnter.Frame && MovingEnter.DiagnosticFingerprint == ExactCandidateFingerprint(MovingEnter.EncodedFrame) &&
			Coordinator.CommitSchedulerAcceptance(Connection, MovingEnter.Frame->Sequence).Succeeded(),
			"R5 oscillating Enter retains exact encoded identity");
		Plan(RootOnly);
		auto MovingLeave = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
		EnvelopeRequire(MovingLeave.Frame && MovingLeave.DiagnosticFingerprint == ExactCandidateFingerprint(MovingLeave.EncodedFrame) &&
			Coordinator.CommitSchedulerAcceptance(Connection, MovingLeave.Frame->Sequence).Succeeded(),
			"R5 oscillating Leave retains exact encoded identity");
	}
	EnvelopeRequire(!Evidence.Overflow && ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence == MotionTail,
		"R5 continuing relevance oscillation produces bounded causal events without source journal growth");

	Plan(WithObject);
	auto Reenter = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(Reenter.Frame && Coordinator.CommitSchedulerAcceptance(Connection, Reenter.Frame->Sequence).Succeeded(),
		"R1 R6 recovery peer reenters before the cessation fence");
	// Complete Enter can represent existing history without advancing the shared
	// cursor immediately. Audit that actual no-frame coverage before the next case.
	for (int Attempt = 0; Attempt < 32 && Coordinator.GetJournalLag(Connection) != 0; ++Attempt) {
		const auto Catchup = Coordinator.ProduceIncremental(Connection);
		EnvelopeRequire(!Catchup.Frame && Catchup.Error == "No relevant replication changes are available",
			"accepted current Enter covers only legitimate preceding journal history");
	}
	EnvelopeRequire(Coordinator.GetJournalLag(Connection) == 0, "accepted current Enter catches the bounded pre-case suffix");
	Evidence.Count = 0; // Retain bounded independent case evidence, not an unbounded log.
	Object->SetName("cessation-first"); Object->SetName("cessation-final");
	const auto CessationTail = ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence;
	std::string QuoteError;
	auto Quote = Coordinator.CaptureFrozenQuote(QuoteError);
	EnvelopeRequire(Quote && QuoteError.empty(), "R1 captures immutable cessation quote");
	const std::map<ConnectionId, std::size_t> FrameLimits{{Connection,
		MaximumReliableServiceGroupBytes - ReliableServiceEnvelopeBytes}};
	const auto Expected = Quote->AdvanceFrozenJournalQuote(FrameLimits);
	const auto Actual = Coordinator.ProduceIncremental(Connection);
	const auto ActualIdentity = Evidence.Last(StructuralCausalKind::Prepared);
	EnvelopeRequire(Expected.Frame && Actual.Frame && Expected.Frame->Sequence == Actual.Frame->Sequence &&
		Expected.Frame->CompleteBytes == Actual.EncodedFrame.size() + ReliableServiceEnvelopeBytes &&
		Expected.Frame->Fingerprint == ActualIdentity.Value.Fingerprint &&
		Expected.Frame->CursorBefore == ActualIdentity.Value.CursorBefore &&
		Expected.Frame->CursorAfter == ActualIdentity.Value.CursorAfter &&
		ActualIdentity.Value.Fingerprint == ExactCandidateFingerprint(Actual.EncodedFrame) &&
		ActualIdentity.Value.CursorAfter == CessationTail && Actual.Frame->Operations.size() == 1,
		"R1 R6 no-motion exact replay and current-state Name coalescing agree byte-for-byte");
	const auto &FinalName = std::get<PropertyReplicationUpdate>(Actual.Frame->Operations.front().Intent);
	EnvelopeRequire(std::get<std::string>(FinalName.Value) == "cessation-final",
		"R6 obsolete Name writes are semantically covered rather than replayed as byte demand");
	RecoveryCausalEvidence FenceAudit(32, 1000, 100, 4);
	EnvelopeRequire(FenceAudit.CapturePeer({Connection, ActualIdentity.Value.CursorBefore, CessationTail,
		Actual.Frame->Sequence.Value(), 1, Expected.Frame->CompleteBytes, {}, {}}), "R1 source-derived fence captures");
	RecoveryPreparedFrame Prepared{Connection, Actual.Frame->Sequence.Value(), ActualIdentity.Value.CompleteBytes,
		ActualIdentity.Value.Fingerprint, {ActualIdentity.Value.CursorBefore, ActualIdentity.Value.CursorAfter,
		RecoveryCoverageDisposition::AcceptedFrame, {}}};
	EnvelopeRequire(FenceAudit.ObservePrepared(Prepared) && !FenceAudit.Represented(),
		"R6 prepared bytes are not accepted source coverage");
	EnvelopeRequire(Coordinator.DiscardSchedulerPreparation(Connection, Actual.Frame->Sequence).Succeeded() &&
		FenceAudit.ObserveRejected(Connection, Actual.Frame->Sequence.Value()) && !FenceAudit.Represented(),
		"R6 rejected source candidate cannot advance the causal cessation fence");
	const auto Retry = Coordinator.ProduceIncremental(Connection);
	EnvelopeRequire(Retry.Frame && Retry.EncodedFrame == Actual.EncodedFrame &&
		Evidence.Last(StructuralCausalKind::Prepared).Value.CursorBefore == ActualIdentity.Value.CursorBefore &&
		Coordinator.CommitSchedulerAcceptance(Connection, Retry.Frame->Sequence).Succeeded() &&
		FenceAudit.ObservePrepared(Prepared) && FenceAudit.ObserveAccepted(Connection, Prepared.Sequence, 1,
			Prepared.CompleteBytes, Prepared.Fingerprint) && FenceAudit.Represented(),
		"R1 R6 actual accepted retry advances the fence with identical source and byte identity");
	// The bare coordinator has no native transport. Do not fabricate delivery:
	// this test proves source representation; GNS/tracker tests own ACK/retirement.
	EnvelopeRequire(!FenceAudit.Converged(), "R10 accepted source representation alone cannot claim native convergence");

	// R7: future live values are not predictable at cessation. Their exact bytes
	// become auditable at preparation, while the original quote remains immutable.
	Object->SetName("bounded-cessation");
	const auto LiveFenceTail = ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence;
	auto LiveQuote = Coordinator.CaptureFrozenQuote(QuoteError);
	EnvelopeRequire(LiveQuote && QuoteError.empty(), "R7 finite cessation quote captures before ongoing mutation");
	Object->SetName("continuous-live-value-after-fence");
	const auto LiveReference = LiveQuote->AdvanceFrozenJournalQuote(FrameLimits);
	const auto LiveFrame = Coordinator.ProduceIncremental(Connection);
	const auto LiveIdentity = Evidence.Last(StructuralCausalKind::Prepared).Value;
	EnvelopeRequire(LiveReference.Frame && LiveFrame.Frame &&
		LiveReference.Frame->Fingerprint != LiveIdentity.Fingerprint &&
		LiveIdentity.Fingerprint == ExactCandidateFingerprint(LiveFrame.EncodedFrame) &&
		LiveIdentity.CursorAfter > LiveFenceTail,
		"R7 exact causal live encoding can differ from immutable cessation reference under legal current-state semantics");
	RecoveryCausalEvidence LiveAudit(32, 1000, 100, 4);
	EnvelopeRequire(LiveAudit.CapturePeer({Connection, LiveIdentity.CursorBefore, LiveFenceTail,
		LiveIdentity.Sequence, 1, LiveReference.Frame->CompleteBytes, {}, {}}) && !LiveAudit.Represented(),
		"R7 withheld baseline source progress remains unresolved despite live source activity");
	EnvelopeRequire(LiveAudit.ObservePrepared({Connection, LiveIdentity.Sequence, LiveIdentity.CompleteBytes,
		LiveIdentity.Fingerprint, {LiveIdentity.CursorBefore, LiveIdentity.CursorAfter, RecoveryCoverageDisposition::AcceptedFrame, {}}}) &&
		Coordinator.CommitSchedulerAcceptance(Connection, LiveFrame.Frame->Sequence).Succeeded() &&
		LiveAudit.ObserveAccepted(Connection, LiveIdentity.Sequence, 2, LiveIdentity.CompleteBytes, LiveIdentity.Fingerprint) &&
		LiveAudit.Represented() && LiveAudit.ReferenceBytes(Connection) == LiveReference.Frame->CompleteBytes,
		"R7 accepted current-state coverage resolves finite source fence without retroactively changing reference bytes");
	Object->SetName("later-live-work-does-not-reopen-source-fence");
	EnvelopeRequire(LiveAudit.Represented() &&
		ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence > LiveIdentity.CursorAfter,
		"R7 later live source backlog cannot reopen represented cessation source prefix");

	auto NextQuotedFrame = [&](ReplicationCoordinator &Frozen) {
		FrozenJournalQuoteStep Result;
		for (int Attempt = 0; Attempt < 2000; ++Attempt) {
			Result = Frozen.AdvanceFrozenJournalQuote(FrameLimits);
			EnvelopeRequire(Result.Error.empty(), "detached pending quote uses captured source without error");
			if (Result.Frame || Result.Complete) return Result;
		}
		throw std::runtime_error("detached pending quote exceeded bounded test work");
	};
	Plan(RootOnly);
	auto LeaveQuote = Coordinator.CaptureFrozenQuote(QuoteError);
	EnvelopeRequire(LeaveQuote && QuoteError.empty(), "cessation quote captures installed pending Leave");
	const auto BeforeDetachedEvents = Evidence.Count;
	const auto ExpectedLeave = NextQuotedFrame(*LeaveQuote);
	EnvelopeRequire(Evidence.Count == BeforeDetachedEvents, "detached planning cannot leak live source observer events");
	const auto ActualLeave = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(ExpectedLeave.Frame && ActualLeave.Frame &&
		ExpectedLeave.Frame->Fingerprint == ExactCandidateFingerprint(ActualLeave.EncodedFrame) &&
		ExpectedLeave.Frame->CompleteBytes == ActualLeave.EncodedFrame.size() + ReliableServiceEnvelopeBytes &&
		ExpectedLeave.Frame->Sequence == ActualLeave.Frame->Sequence &&
		ExpectedLeave.Frame->CursorBefore == ExpectedLeave.Frame->CursorAfter &&
		Coordinator.CommitSchedulerAcceptance(Connection, ActualLeave.Frame->Sequence).Succeeded(),
		"captured pending Leave reference equals exact no-motion production frame without invented journal progress");
	const auto LeaveDone = NextQuotedFrame(*LeaveQuote);
	EnvelopeRequire(LeaveDone.Complete && !LeaveDone.Frame, "frozen Leave then irrelevant journal coverage converges");

	Plan(WithObject);
	auto EnterQuote = Coordinator.CaptureFrozenQuote(QuoteError);
	EnvelopeRequire(EnterQuote && QuoteError.empty(), "cessation quote captures installed pending Enter");
	const auto ExpectedEnter = NextQuotedFrame(*EnterQuote);
	const auto ActualEnter = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(ExpectedEnter.Frame && ActualEnter.Frame &&
		ExpectedEnter.Frame->Fingerprint == ExactCandidateFingerprint(ActualEnter.EncodedFrame) &&
		ExpectedEnter.Frame->CompleteBytes == ActualEnter.EncodedFrame.size() + ReliableServiceEnvelopeBytes &&
		ExpectedEnter.Frame->Sequence == ActualEnter.Frame->Sequence &&
		Coordinator.CommitSchedulerAcceptance(Connection, ActualEnter.Frame->Sequence).Succeeded(),
		"captured pending Enter reference equals exact no-motion production frame");
	EnvelopeRequire(Coordinator.RequestPlanning(Connection, std::make_shared<const PeerRelevanceSelection>(RootOnly), ++Tick).Succeeded(),
		"in-flight cessation input is captured before live planning executes");
	auto InFlightQuote = Coordinator.CaptureFrozenQuote(QuoteError);
	EnvelopeRequire(InFlightQuote && QuoteError.empty(), "captured in-flight input is detached from its coroutine");
	// Source changes its mind after t0 without a journal mutation. The detached
	// reference must still derive t0's known selection rather than future motion.
	Plan(WithObject);
	const auto InFlightExpected = NextQuotedFrame(*InFlightQuote);
	EnvelopeRequire(InFlightExpected.Frame && InFlightExpected.Frame->CursorBefore == InFlightExpected.Frame->CursorAfter &&
		Coordinator.GetView(Connection)->Knows(Object->GetObjectId()) &&
		Coordinator.GetMetrics().StructuralPendingLeaves == 0,
		"detached planning completes captured Leave while later live selection correctly retains the object");

	Coordinator.RemovePeer(Connection);
	EnvelopeRequire(Evidence.Last(StructuralCausalKind::PeerRemoved).Value.Connection == Connection && !Evidence.Overflow,
		"generation removal is explicit and evidence stays bounded");
	EnvelopeRequire(!Coordinator.CommitSchedulerAcceptance(Connection, LiveFrame.Frame->Sequence).Succeeded(),
		"R8 removed peer cannot replay old accepted preparation");
	const ConnectionId ReplacementConnection{Connection.Slot, Connection.Generation + 1};
	EnvelopeRequire(Coordinator.RegisterPeerPlanned(ReplacementConnection, ReplicationEpoch(1),
		std::make_shared<const PeerRelevanceSelection>(RootOnly)).Succeeded(), "R8 slot reuse starts a distinct source generation");
	for (int Attempt = 0; Attempt < 2000 && !Coordinator.IsPlanningReady(ReplacementConnection); ++Attempt)
		Coordinator.ProcessPlanning(++Tick);
	const auto ReplacementBaseline = Coordinator.ProducePendingBaseline(ReplacementConnection, 8, ++Tick);
	EnvelopeRequire(ReplacementBaseline.Frame && Evidence.Last(StructuralCausalKind::Prepared).Value.Connection == ReplacementConnection &&
		ReplacementBaseline.Frame->Sequence.Value() == 1 && !LiveAudit.ObservePending(ReplacementConnection, 1),
		"R8 generation reuse has its own sequence and cannot inherit old recovery coverage");
	Coordinator.RemovePeer(ReplacementConnection);
	// R9 proves the provider-neutral source layer on two independent coordinator
	// instances over the same authoritative world. This is not a TLS/provider PASS.
	ReplicationCoordinator LocalSource(World), NodeSource(World);
	const ConnectionId IndependentConnection{113, 7};
	for (auto *Source : {&LocalSource, &NodeSource}) {
		EnvelopeRequire(Source->RegisterPeerPlanned(IndependentConnection, ReplicationEpoch(1),
			std::make_shared<const PeerRelevanceSelection>(WithObject)).Succeeded(), "R9 independent source registers");
		for (int Attempt = 0; Attempt < 2000 && !Source->IsPlanningReady(IndependentConnection); ++Attempt)
			Source->ProcessPlanning(++Tick);
		auto IndependentBaseline = Source->ProducePendingBaseline(IndependentConnection, 8, ++Tick);
		EnvelopeRequire(IndependentBaseline.Frame && Source->CommitSchedulerAcceptance(IndependentConnection,
			IndependentBaseline.Frame->Sequence).Succeeded(), "R9 independent source baseline accepts");
	}
	Object->SetName("provider-neutral-recovery-state");
	const auto LocalFrame = LocalSource.ProduceIncremental(IndependentConnection);
	const auto NodeFrame = NodeSource.ProduceIncremental(IndependentConnection);
	EnvelopeRequire(LocalFrame.Frame && NodeFrame.Frame && LocalFrame.EncodedFrame == NodeFrame.EncodedFrame &&
		LocalFrame.DiagnosticFingerprint == ExactCandidateFingerprint(LocalFrame.EncodedFrame) &&
		NodeFrame.DiagnosticFingerprint == LocalFrame.DiagnosticFingerprint,
		"R9 identical authoritative semantics yield exact identical source evidence independent of provider ownership");
	LocalSource.RemovePeer(IndependentConnection); NodeSource.RemovePeer(IndependentConnection);
	World->Destroy();
	std::cout << "[Recovery:SourceEvidence] R1-R10 causal coordinator cases passed; native delivery and physical providers remain separate gates\n";
}
}

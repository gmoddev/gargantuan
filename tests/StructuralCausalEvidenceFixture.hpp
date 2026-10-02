#pragma once

#include "ReliableEnvelopeContractFixture.hpp"
#include "../src/network/GameSessionTestAccess.hpp"
#include "../src/network/ReliableByteAdmissionDiagnostics.hpp"

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

	Plan(WithObject);
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
	auto Enter = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(Enter.Frame && Evidence.Last(StructuralCausalKind::Prepared).EnterCount == 1 &&
		Coordinator.CommitSchedulerAcceptance(Connection, Enter.Frame->Sequence).Succeeded(), "causal Enter prepares and commits");
	Plan(RootOnly);
	auto Leave = Coordinator.ProducePendingRelevance(Connection, 8, ++Tick);
	EnvelopeRequire(Leave.Frame && Evidence.Last(StructuralCausalKind::Prepared).LeaveCount == 1 &&
		Evidence.Last(StructuralCausalKind::Prepared).PendingCount == 1 &&
		Evidence.Last(StructuralCausalKind::Prepared).Value.Fingerprint == ExactCandidateFingerprint(Leave.EncodedFrame) &&
		Coordinator.CommitSchedulerAcceptance(Connection, Leave.Frame->Sequence).Succeeded(),
		"causal Leave has exact independent frame identity and resolves its own token");
	Coordinator.RemovePeer(Connection);
	EnvelopeRequire(Evidence.Last(StructuralCausalKind::PeerRemoved).Value.Connection == Connection && !Evidence.Overflow,
		"generation removal is explicit and evidence stays bounded");
	World->Destroy();
}
}

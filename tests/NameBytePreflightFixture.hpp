#pragma once

#include "ReliableEnvelopeContractFixture.hpp"
#include "../src/network/GameSessionTestAccess.hpp"

namespace gargantuan::test {

struct NameBytePreflightFixture {
	using Coordinator = network::ReplicationCoordinator;
	std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
	std::vector<std::shared_ptr<Folder>> Objects;
	std::unique_ptr<Coordinator> Optimized, Reference;
	network::ConnectionId Connection{152, 4};
	network::PeerRelevanceSelection Selection;
	runtime_detail::WorkSample OptimizedWork{}, ReferenceWork{};
	static constexpr auto Limit = network::MaximumReliableServiceGroupBytes - network::ReliableServiceEnvelopeBytes;

	explicit NameBytePreflightFixture(std::size_t Count = 32) {
		Selection.RequiredObjects = {World->GetObjectId()};
		Selection.DesiredObjects = {World->GetObjectId()};
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Object = std::make_shared<Folder>();
			Object->SetParent(World);
			Selection.DesiredObjects.push_back(Object->GetObjectId());
			Objects.push_back(std::move(Object));
		}
		std::ranges::sort(Selection.DesiredObjects);
		Optimized = std::make_unique<Coordinator>(World);
		Reference = std::make_unique<Coordinator>(World);
		network::detail::GameSessionTestAccess::SetIncrementalEncodingOptimizationsEnabled(*Reference, false);
		for (auto *Source : {Optimized.get(), Reference.get()}) {
			auto Baseline = Source->AddPeerBounded(Connection, network::ReplicationEpoch(1), Selection);
			EnvelopeRequire(Baseline.Frame && Source->CommitSchedulerAcceptance(Connection, Baseline.Frame->Sequence).Succeeded(),
				"Name preflight A/B fixture accepts identical production baselines");
		}
	}
	~NameBytePreflightFixture() { Optimized->RemovePeer(Connection); Reference->RemovePeer(Connection); World->Destroy(); }

	void Names(std::size_t Rounds, std::size_t Bytes = 24 * 1024) {
		for (std::size_t Round = 0; Round < Rounds; ++Round)
			for (std::size_t Index = 0; Index < Objects.size(); ++Index)
				Objects[Index]->SetName(std::string(Bytes, static_cast<char>('a' + (Round + Index) % 26)));
	}
	void Barrier() {
		EnvelopeRequire(Objects.front()->ApplyAttributeMutation("PreflightBarrier", WireValue(1),
			ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success, "Name preflight fixture inserts semantic barrier");
	}
	network::ReplicationProduceResult Compare(std::size_t Transitions = 512, std::size_t Reads = 2048,
		std::size_t FrameLimit = Limit, std::size_t Available = Limit, bool Accept = true) {
		struct Observation {
			std::uint64_t Before = 0, After = 0;
			bool Seen = false;
			network::detail::StructuralCausalEvidenceSink Sink{this,
				[](void *Context, const network::detail::StructuralCausalEvent &Event) noexcept {
					if (Event.Kind != network::detail::StructuralCausalKind::Prepared &&
						Event.Kind != network::detail::StructuralCausalKind::NoFrame) return;
					auto &Self = *static_cast<Observation *>(Context);
					Self.Before = Event.CursorBefore; Self.After = Event.CursorAfter; Self.Seen = true;
				}};
		};
		Observation LeftEvidence, RightEvidence;
		auto Produce = [&](Coordinator &Source, runtime_detail::WorkSample &Sample, Observation &Evidence) {
			runtime_detail::WorkCapture Capture(&Sample);
			struct EvidenceScope {
				network::detail::StructuralCausalEvidenceSink *Previous = network::detail::ActiveStructuralCausalEvidence;
				explicit EvidenceScope(network::detail::StructuralCausalEvidenceSink &Sink) {
					network::detail::ActiveStructuralCausalEvidence = &Sink;
				}
				~EvidenceScope() { network::detail::ActiveStructuralCausalEvidence = Previous; }
			} Scope(Evidence.Sink);
			return Source.ProduceIncremental(Connection, Transitions, FrameLimit, Reads, Available);
		};
		auto Left = Produce(*Optimized, OptimizedWork, LeftEvidence);
		auto Right = Produce(*Reference, ReferenceWork, RightEvidence);
		EnvelopeRequire(Left.Error == Right.Error && bool(Left.Frame) == bool(Right.Frame) &&
			Left.SelectedTransitions == Right.SelectedTransitions && Left.DeferredForBytes == Right.DeferredForBytes &&
			Left.RequiredFrameBytes == Right.RequiredFrameBytes && Left.EncodedFrame == Right.EncodedFrame &&
			Left.DiagnosticFingerprint == Right.DiagnosticFingerprint && Left.JournalRecordsExamined == Right.JournalRecordsExamined &&
			Left.JournalRecordsExamined <= Reads && LeftEvidence.Seen == RightEvidence.Seen &&
			LeftEvidence.Before == RightEvidence.Before && LeftEvidence.After == RightEvidence.After,
			"Name preflight preserves exact bytes/fingerprint, outcome, cursor evidence and charged journal examinations");
		EnvelopeRequire(OptimizedWork.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::EncodeRetries)] ==
			ReferenceWork.Counters[static_cast<std::size_t>(runtime_detail::WorkCounter::EncodeRetries)],
			"Name preflight preserves geometric retry diagnostics");
		if (Left.Frame) {
			EnvelopeRequire(Left.Frame->Sequence == Right.Frame->Sequence &&
				Left.EncodedFrame.size() + network::ReliableServiceEnvelopeBytes <= network::MaximumReliableServiceGroupBytes,
				"Name preflight preserves sequence and the finite complete-group limit");
			if (Accept) {
				EnvelopeRequire(Optimized->CommitSchedulerAcceptance(Connection, Left.Frame->Sequence).Succeeded() &&
					Reference->CommitSchedulerAcceptance(Connection, Right.Frame->Sequence).Succeeded(), "A/B exact frames accept");
			} else {
				EnvelopeRequire(Optimized->DiscardSchedulerPreparation(Connection, Left.Frame->Sequence).Succeeded() &&
					Reference->DiscardSchedulerPreparation(Connection, Right.Frame->Sequence).Succeeded(), "A/B preparations reject without progress");
			}
		}
		EnvelopeRequire(Optimized->GetJournalLag(Connection) == Reference->GetJournalLag(Connection),
			"Name preflight preserves actual accepted cursor");
		const auto *LeftView = Optimized->GetView(Connection);
		const auto *RightView = Reference->GetView(Connection);
		EnvelopeRequire(LeftView && RightView && LeftView->Connection == RightView->Connection &&
			LeftView->Epoch == RightView->Epoch && LeftView->KnownObjects == RightView->KnownObjects &&
			LeftView->RelevantObjects == RightView->RelevantObjects &&
			LeftView->LatestStateSequences == RightView->LatestStateSequences,
			"Name preflight preserves the accepted peer view snapshot");
		const auto LeftMetrics = Optimized->GetMetrics(), RightMetrics = Reference->GetMetrics();
		EnvelopeRequire(LeftMetrics.OperationsGenerated == RightMetrics.OperationsGenerated &&
			LeftMetrics.OperationsCoalesced == RightMetrics.OperationsCoalesced &&
			LeftMetrics.IncrementalBytes == RightMetrics.IncrementalBytes &&
			LeftMetrics.StructuralTransitionsAccepted == RightMetrics.StructuralTransitionsAccepted &&
			LeftMetrics.StructuralTransitionsCommitted == RightMetrics.StructuralTransitionsCommitted &&
			LeftMetrics.ReplicationBacklog == RightMetrics.ReplicationBacklog,
			"Name preflight preserves committed operation, byte, transition and backlog counters");
		return Left;
	}
	void Drain() {
		for (std::size_t Attempt = 0; Attempt < 4096; ++Attempt) {
			auto Result = Compare();
			EnvelopeRequire(Result.Frame || Result.Error == "No relevant replication changes are available" ||
				Result.Error == "No replication changes are available", "Name preflight legal workload drains without new errors");
			if (Optimized->GetJournalLag(Connection) == 0) return;
		}
		throw std::runtime_error("Name preflight legal workload failed bounded convergence");
	}
};

inline void TestNameBytePreflight() {
	using namespace network;
	using runtime_detail::WorkCounter;
	auto Counter = [](const auto &Sample, WorkCounter CounterValue) { return Sample.Counters[static_cast<std::size_t>(CounterValue)]; };
	{
		NameBytePreflightFixture F;
		F.Names(16);
		const auto Deferred = F.Compare(512, 2048, F.Limit, 0);
		EnvelopeRequire(Deferred.DeferredForBytes && Deferred.RequiredFrameBytes != 0,
			"Name preflight retains exact byte deferral rather than approximating admission");
		EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) > 0 &&
			Counter(F.OptimizedWork, WorkCounter::NamePreflightCacheHits) > 0 &&
			Counter(F.OptimizedWork, WorkCounter::NamePreflightValidationBytes) == 32 * 24 * 1024,
			"one call validates each immutable current Name once across geometric retries");
		EnvelopeRequire(F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls <
			F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls,
			"Name byte proof eliminates actual doomed encoder calls");
		EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::StructuralEncodePayloadBytes) <
			Counter(F.ReferenceWork, WorkCounter::StructuralEncodePayloadBytes),
			"encoding optimizations reduce actual payload writes across successful and discarded attempts");
		std::cout << "[Network:NameBytePreflight] case=current"
			<< " optimized_encode_calls=" << F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls
			<< " reference_encode_calls=" << F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls
			<< " optimized_encode_ns=" << F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Nanoseconds
			<< " reference_encode_ns=" << F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Nanoseconds
			<< " preflight_validation_bytes=" << Counter(F.OptimizedWork, WorkCounter::NamePreflightValidationBytes)
			<< " optimized_payload_bytes=" << Counter(F.OptimizedWork, WorkCounter::StructuralEncodePayloadBytes)
			<< " reference_payload_bytes=" << Counter(F.ReferenceWork, WorkCounter::StructuralEncodePayloadBytes)
			<< " preflight_cache_hits=" << Counter(F.OptimizedWork, WorkCounter::NamePreflightCacheHits) << '\n';
		F.Compare(512, 2048, F.Limit, F.Limit, false);
		F.Objects.front()->SetName(std::string(24 * 1024, 'Z'));
		F.Drain(); // New top-level calls must not reuse stale validation/cache identity.
	}
	{
		NameBytePreflightFixture F;
		F.Names(12); F.Barrier(); F.Names(2);
		F.Drain();
		EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) > 0 &&
			F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls <
			F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls,
			"historical Name retries optimize while barrier and current suffix preserve every accepted frame");
		EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::StructuralEncodePayloadBytes) <
			Counter(F.ReferenceWork, WorkCounter::StructuralEncodePayloadBytes),
			"historical frames reduce discarded payload writes while preserving exact accepted output");
		std::cout << "[Network:NameBytePreflight] case=historical"
			<< " optimized_encode_calls=" << F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls
			<< " reference_encode_calls=" << F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Calls
			<< " optimized_encode_ns=" << F.OptimizedWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Nanoseconds
			<< " reference_encode_ns=" << F.ReferenceWork[static_cast<std::size_t>(runtime_detail::WorkPhase::StructuralEncode)].Nanoseconds
			<< " optimized_payload_bytes=" << Counter(F.OptimizedWork, WorkCounter::StructuralEncodePayloadBytes)
			<< " reference_payload_bytes=" << Counter(F.ReferenceWork, WorkCounter::StructuralEncodePayloadBytes) << '\n';
	}
	for (const std::size_t Reads : {0u, 1u, 2u, 3u, 5u, 17u, 31u, 63u}) {
		NameBytePreflightFixture F;
		F.Names(1);
		F.Compare(512, 0); // Refresh both catalogs without reading or accepting peer history.
		const auto InitialLag = F.Optimized->GetJournalLag(F.Connection);
		constexpr std::size_t FrameLimit = 32 * 1024;
		const auto Deferred = F.Compare(512, Reads, FrameLimit, 0);
		EnvelopeRequire(!Deferred.Frame && F.Optimized->GetJournalLag(F.Connection) == InitialLag,
			"small read budgets and byte deferral cannot advance accepted history");
		if (Reads == 0) {
			EnvelopeRequire(Deferred.Error == "Replication transition work limit is invalid" &&
				Deferred.JournalRecordsExamined == 0, "zero journal budget retains the original validation error");
			continue;
		}
		EnvelopeRequire(Deferred.DeferredForBytes && Deferred.RequiredFrameBytes > 24 * 1024 &&
			Deferred.RequiredFrameBytes <= FrameLimit, "odd read budgets retain exact bounded byte deferral");
		const auto Rejected = F.Compare(512, Reads, FrameLimit, FrameLimit, false);
		EnvelopeRequire(Rejected.Frame && F.Optimized->GetJournalLag(F.Connection) == InitialLag,
			"bounded geometric preparation rejection preserves the original cursor");
		const auto Accepted = F.Compare(512, Reads, FrameLimit, FrameLimit);
		EnvelopeRequire(Accepted.Frame && Accepted.EncodedFrame == Rejected.EncodedFrame &&
			F.Optimized->GetJournalLag(F.Connection) < InitialLag,
			"bounded retry accepts exactly the rejected bytes without losing history");
		if (Reads >= 3) EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) > 0,
			"odd read budget regression exercises the optimized recursive path");
	}
	bool SawExhaustedReadBudget = false, SawAtomicStopWithBudget = false;
	for (const std::size_t Reads : {3u, 5u, 17u, 31u, 63u}) {
		NameBytePreflightFixture F;
		F.Names(1);
		F.Compare(512, 0);
		const auto InitialLag = F.Optimized->GetJournalLag(F.Connection);
		const auto Exhausted = F.Compare(512, Reads, 1024);
		std::cout << "[Network:NameBytePreflight] case=atomic-budget reads=" << Reads
			<< " examined=" << Exhausted.JournalRecordsExamined << " error=" << Exhausted.Error << '\n';
		EnvelopeRequire(!Exhausted.Frame &&
			Exhausted.Error == "Structural operation exceeds the negotiated reliable message limit" &&
			Exhausted.JournalRecordsExamined > 0 && Exhausted.JournalRecordsExamined <= Reads &&
			F.Optimized->GetJournalLag(F.Connection) == InitialLag &&
			Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) > 0,
			"bounded geometric journal tail preserves exact atomic error, A/B read charge and uncommitted cursor");
		// The real encoder stops at one indivisible record. Odd budgets can leave
		// an unused remainder; Compare still requires exact A/B charge equality.
		SawExhaustedReadBudget |= Exhausted.JournalRecordsExamined == Reads;
		SawAtomicStopWithBudget |= Exhausted.JournalRecordsExamined < Reads;
	}
	EnvelopeRequire(SawExhaustedReadBudget && SawAtomicStopWithBudget,
		"journal regressions exercise both exhaustion and an atomic stop before exhaustion");
	for (const bool InvalidInsidePrefix : {true, false}) {
		NameBytePreflightFixture F(4);
		F.Objects[0]->SetName(std::string((InvalidInsidePrefix ? 40 : 24) * 1024, 'a'));
		if (!InvalidInsidePrefix) F.Objects[1]->SetName(std::string(24 * 1024, 'b'));
		ChangeJournal::Get().Commit(F.World->GetObjectId(), F.Objects[InvalidInsidePrefix ? 1 : 2]->GetObjectId(),
			PropertyUpdatedChange{"Name", WireValue(std::string{"\xc0\xaf", 2}), true});
		F.Barrier(); // Keep the malformed historical Name instead of current-state coalescing.
		F.Compare(512, 0);
		const auto InitialLag = F.Optimized->GetJournalLag(F.Connection);
		const auto Prefix = F.Compare(2, 31, 32 * 1024);
		if (InvalidInsidePrefix) {
			EnvelopeRequire(!Prefix.Frame && !Prefix.Error.empty() &&
				F.Optimized->GetJournalLag(F.Connection) == InitialLag &&
				Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) == 0,
				"invalid selected Name wins over an earlier oversized string prefix");
		} else {
			EnvelopeRequire(Prefix.Frame && Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) > 0,
				"malformed Name outside the transition prefix cannot reject a legal reduced prefix");
			const auto RemainingLag = F.Optimized->GetJournalLag(F.Connection);
			const auto Invalid = F.Compare(2, 31, 32 * 1024);
			EnvelopeRequire(!Invalid.Frame && !Invalid.Error.empty() &&
				F.Optimized->GetJournalLag(F.Connection) == RemainingLag,
				"accepting the earlier valid prefix cannot discard the later invalid Name");
		}
	}
	for (const auto &Invalid : std::array<std::string, 4>{std::string{"\xc0\xaf", 2}, std::string{"a\0b", 3},
		std::string(MaximumProtocolStringBytes + 1, 'x'), std::string{"\xed\xa0\x80", 3}}) {
		NameBytePreflightFixture F;
		F.Names(1);
		// Retained malformed history exercises error precedence independently of
		// current-state authoring validation. A later barrier prevents coalescing it.
		ChangeJournal::Get().Commit(F.World->GetObjectId(), F.Objects.back()->GetObjectId(),
			PropertyUpdatedChange{"Name", WireValue(Invalid), true});
		F.Barrier();
		const auto Result = F.Compare();
		EnvelopeRequire(!Result.Frame && !Result.Error.empty() &&
			Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) == 0,
			"invalid later Name is rejected before oversized-prefix retry can hide it");
	}
	{
		NameBytePreflightFixture F;
		F.Names(1);
		const auto ClassId = GetActiveRuntimeSchemaRegistry().FindClassByName("Engine.Folder")->Id;
		ChangeJournal::Get().Commit(F.World->GetObjectId(), F.Objects.back()->GetObjectId(),
			PropertyUpdatedChange{"Name", WireValue(std::string{"custom-schema-value"}), true, ClassId, 1});
		F.Compare();
		EnvelopeRequire(Counter(F.OptimizedWork, WorkCounter::NamePreflightRetries) == 0,
			"declaring-class Name is never treated as a native Name byte proof");
	}
	{
		NameBytePreflightFixture F(8);
		F.Names(1, 1);
		const auto Small = F.Compare(512, 2048, F.Limit, 0);
		EnvelopeRequire(Small.DeferredForBytes && Small.RequiredFrameBytes > 8, "measure exact native framing overhead");
		const auto Overhead = Small.RequiredFrameBytes - 8;
		for (std::size_t Index = 0; Index < 7; ++Index) F.Objects[Index]->SetName(std::string(MaximumProtocolStringBytes, 'm'));
		const auto Tail = F.Limit - Overhead - 7 * MaximumProtocolStringBytes;
		EnvelopeRequire(Tail > 0 && Tail <= MaximumProtocolStringBytes, "exact maximum group has legal individual Names");
		F.Objects.back()->SetName(std::string(Tail, 't'));
		const auto Maximum = F.Compare();
		EnvelopeRequire(Maximum.Frame && Maximum.EncodedFrame.size() + ReliableServiceEnvelopeBytes == MaximumReliableServiceGroupBytes,
			"preflight preserves an exact legal 512-KiB complete group");
		F.Objects.front()->SetName(std::string(MaximumProtocolStringBytes, 'n'));
		const auto Atomic = F.Compare(1, 3, 1024);
		EnvelopeRequire(!Atomic.Frame && Atomic.Error == "Structural operation exceeds the negotiated reliable message limit",
			"atomic oversize retains the existing indivisible-operation error");
	}
	std::cout << "[Network:NameBytePreflight] exact-AB/errors/budgets/cache/512KiB result=pass\n";
}

}

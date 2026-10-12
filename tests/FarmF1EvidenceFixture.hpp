#pragma once
#include "../src/host/server/FarmF1Evidence.hpp"
#include <memory>
#include <sstream>
#include <stdexcept>
#include <source_location>

inline void TestFarmF1Evidence() {
	using namespace gargantuan;
	using network::detail::PooledServiceRecord;
	auto Require = [](bool Value, std::source_location Site = std::source_location::current()) {
		if (!Value) throw std::runtime_error("farm F1 retention regression at line " + std::to_string(Site.line()));
	};
	auto Completed = [](std::uint32_t Slot, std::uint64_t Sequence = 1, bool Failed = false) {
		PooledServiceRecord R;
		R.Connection = {Slot, 1}; R.Accepted.Structural = Sequence * 77;
		R.Result.Valid = R.Result.Available = true; R.Result.Qualified = !Failed;
		R.Result.RetiredBytes = 77;
		R.Result.RetiredToken = Sequence;
		R.Result.CompletedTurnovers = Sequence; R.Result.CompletedAdmissionOpportunities = Sequence;
		R.Result.LastCompletedTurnoverToken = Sequence;
		R.Result.LastCompletedTurnoverActivatedAtMicroseconds = Sequence * 10000;
		R.Result.LastCompletedTurnoverAtMicroseconds = Sequence * 10000 + 20;
		R.Result.MaximumMainOpportunityMicroseconds = 20; R.Result.MaximumTurnoverMicroseconds = 20;
		R.Feedback.emplace(); auto &S = *R.Feedback;
		S.Connection = R.Connection; S.CountersValid = true;
		S.QualifiedOrdinaryRegisteredMessages = S.QualifiedOrdinaryCompletedMessages = Sequence;
		S.QualifiedOrdinaryMaximumLatencyMicroseconds = 50000;
		S.ObservedAtMicroseconds = Sequence * 10000 + 20;
		S.StructuralCompletedGrantSequence = Sequence;
		S.StructuralPayloadBytesFirstSent = S.StructuralPayloadBytesAcked = Sequence * 77;
		S.StructuralLastCompletedGrantToken = Sequence;
		S.StructuralLastCompletedGrantBytes = 77;
		S.StructuralLastCompletedGrantActivatedAtMicroseconds = Sequence * 10000;
		S.StructuralLastCompletedGrantFirstSendAtMicroseconds = Sequence * 10000 + 10;
		S.StructuralLastCompletedGrantCompletedAtMicroseconds = Sequence * 10000 + 10;
		S.StructuralLastCompletedGrantFailed = Failed;
		S.LastCompletedStructuralSegmentEventCount = 1;
		S.LastCompletedStructuralSegmentEvents[0] = {Sequence * 10000 + 10, 77};
		return R;
	};
	auto Grant = [](host::detail::FarmF1Evidence &Evidence, const PooledServiceRecord &R) {
		const auto &S = *R.Feedback;
		Evidence.Accept(R.Connection, S.StructuralLastCompletedGrantToken, 77,
			S.StructuralLastCompletedGrantActivatedAtMicroseconds);
		Evidence.Observe(R);
	};
	auto Initial = [&](std::uint32_t Slot, std::uint64_t Sequence, std::uint64_t Delay) {
		auto R = Completed(Slot, Sequence, true); auto &S = *R.Feedback;
		const auto First = S.StructuralLastCompletedGrantActivatedAtMicroseconds + Delay;
		S.StructuralLastCompletedGrantFirstSendAtMicroseconds = S.StructuralLastCompletedGrantCompletedAtMicroseconds = First;
		S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = First;
		S.ObservedAtMicroseconds = First + 10;
		R.Result.LastCompletedTurnoverAtMicroseconds = S.ObservedAtMicroseconds;
		R.Result.MaximumTurnoverMicroseconds = Delay + 10;
		auto &T = S.LastCompletedInitialTrace;
		T.Enabled = true; T.Token = Sequence; T.MessageNumber = 9; T.NativeHandle = 123; T.DispatchSequence = 8;
		T.TransportEntered = Sequence * 10000; T.AdapterLocked = T.TransportEntered + 1;
		T.NativeApiEntered = T.AdapterLocked + 1; T.NativeApiLocked = T.NativeApiEntered + 1;
		T.NativeEnqueued = T.NativeApiLocked + 1; T.FirstSnpVisit = T.NativeEnqueued + 1;
		T.PacketPreparationEntered = T.FirstSnpVisit + 1; T.WorkerQueued = T.PacketPreparationEntered + 1;
		T.WorkerStarted = T.WorkerQueued + 1; T.NativeCompleted = First; T.ActorReconciled = First + 1;
		return R;
	};
	const auto *Previous = network::detail::ActivePooledService;
	using FarmEvidence = host::detail::FarmF1Evidence;
	{
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1); auto &Evidence = *Storage;
		auto R = Initial(1, 1, 7070); R.Result.Qualified = true;
		R.Feedback->StructuralServiceFailed = true;
		R.Feedback->StructuralMaximumFiniteShortfallByteMicroseconds = 1;
		R.Feedback->StructuralMaximumDeficitByteMicroseconds =
			network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds + 1;
		Grant(Evidence, R);
		Require(Evidence.Valid(1) && Evidence.Observations()[0].FirstFailed.Measured &&
			Evidence.Observations()[0].FirstInitialFailure.Measured);
		std::ostringstream Out; Evidence.Write(Out, "hybrid-test");
		Require(Out.str().find("contract=HYBRID_F1_V1") != std::string::npos &&
			Out.str().find("completion_failed=0 last_completion_failed=0 completion_evidence_invalid=0 execution_failed=0") != std::string::npos);
		auto Strict = std::make_unique<FarmEvidence>(); Grant(*Strict, R);
		Require(!Strict->Valid(1)); // The same old native deficit remains fatal under the old contract.
	}
	for (const auto Delay : {37324ULL, 37325ULL}) {
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		auto R = Initial(1, 1, Delay); R.Result.Qualified = true;
		Grant(*Storage, R);
		Require(Storage->Valid(1) == (Delay == 37324));
	}
	for (const auto Burst : {31250ULL, 31251ULL, 35715ULL}) {
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		auto R = Completed(1); auto &S = *R.Feedback;
		R.Accepted.Structural = R.Result.RetiredBytes = S.StructuralLastCompletedGrantBytes = network::MaximumReliableServiceGroupBytes;
		S.StructuralPayloadBytesFirstSent = S.StructuralPayloadBytesAcked = R.Accepted.Structural;
		S.StructuralLastCompletedGrantFirstSendAtMicroseconds = 11000;
		S.StructuralLastCompletedGrantCompletedAtMicroseconds = 11000 + Burst;
		S.ObservedAtMicroseconds = S.StructuralLastCompletedGrantCompletedAtMicroseconds + 1;
		S.LastCompletedStructuralSegmentEventCount = 2;
		S.LastCompletedStructuralSegmentEvents[0] = {11000, 1248};
		S.LastCompletedStructuralSegmentEvents[1] = {11000 + Burst, R.Accepted.Structural - 1248};
		R.Result.LastCompletedTurnoverAtMicroseconds = S.ObservedAtMicroseconds;
		R.Result.MaximumTurnoverMicroseconds = S.ObservedAtMicroseconds - 10000;
		Storage->Accept(R.Connection, 1, R.Accepted.Structural, 10000); Storage->Observe(R);
		Require(Storage->Valid(1)); // 14-MiB/s serialization can fit H1; no extra 31.25-ms hard gate.
	}
	for (unsigned Case = 0; Case < 6; ++Case) {
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		auto R = Completed(1); auto &S = *R.Feedback;
		if (Case == 0) S.StructuralCompletionServiceFailed = true;
		if (Case == 1) S.StructuralLastCompletedCompletionFailed = true;
		if (Case == 2) S.StructuralCompletionEvidenceInvalid = true;
		if (Case == 3) S.LastCompletedStructuralSegmentEventCount = 0;
		if (Case == 4) S.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 76;
		if (Case == 5) S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = 10009;
		Grant(*Storage, R); Grant(*Storage, Completed(1, 2));
		Require(!Storage->Valid(1)); // Later clean certificates cannot erase hard failure or malformed chronology.
	}
	{
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		Grant(*Storage, Completed(1));
		auto R = Completed(1); R.Feedback.reset(); R.Result.Available = false;
		R.Result.RetiredBytes = R.Result.RetiredToken = 0; R.Result.ExecutionFailed = true;
		Storage->Observe(R);
		Require(!Storage->Valid(1) && Storage->Observations()[0].ExecutionFailed);
	}
	for (unsigned Case = 0; Case < 5; ++Case) {
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		Grant(*Storage, Completed(1));
		auto R = Completed(1); R.Feedback.reset(); R.Result.Available = false;
		R.Result.RetiredBytes = R.Result.RetiredToken = 0;
		if (Case == 0) R.Result.MainOpportunityFailed = true;
		if (Case == 1) R.Result.TurnoverFailed = true;
		if (Case == 2) R.Result.MaximumMainOpportunityMicroseconds = 100001;
		if (Case == 3) R.Result.MaximumTurnoverMicroseconds = 150001;
		if (Case == 4) R.Result.QueuedOrdinaryEverFailed = true;
		Storage->Observe(R);
		Require(!Storage->Valid(1) && (Case == 4 ? Storage->Observations()[0].QueuedFailed : Storage->Observations()[0].ExecutionFailed));
	}
	for (unsigned Case = 0; Case < 10; ++Case) {
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		Grant(*Storage, Completed(1));
		auto R = Completed(1); auto &S = *R.Feedback;
		R.Result.RetiredBytes = R.Result.RetiredToken = 0;
		if (Case == 0) S.QualifiedOrdinaryRegisteredMessages = S.QualifiedOrdinaryCompletedMessages = 0;
		if (Case == 1) { S.QualifiedOrdinaryRegisteredMessages = 2; S.QualifiedOrdinaryPendingMessages = 1; }
		if (Case == 2) S.QualifiedOrdinaryMaximumLatencyMicroseconds = 50001;
		if (Case == 3) S.QualifiedOrdinaryEverFailed = true;
		if (Case == 4) S.QualifiedOrdinaryLastCompletedFailed = true;
		if (Case == 5) S.QualifiedOrdinaryActiveDeadlineFailed = true;
		if (Case == 6) S.QualifiedOrdinaryEvidenceInvalid = true;
		if (Case == 7) {
			S.QualifiedOrdinaryEverFailed = true; S.QualifiedOrdinaryFailureSequence = 9;
			S.QualifiedOrdinaryFailurePhase = 2; S.QualifiedOrdinaryFailureMessageNumber = 7;
			S.QualifiedOrdinaryFailureAcceptedAtMicroseconds = 100;
			S.QualifiedOrdinaryFailureCompletedAtMicroseconds = 50101;
			S.QualifiedOrdinaryFailureExpectedBytes = S.QualifiedOrdinaryFailureFirstSentBytes = 115;
		}
		if (Case == 8) S.QualifiedOrdinaryPendingMessages = 1; // Count mismatch, not a pending-message certificate.
		if (Case == 9) S.QualifiedOrdinaryFailurePhase = 1; // Partial first-failure witness is malformed.
		Storage->Observe(R);
		Require(!Storage->Valid(1));
		if (Case == 7) {
			const auto &P = Storage->Observations()[0];
			Require(P.OrdinaryFailureSequence == 9 && P.OrdinaryFailurePhase == 2 &&
				P.OrdinaryFailureMessageNumber == 7 && P.OrdinaryFailureCompleted == 50101 && P.OrdinaryFailureExpectedBytes == 115);
		}
	}
	{
		auto Storage = std::make_unique<FarmEvidence>(FarmEvidence::Contract::HybridF1V1);
		auto R = Completed(1);
		R.Feedback->QualifiedOrdinaryRegisteredMessages = R.Feedback->QualifiedOrdinaryCompletedMessages = 0;
		R.Feedback->QualifiedOrdinaryMaximumLatencyMicroseconds = 0;
		Grant(*Storage, R);
		Require(!Storage->Valid(1)); // An otherwise complete structural campaign cannot invent H2 coverage.
	}
	for (const bool Mismatch : {false, true}) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1, 1, true);
		auto &T = R.Feedback->LastCompletedInitialTrace;
		T.Enabled = true; T.Token = Mismatch ? 2 : 1; T.MessageNumber = 9; T.NativeHandle = 123; T.DispatchSequence = 8;
		T.TransportEntered = 10000; T.AdapterLocked = 10001; T.NativeApiEntered = 10002; T.NativeApiLocked = 10003;
		T.NativeEnqueued = 10004; T.FirstSnpVisit = 10005; T.PacketPreparationEntered = 10006;
		T.WorkerQueued = 10007; T.WorkerStarted = 10008; T.NativeCompleted = 10010; T.ActorReconciled = 10011;
		Grant(Evidence, R);
		const auto &W = Evidence.Observations()[0].FirstFailed;
		Require(W.Measured && W.InitialTraceValid == !Mismatch && W.InitialTrace.DispatchSequence == 8);
		Grant(Evidence, Completed(1, 2));
		Require(Evidence.Observations()[0].FirstFailed.InitialTrace.Token == T.Token && !Evidence.Valid(1));
		std::ostringstream Out; Evidence.Write(Out, "initial-trace");
		Require(Out.str().find(Mismatch ? "connection=1:1 valid=0 token=2" : "connection=1:1 valid=1 token=1") != std::string::npos);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		for (unsigned I = 1; I <= 32; ++I) Grant(Evidence, Completed(I));
		Require(Evidence.Valid());
		// Exact ACK and retirement cannot conceal a failed final first-send grant.
		Grant(Evidence, Completed(32, 2, true));
		Require(!Evidence.Valid());
		Grant(Evidence, Completed(32, 3));
		Require(!Evidence.Valid()); // Later requalification does not erase evidence.
		const auto &Witness = Evidence.Observations()[31].FirstFailed;
		Require(Witness.Observed && Witness.Measured && Witness.Sequence == 2 && Witness.Token == 2 &&
			Witness.Bytes == 77 && Witness.Activated == 20000 && Witness.First == 20010 &&
			Witness.Completed == 20010 && Witness.SegmentCount == 1 &&
			Witness.Segments[0].AtMicroseconds == 20010 && Witness.Segments[0].PayloadBytes == 77);
		std::ostringstream Output; Evidence.Write(Output, "f1-test");
		Require(Output.str().find("completed=3 qualified=2") != std::string::npos);
		Require(Output.str().find(" failed=0 valid=1\n[Qualification:FarmF1Failure] event=peer") != std::string::npos);
		Require(Output.str().find("connection=32:1 sequence=2 token=2 bytes=77 activated_us=20000") != std::string::npos);
		Require(Output.str().find("connection=32:1 token=2 sequence=0 at_us=20010 bytes=77") != std::string::npos);
	}
	Require(network::detail::ActivePooledService == Previous);
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		Grant(Evidence, Completed(1));
		Grant(Evidence, Completed(1, 3)); // Lost completion cannot be inferred.
		Require(!Evidence.Valid(1));
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1); R.Result.RetiredBytes = 0;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
		R.Result.RetiredBytes = 77; Evidence.Observe(R); Require(Evidence.Valid(1));
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1); R.Feedback->StructuralMaximumFiniteShortfallByteMicroseconds = 1;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		for (unsigned I = 1; I <= 33; ++I) Grant(Evidence, Completed(I));
		Require(!Evidence.Valid());
		std::ostringstream Out; Evidence.Write(Out, "storage-overflow");
		Require(Out.str().find("storage_invalid=1") != std::string::npos &&
			Out.str().find("first_failed_observed=1") == std::string::npos);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		// Explicit production bootstrap boundary; absence of a certificate alone
		// never exempts a qualified grant from F1.
		auto R = Completed(1); auto &S = *R.Feedback;
		S.StructuralCompletedGrantSequence = S.StructuralLastCompletedGrantToken = 0;
		S.StructuralLastCompletedGrantBytes = S.StructuralLastCompletedGrantActivatedAtMicroseconds = 0;
		S.StructuralLastCompletedGrantFirstSendAtMicroseconds = S.StructuralLastCompletedGrantCompletedAtMicroseconds = 0;
		Evidence.Accept(R.Connection, 1, 77, std::numeric_limits<std::uint64_t>::max());
		Evidence.Observe(R); Require(!Evidence.Valid(1)); // Bootstrap alone proves no qualified service.
		R = Completed(1, 2); R.Feedback->StructuralCompletedGrantSequence = 1;
		Grant(Evidence, R); Require(Evidence.Valid(1));
		std::ostringstream Out; Evidence.Write(Out, "bootstrap");
		Require(Out.str().find("bootstrap_accepted=1 bootstrap_bytes=77 bootstrap_retired=77 pending_token=0") != std::string::npos);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1); R.Feedback->StructuralCompletedGrantSequence = 0;
		Grant(Evidence, R); Require(!Evidence.Valid(1)); // Qualified accepted bytes cannot become bootstrap.
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		Evidence.Observe(Completed(1)); Require(!Evidence.Valid(1)); // Missing admission identity fails closed.
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1); R.Result.RetiredToken = 9;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
		Require(Evidence.Observations()[0].AccountingInvalid && !Evidence.Observations()[0].FirstFailed.Observed);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		Grant(Evidence, Completed(1));
		Evidence.Accept({1, 1}, 2, 77, std::numeric_limits<std::uint64_t>::max());
		Require(!Evidence.Valid(1)); // A Ready peer cannot relabel later work as bootstrap.
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1, 1, true);
		R.Feedback->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds =
			network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds + 1;
		R.Feedback->LastCompletedStructuralSegmentEventCount = 2;
		R.Feedback->StructuralLastCompletedGrantCompletedAtMicroseconds += 5;
		R.Feedback->LastCompletedStructuralSegmentEvents[0].PayloadBytes = 20;
		R.Feedback->LastCompletedStructuralSegmentEvents[1] = {10015, 57};
		Grant(Evidence, R);
		const auto Maximum = R.Feedback->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds;
		Grant(Evidence, Completed(1, 2));
		Grant(Evidence, Completed(1, 3, true));
		const auto &P = Evidence.Observations()[0]; const auto &W = P.FirstFailed;
		Require(!P.AccountingInvalid && W.Measured && W.Token == 1 && W.MaximumRunning == Maximum &&
			W.SegmentCount == 2 && W.Completed == 10015 && W.Segments[1].PayloadBytes == 57);
	}
	for (unsigned Case = 0; Case < 7; ++Case) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1, 1, true); auto &S = *R.Feedback;
		if (Case == 0) S.LastCompletedStructuralSegmentEventCount = 0;
		if (Case == 1) S.LastCompletedStructuralSegmentEventCount = 513;
		if (Case == 2) S.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 76;
		if (Case == 3) S.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 78;
		if (Case == 4) S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = 10009;
		if (Case == 5) S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = 10011;
		if (Case == 6) S.CountersValid = false;
		Grant(Evidence, R); Grant(Evidence, Completed(1, 2));
		const auto &W = Evidence.Observations()[0].FirstFailed;
		Require(W.Observed && !W.Measured && W.Token == 1);
		std::ostringstream Out; Evidence.Write(Out, "malformed");
		Require(Out.str().find("first_failed_measured=0") != std::string::npos &&
			Out.str().find("event=segment") == std::string::npos);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1, 1, true); R.Feedback->Connection = {1, 2};
		Grant(Evidence, R);
		Require(Evidence.Observations()[0].AccountingInvalid && !Evidence.Observations()[0].FirstFailed.Measured);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Completed(1, 1, true); auto &S = *R.Feedback;
		R.Accepted.Structural = S.StructuralLastCompletedGrantBytes = 512;
		S.StructuralPayloadBytesFirstSent = S.StructuralPayloadBytesAcked = 512;
		R.Result.RetiredBytes = 512;
		S.LastCompletedStructuralSegmentEventCount = 512;
		for (auto &Event : S.LastCompletedStructuralSegmentEvents) Event = {10010, 1};
		Evidence.Accept(R.Connection, 1, 512, 10000); Evidence.Observe(R);
		const auto &W = Evidence.Observations()[0].FirstFailed;
		Require(W.Measured && W.SegmentCount == 512 && W.Segments[511].PayloadBytes == 1);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		struct Notification { unsigned Count = 0; std::uint64_t Observed = 0; } Notice;
		Evidence.SetFailureObserver(&Notice, [](void *Context, std::uint64_t Observed) noexcept {
			auto &N = *static_cast<Notification *>(Context); ++N.Count; N.Observed = Observed;
		}, true);
		auto Bad = Completed(1, 1, true); Bad.Feedback->LastCompletedStructuralSegmentEventCount = 513;
		Grant(Evidence, Bad); Require(Notice.Count == 0);
		auto Accounting = Completed(2); Accounting.Result.RetiredToken = 9;
		Grant(Evidence, Accounting); Require(Notice.Count == 0);
		Grant(Evidence, Completed(3, 1, true)); Require(Notice.Count == 0); // Running/drain failure does not freeze.
		Grant(Evidence, Initial(3, 2, 7070));
		Require(Notice.Count == 1 && Notice.Observed == 27080);
		const auto &P = Evidence.Observations()[2];
		Require(P.FirstFailed.Token == 1 && P.FirstInitialFailure.Measured && P.FirstInitialFailure.Token == 2 &&
			P.FirstInitialFailure.InitialTraceValid && P.FirstInitialFailure.First == 27070);
		Grant(Evidence, Completed(3, 3)); Grant(Evidence, Initial(4, 1, 8000));
		Require(Notice.Count == 1 && Notice.Observed == 27080 && P.FirstInitialFailure.Token == 2);
		std::ostringstream Out; Evidence.Write(Out, "initial-after-running");
		Require(Out.str().find("sequence=2 token=2 bytes=77 activated_us=20000 first_us=27070") != std::string::npos &&
			Out.str().find("event=trace run=initial-after-running connection=3:1 valid=1 enabled=1") != std::string::npos);
	}
	for (unsigned Case = 0; Case < 5; ++Case) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		unsigned Notifications = 0;
		Evidence.SetFailureObserver(&Notifications, [](void *Context, std::uint64_t) noexcept {
			++*static_cast<unsigned *>(Context);
		}, true);
		auto R = Initial(1, 1, 7070);
		if (Case == 0) R.Result.Valid = false;
		if (Case == 1) R.Accepted.Structural = 78;
		if (Case == 2) R.Feedback->StructuralPayloadBytesFirstSent = 78;
		if (Case == 3) R.Feedback->StructuralPayloadBytesAcked = 78;
		if (Case == 4) R.Result.RetiredToken = 9;
		Grant(Evidence, R);
		const auto &P = Evidence.Observations()[0];
		Require(P.AccountingInvalid && P.FirstFailed.Observed && !P.FirstFailed.Measured &&
			!P.FirstInitialFailure.Measured && Notifications == 0);
	}
	{
		// Exact integer-microsecond boundary from the canonical byte-us intercept;
		// no W/R drain allowance and no rounded millisecond trigger.
		using Curve = network::FiniteGrantServiceCurve;
		Require(!Curve::ZeroFirstSendBreach(0, 7070) &&
			!Curve::ZeroFirstSendBreach(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()) &&
			!Curve::ZeroFirstSendBreach(10000, 9999) &&
			!Curve::ZeroFirstSendBreach(1, std::numeric_limits<std::uint64_t>::max()));
		const auto LastLegal = Curve::FiniteInterceptByteMicroseconds / Curve::PeerRateBytesPerSecond;
		Require(LastLegal * Curve::PeerRateBytesPerSecond <= Curve::FiniteInterceptByteMicroseconds &&
			(LastLegal + 1) * Curve::PeerRateBytesPerSecond > Curve::FiniteInterceptByteMicroseconds);
		for (const auto Delay : {LastLegal, LastLegal + 1}) {
			auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
			unsigned Notifications = 0;
			Evidence.SetFailureObserver(&Notifications, [](void *Context, std::uint64_t) noexcept {
				++*static_cast<unsigned *>(Context);
			}, true);
			Grant(Evidence, Initial(1, 1, Delay));
			Require(Evidence.Observations()[0].FirstInitialFailure.Measured == (Delay > LastLegal) &&
				Notifications == (Delay > LastLegal ? 1U : 0U));
		}
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		// A maximum grant begins promptly but drains beyond its finite envelope.
		// A finite/running miss is real, but zero-first service did not breach.
		auto R = Initial(1, 1, 295); auto &S = *R.Feedback;
		R.Accepted.Structural = S.StructuralLastCompletedGrantBytes = network::MaximumReliableServiceGroupBytes;
		S.StructuralPayloadBytesFirstSent = S.StructuralPayloadBytesAcked = R.Accepted.Structural;
		R.Result.RetiredBytes = R.Accepted.Structural;
		S.StructuralLastCompletedGrantCompletedAtMicroseconds = 50000;
		S.ObservedAtMicroseconds = 50010;
		S.LastCompletedStructuralSegmentEventCount = 2;
		S.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 1248;
		S.LastCompletedStructuralSegmentEvents[1] = {50000, R.Accepted.Structural - 1248};
		S.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds =
			network::PooledReliableServiceProfile::RunningGrantBoundByteMicroseconds + 1;
		S.StructuralMaximumFiniteShortfallByteMicroseconds = 1;
		unsigned Notifications = 0;
		Evidence.SetFailureObserver(&Notifications, [](void *Context, std::uint64_t) noexcept {
			++*static_cast<unsigned *>(Context);
		}, true);
		Evidence.Accept(R.Connection, 1, R.Accepted.Structural, 10000); Evidence.Observe(R);
		Require(Evidence.Observations()[0].FirstFailed.Measured && !Evidence.Observations()[0].FirstInitialFailure.Measured &&
			Notifications == 0 && !Evidence.Valid(1));
	}
	for (unsigned Case = 0; Case < 13; ++Case) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Initial(1, 1, 7070); auto &S = *R.Feedback;
		// Admission is held fixed so a stale token or activation cannot certify a breach.
		Evidence.Accept(R.Connection, 1, 77, 10000);
		if (Case == 0) S.StructuralLastCompletedGrantToken = 2;
		if (Case == 1) S.StructuralLastCompletedGrantActivatedAtMicroseconds = 9999;
		if (Case == 2) S.StructuralLastCompletedGrantFirstSendAtMicroseconds = 9999;
		if (Case == 3) S.StructuralLastCompletedGrantCompletedAtMicroseconds = 17069;
		if (Case == 4) S.ObservedAtMicroseconds = 17069;
		if (Case == 5) S.LastCompletedStructuralSegmentEventCount = 0;
		if (Case == 6) S.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = 17069;
		if (Case == 7) S.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 76;
		if (Case == 8) S.CountersValid = false;
		if (Case == 9) S.StructuralCompletedGrantSequence = 2;
		if (Case == 10) S.StructuralLastCompletedGrantBytes = 0;
		if (Case == 11) S.StructuralLastCompletedGrantActivatedAtMicroseconds = 0;
		if (Case == 12) S.StructuralLastCompletedGrantActivatedAtMicroseconds = std::numeric_limits<std::uint64_t>::max();
		Evidence.Observe(R);
		Require(!Evidence.Observations()[0].FirstInitialFailure.Measured);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto Bad = Initial(1, 1, 7070); Bad.Feedback->LastCompletedStructuralSegmentEventCount = 0;
		Grant(Evidence, Bad); Grant(Evidence, Initial(1, 2, 7070));
		Require(!Evidence.Observations()[0].FirstFailed.Measured &&
			Evidence.Observations()[0].FirstInitialFailure.Measured && Evidence.Observations()[0].FirstInitialFailure.Token == 2);
	}
	for (const bool Mismatch : {false, true}) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto R = Initial(1, 1, 7070);
		if (Mismatch) R.Feedback->LastCompletedInitialTrace.Token = 2;
		else R.Feedback->LastCompletedInitialTrace.WorkerStarted = 17071;
		Grant(Evidence, R);
		Require(Evidence.Observations()[0].FirstInitialFailure.Measured &&
			!Evidence.Observations()[0].FirstInitialFailure.InitialTraceValid);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto Earlier = Completed(1); Earlier.Result.Valid = false;
		Grant(Evidence, Earlier); Grant(Evidence, Completed(1, 2, true));
		Require(Evidence.Observations()[0].AccountingInvalid && Evidence.Observations()[0].FirstFailed.Measured);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		unsigned Notifications = 0;
		Evidence.SetFailureObserver(&Notifications, [](void *Context, std::uint64_t) noexcept {
			++*static_cast<unsigned *>(Context);
		}); // Existing default freezes on the first general measured failure.
		Grant(Evidence, Completed(1, 1, true));
		Require(Notifications == 1 && Evidence.Observations()[0].FirstFailed.Measured &&
			!Evidence.Observations()[0].FirstInitialFailure.Measured);
	}
}

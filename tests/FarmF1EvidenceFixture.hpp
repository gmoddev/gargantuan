#pragma once
#include "../src/host/server/FarmF1Evidence.hpp"
#include <memory>
#include <sstream>
#include <stdexcept>

inline void TestFarmF1Evidence() {
	using namespace gargantuan;
	using network::detail::PooledServiceRecord;
	auto Require = [](bool Value) { if (!Value) throw std::runtime_error("farm F1 retention regression"); };
	auto Completed = [](std::uint32_t Slot, std::uint64_t Sequence = 1, bool Failed = false) {
		PooledServiceRecord R;
		R.Connection = {Slot, 1}; R.Accepted.Structural = Sequence * 77;
		R.Result.Valid = R.Result.Available = true; R.Result.Qualified = !Failed;
		R.Result.RetiredBytes = 77;
		R.Result.RetiredToken = Sequence;
		R.Feedback.emplace(); auto &S = *R.Feedback;
		S.Connection = R.Connection; S.CountersValid = true;
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
	const auto *Previous = network::detail::ActivePooledService;
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
		});
		auto Bad = Completed(1, 1, true); Bad.Feedback->LastCompletedStructuralSegmentEventCount = 513;
		Grant(Evidence, Bad); Require(Notice.Count == 0);
		auto Accounting = Completed(2); Accounting.Result.RetiredToken = 9;
		Grant(Evidence, Accounting); Require(Notice.Count == 0);
		Grant(Evidence, Completed(3, 1, true));
		Require(Notice.Count == 1 && Notice.Observed == 10020);
		Grant(Evidence, Completed(3, 2)); Grant(Evidence, Completed(4, 1, true));
		Require(Notice.Count == 1 && Notice.Observed == 10020);
	}
	for (unsigned Case = 0; Case < 5; ++Case) {
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		unsigned Notifications = 0;
		Evidence.SetFailureObserver(&Notifications, [](void *Context, std::uint64_t) noexcept {
			++*static_cast<unsigned *>(Context);
		});
		auto R = Completed(1, 1, true);
		if (Case == 0) R.Result.Valid = false;
		if (Case == 1) R.Accepted.Structural = 78;
		if (Case == 2) R.Feedback->StructuralPayloadBytesFirstSent = 78;
		if (Case == 3) R.Feedback->StructuralPayloadBytesAcked = 78;
		if (Case == 4) R.Result.RetiredToken = 9;
		Grant(Evidence, R);
		const auto &P = Evidence.Observations()[0];
		Require(P.AccountingInvalid && P.FirstFailed.Observed && !P.FirstFailed.Measured && Notifications == 0);
	}
	{
		auto Storage = std::make_unique<host::detail::FarmF1Evidence>(); auto &Evidence = *Storage;
		auto Earlier = Completed(1); Earlier.Result.Valid = false;
		Grant(Evidence, Earlier); Grant(Evidence, Completed(1, 2, true));
		Require(Evidence.Observations()[0].AccountingInvalid && Evidence.Observations()[0].FirstFailed.Measured);
	}
}

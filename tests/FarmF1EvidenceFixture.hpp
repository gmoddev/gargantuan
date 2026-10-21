#pragma once
#include "../src/host/server/FarmF1Evidence.hpp"
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
		host::detail::FarmF1Evidence Evidence;
		for (unsigned I = 1; I <= 32; ++I) Grant(Evidence, Completed(I));
		Require(Evidence.Valid());
		// Exact ACK and retirement cannot conceal a failed final first-send grant.
		Grant(Evidence, Completed(32, 2, true));
		Require(!Evidence.Valid());
		Grant(Evidence, Completed(32, 3));
		Require(!Evidence.Valid()); // Later requalification does not erase evidence.
		std::ostringstream Output; Evidence.Write(Output, "f1-test");
		Require(Output.str().find("completed=3 qualified=2") != std::string::npos);
	}
	Require(network::detail::ActivePooledService == Previous);
	{
		host::detail::FarmF1Evidence Evidence;
		Grant(Evidence, Completed(1));
		Grant(Evidence, Completed(1, 3)); // Lost completion cannot be inferred.
		Require(!Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Result.RetiredBytes = 0;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
		R.Result.RetiredBytes = 77; Evidence.Observe(R); Require(Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Feedback->StructuralMaximumFiniteShortfallByteMicroseconds = 1;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		for (unsigned I = 1; I <= 33; ++I) Grant(Evidence, Completed(I));
		Require(!Evidence.Valid());
	}
	{
		host::detail::FarmF1Evidence Evidence;
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
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Feedback->StructuralCompletedGrantSequence = 0;
		Grant(Evidence, R); Require(!Evidence.Valid(1)); // Qualified accepted bytes cannot become bootstrap.
	}
	{
		host::detail::FarmF1Evidence Evidence;
		Evidence.Observe(Completed(1)); Require(!Evidence.Valid(1)); // Missing admission identity fails closed.
	}
	{
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Result.RetiredToken = 9;
		Grant(Evidence, R); Require(!Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		Grant(Evidence, Completed(1));
		Evidence.Accept({1, 1}, 2, 77, std::numeric_limits<std::uint64_t>::max());
		Require(!Evidence.Valid(1)); // A Ready peer cannot relabel later work as bootstrap.
	}
}

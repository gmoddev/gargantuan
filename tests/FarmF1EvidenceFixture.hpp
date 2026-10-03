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
	const auto *Previous = network::detail::ActivePooledService;
	{
		host::detail::FarmF1Evidence Evidence;
		for (unsigned I = 1; I <= 32; ++I) Evidence.Observe(Completed(I));
		Require(Evidence.Valid());
		// Exact ACK and retirement cannot conceal a failed final first-send grant.
		Evidence.Observe(Completed(32, 2, true));
		Require(!Evidence.Valid());
		Evidence.Observe(Completed(32, 3));
		Require(!Evidence.Valid()); // Later requalification does not erase evidence.
		std::ostringstream Output; Evidence.Write(Output, "f1-test");
		Require(Output.str().find("completed=3 qualified=2") != std::string::npos);
	}
	Require(network::detail::ActivePooledService == Previous);
	{
		host::detail::FarmF1Evidence Evidence;
		Evidence.Observe(Completed(1));
		Evidence.Observe(Completed(1, 3)); // Lost completion cannot be inferred.
		Require(!Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Result.RetiredBytes = 0;
		Evidence.Observe(R); Require(!Evidence.Valid(1));
		R.Result.RetiredBytes = 77; Evidence.Observe(R); Require(Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		auto R = Completed(1); R.Feedback->StructuralMaximumFiniteShortfallByteMicroseconds = 1;
		Evidence.Observe(R); Require(!Evidence.Valid(1));
	}
	{
		host::detail::FarmF1Evidence Evidence;
		for (unsigned I = 1; I <= 33; ++I) Evidence.Observe(Completed(I));
		Require(!Evidence.Valid());
	}
}

#pragma once
#include "../src/network/OrdinaryServicePolicy.hpp"
#include <stdexcept>
#include <thread>
#include <vector>

namespace gargantuan::test {
inline void TestOrdinaryServicePolicy() {
	using namespace network::detail;
	auto Check = [](bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); };
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>(); OrdinaryServicePolicy Policy(Global);
		Check(!Policy.Accept(33, 0, 1'000'000).Protected, "bootstrap has no normal-source entitlement");
		Check(Policy.SetReady(true), "Main declares the peer Ready");
		for (unsigned I = 0; I < 16; ++I) Check(Policy.Accept(33, 0, 1'000'000).Protected, "canonical peer burst enrolls");
		Check(!Policy.Accept(33, 0, 1'000'000).Protected, "excess remains outside the peer count allocation");
		Check(Global->SetNormal(false) && Global->SetNormal(true) && Policy.SetReady(false) && Policy.SetReady(true),
			"scope and readiness change without recreating the policy");
		Check(!Policy.Accept(33, 0, 1'000'000).Protected, "policy churn cannot refill exhausted source budgets");
		Check(!Policy.Accept(33, 0, 1'015'624).Protected && Policy.Accept(33, 0, 1'015'625).Protected,
			"peer count refill preserves its exact fractional boundary");
	}
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>(); OrdinaryServicePolicy Policy(Global); Policy.SetReady(true);
		Check(Policy.Accept(20'480, 1, 1'000'000).Protected, "complete BODY exactly fills the peer byte burst");
		Check(!Policy.Accept(33, 1, 1'000'000).Protected && !Policy.Accept(33, 1, 1'001'000).Protected &&
			Policy.Accept(33, 1, 1'001'008).Protected, "peer byte envelope includes adapter bytes and exact refill");
	}
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>();
		std::vector<OrdinaryServicePolicy> Peers;
		for (unsigned I = 0; I < 9; ++I) { Peers.emplace_back(Global); Peers.back().SetReady(true); }
		for (unsigned I = 0; I < 8; ++I) for (unsigned N = 0; N < 16; ++N)
			Check(Peers[I].Accept(33, 2, 1'000'000).Protected, "shared 128-message burst enrolls exactly");
		Check(!Peers[8].Accept(33, 2, 1'000'000).Protected && !Peers[8].Accept(33, 2, 1'001'953).Protected &&
			Peers[8].Accept(33, 2, 1'001'954).Protected, "fresh peer cannot bypass shared count envelope");
	}
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>();
		std::vector<OrdinaryServicePolicy> Peers;
		for (unsigned I = 0; I < 9; ++I) { Peers.emplace_back(Global); Peers.back().SetReady(true); }
		for (unsigned I = 0; I < 8; ++I)
			Check(Peers[I].Accept(20'480, 3, 1'000'000).Protected, "shared 160-KiB byte burst enrolls exactly");
		Check(!Peers[8].Accept(33, 3, 1'000'000).Protected && !Peers[8].Accept(33, 3, 1'000'125).Protected &&
			Peers[8].Accept(33, 3, 1'000'126).Protected, "fresh peer cannot bypass shared byte envelope");
	}
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>(); OrdinaryServicePolicy Policy(Global); Policy.SetReady(true);
		for (unsigned I = 0; I < 16; ++I) Check(Policy.Prepare(33, 6, 1'000'000).Protected,
			"direct WouldBlock proposals create no consumed enrollment or obligation");
		const auto Accepted = Policy.Prepare(33, 6, 1'000'000);
		Global->SetNormal(false);
		Check(Policy.Commit(33, Accepted) && Accepted.Protected && Accepted.AcceptedAtMicroseconds == 1'000'000,
			"accepted direct membership and entry clock survive scope churn");
		Check(!Policy.Accept(33, 6, 1'000'000).Protected, "overload enrolls no new protected work");
		Global->SetNormal(true);
		for (unsigned I = 0; I < 15; ++I) Check(Policy.Accept(33, 6, 1'000'000).Protected, "normal resumes its remaining allocation");
		Check(!Policy.Accept(33, 6, 1'000'000).Protected, "resume creates no new burst");
	}
	{
		auto Global = std::make_shared<OrdinaryServiceBudget>(); OrdinaryServicePolicy Policy(Global); Policy.SetReady(true);
		bool OtherProtected = true, OtherAccepted = false;
		std::thread Other([&] {
			const auto Candidate = Policy.Prepare(33, 6, 1'000'000);
			OtherProtected = Candidate.Protected; OtherAccepted = Policy.Commit(33, Candidate);
		}); Other.join();
		Check(!OtherProtected && OtherAccepted, "non-Main direct traffic remains accepted outside the declared source class");
		for (unsigned I = 0; I < 16; ++I) Check(Policy.Accept(33, 6, 1'000'000).Protected, "other thread cannot consume Main source allocation");
		Check(!Policy.Accept(33, 6, 1'000'000).Protected, "Main-affine allocation remains exact");
	}
}
}

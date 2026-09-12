#pragma once

#include "../src/network/ReliableByteAdmission.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace gargantuan::test {
inline void TestReliableByteAdmission() {
	using namespace network;
	using network::detail::ReliableByteAdmission;
	auto Require = [](bool Condition, const char *Message) { if (!Condition) throw std::runtime_error(Message); };
	auto ProfileFor = [](std::uint64_t Rate, std::uint32_t Peers = 1) {
		ReliableServiceProfile Profile;
		Profile.ConnectionRate = Rate; Profile.AggregateRate = Rate * Peers;
		Profile.BackendRate = 2 * Rate; Profile.MaximumConnections = Peers;
		Profile.GlobalBacklog = Profile.GlobalBurst + 2 * Profile.GameplayBurst * Peers;
		return Profile;
	};
	constexpr ConnectionId First{1, 1}, Second{2, 1};
	constexpr auto Group = MaximumReliableServiceGroupBytes;
	for (auto Rate : {256ull * 1024, 512ull * 1024, 1024ull * 1024, 2ull * 1024 * 1024, 4ull * 1024 * 1024, 8ull * 1024 * 1024}) {
		auto Profile = ProfileFor(Rate);
		Require(Profile.IsValid(), "finite compatibility profile is valid");
		Require(Profile.IsLatencyCompatible() == (Rate == 8ull * 1024 * 1024), "low rate cannot waive full-group latency");
		auto Qualified = Profile; Qualified.RequireLatencyCompatibility = true;
		Require(Qualified.IsValid() == Profile.IsLatencyCompatible(), "requested incompatible class rejects explicitly");
		ReliableByteAdmission Admission(Profile);
		Require(Admission.BeginStep(0) && Admission.Observe(First, 0), "register initial observation");
		Require(Admission.Allowance(First, 0) == 0, "no unearned startup credit");
		for (int Pass = 0; Pass != 100; ++Pass) Require(Admission.Allowance(First, 0) == 0, "passes never refill credit");
		const auto Time = (Group * 1'000'000 + Profile.PeerStructuralRate() - 1) / Profile.PeerStructuralRate();
		Require(Admission.Allowance(First, Time - 1) < Group, "full group waits for elapsed capacity");
		Require(Admission.Allowance(First, Time) == Group, "oversized group accumulates exact bounded credit");
		auto Receipt = Admission.Reserve(First, Group);
		Require(Receipt.has_value() && Admission.GlobalCredit() == 0, "peer and global reservation debit atomically");
		Require(!Admission.BeginStep(Time) && !Admission.Reserve(First, 1), "only one synchronous reservation may be outstanding");
		Require(Admission.Rollback(*Receipt) && !Admission.Rollback(*Receipt), "rollback refunds once only");
		Receipt = Admission.Reserve(First, Group);
		Require(Receipt && Admission.Commit(*Receipt) && !Admission.Commit(*Receipt), "complete group accepts exactly once");
		Require(!Admission.Reserve(First, Group + 1), "no oversized free bypass");
		Require(Admission.GetMetrics().AcceptedBytes == Group && Admission.GetMetrics().RolledBackBytes == Group, "reservation totals remain exact");
		Admission.EndStep();
		Require(Admission.BeginStep(Time + 100'000'000) && Admission.Observe(First, 0), "fresh feedback after long idle");
		Require(Admission.Allowance(First, Time + 100'000'000) == Group, "idle cannot grow a burst beyond ceiling");
		Require(Admission.Allowance(First, Time) == 0, "backwards clock fails closed");
		std::cout << "[Network:ByteAdmission] Rate=" << Rate << " GroupWaitUs=" << Time
			<< " GlobalCreditHighWater=" << Admission.GetMetrics().GlobalCreditHighWater
			<< " CapacityCompatible=" << Profile.IsLatencyCompatible() << '\n';
	}
	for (int Case = 0; Case != 13; ++Case) {
		auto Invalid = ProfileFor(8 * 1024 * 1024);
		switch (Case) {
		case 0: Invalid.ConnectionRate = 0; break;
		case 1: Invalid.AggregateRate = 0; break;
		case 2: Invalid.MaximumConnections = 0; break;
		case 3: Invalid.MaximumConnections = 4097; break;
		case 4: Invalid.StructuralPermille = 751; break;
		case 5: Invalid.StructuralPermille = 0; break;
		case 6: Invalid.PeerBurst = Group - 1; break;
		case 7: Invalid.GlobalBurst = Group - 1; break;
		case 8: Invalid.GameplayBurst = 1; break;
		case 9: Invalid.PeerBacklog = 1; break;
		case 10: Invalid.GlobalBacklog = std::numeric_limits<std::uint64_t>::max(); break;
		case 11: Invalid.BackendRate = std::numeric_limits<std::uint64_t>::max(); break;
		case 12: Invalid.NonQueueAllowanceMilliseconds = 250; break;
		}
		Require(!Invalid.IsValid(), "invalid resource profile fails with actionable diagnostic");
		bool Threw = false;
		try { ReliableByteAdmission Rejected(Invalid); } catch (const std::invalid_argument &) { Threw = true; }
		Require(Threw, "invalid profile cannot instantiate accounting");
	}
	{
		const auto Profile = ProfileFor(8 * 1024 * 1024, 2);
		ReliableByteAdmission Admission(Profile);
		Require(Admission.BeginStep(0) && Admission.Observe(First, 0) && Admission.Observe(Second, 0), "two-peer envelope");
		Require(Admission.Allowance(First, 1'000'000) == Group, "refill peers independently");
		auto Receipt = Admission.Reserve(First, Group);
		Require(Receipt && Admission.Commit(*Receipt), "drain aggregate credit");
		Admission.DeferSize(Second, Group);
		Require(Admission.Allowance(Second, 1'000'000) == 0, "large peer earmarks global refill");
		bool Accepted = false;
		for (std::uint64_t Offset = 1000; Offset <= 50'000; Offset += 1000) {
			Require(Admission.BeginStep(1'000'000 + Offset) && Admission.Observe(First, 0) && Admission.Observe(Second, 0), "bounded repeated observations");
			Require(Admission.Allowance(First, 1'000'000 + Offset) == 0, "small-first competition cannot steal earmarked credit");
			if (Admission.Allowance(Second, 1'000'000 + Offset) >= Group) {
				auto Complete = Admission.Reserve(Second, Group);
				Require(Complete && Admission.Commit(*Complete), "large eligible peer eventually accepts");
				Accepted = true; break;
			}
			Admission.EndStep();
		}
		Require(Accepted, "global wait stays below exact refill plus one observation period");
		Admission.Remove(Second);
		Require(Admission.PeerCount() == 1, "disconnect releases peer cursor and global earmark");
		Require(Admission.BeginStep(2'000'000) && Admission.Observe(First, std::nullopt), "unavailable feedback represented explicitly");
		Require(!Admission.Allowance(First, 2'000'000), "unknown backlog never means zero");
		Require(Admission.BeginStep(3'000'000), "new feedback episode");
		Require(!Admission.Allowance(First, 3'000'000), "missing peer observation cannot hide aggregate backlog");
		Require(Admission.Observe(First, Profile.PeerBacklog), "high backlog observation");
		Require(!Admission.Allowance(First, 3'000'000), "high backlog stops admission");
		Require(Admission.BeginStep(4'000'000) && Admission.Observe(First, Profile.PeerBacklog - Profile.GameplayBurst - 1), "near high-water observation");
		Require(!Admission.Allowance(First, 4'000'000), "hysteresis prevents threshold oscillation");
		Require(Admission.BeginStep(5'000'000) && Admission.Observe(First, Profile.GameplayBurst), "gameplay burst remains while queue recovers");
		Require(Admission.Allowance(First, 5'000'000) == Group, "full legal group fits alongside existing and fresh gameplay headroom");
		Require(!Admission.Observe({First.Slot, 2}, 0), "stale connection slot cannot coexist with replacement");
		Admission.Remove(First);
		Require(Admission.Observe({First.Slot, 2}, 0) && !Admission.Allowance({First.Slot, 2}, 5'000'000), "replacement starts with no old credit");
		Require(!Admission.Reserve(First, 68), "stale generation cannot reserve replacement resources");
	}
	{
		const auto Profile = ProfileFor(8 * 1024 * 1024, 500);
		Require(Profile.IsValid(), "500-peer envelope funds finite aggregate headroom");
		ReliableByteAdmission Admission(Profile);
		Require(Admission.BeginStep(0), "500-peer start");
		for (std::uint32_t Slot = 1; Slot <= 500; ++Slot) Require(Admission.Observe({Slot, 1}, 0), "bounded 500-peer registration");
		std::array<bool, 500> Served{};
		std::uint64_t MaximumWait = 0;
		std::size_t Count = 0;
		for (std::uint64_t Tick = 1; Tick <= 2000 && Count != Served.size(); ++Tick) {
			const auto Time = Tick * 1000;
			Require(Admission.BeginStep(Time), "500-peer clock");
			for (std::uint32_t Slot = 1; Slot <= 500; ++Slot) Require(Admission.Observe({Slot, 1}, 0), "500-peer feedback");
			for (std::uint32_t Offset = 0; Offset != 500; ++Offset) {
				const auto Index = static_cast<std::uint32_t>((Tick + Offset) % 500);
				const ConnectionId Id{Index + 1, 1};
				if (Served[Index]) { Admission.NoWork(Id); continue; }
				Admission.DeferSize(Id, Group);
				if (Admission.Allowance(Id, Time) < Group) continue;
				auto Receipt = Admission.Reserve(Id, Group);
				Require(Receipt && Admission.Commit(*Receipt), "rotating full group accepts");
				Served[Index] = true; ++Count; MaximumWait = Time;
			}
			Admission.EndStep();
		}
		Require(Count == 500 && MaximumWait <= 600'000, "all 500 eligible peers receive finite full-group service");
		Require(Admission.GetMetrics().AcceptedBytes == 500 * Group && Admission.GlobalCredit() <= Profile.GlobalBurst, "aggregate traffic and credit are exact");
		Require(Admission.LogicalBytes() < 128 * 1024, "frontier memory is compact per-connection state, not per object");
		std::cout << "[Network:ByteAdmission] FairPeers=" << Count << " MaxWaitUs=" << MaximumWait
			<< " PeerStates=" << Admission.PeerCount() << " LogicalBytes=" << Admission.LogicalBytes()
			<< " GlobalBacklogLimit=" << Profile.GlobalBacklog << '\n';
	}
	std::cout << "[Network:ByteAdmission] deterministic profiles/credit/fairness/lifecycle/backlog result=pass\n";
}
}

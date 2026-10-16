#pragma once
#include "../cmake/gns/OrdinaryWirePacer.hpp"
#include "../include/gargantuan/network/ReliableServiceProfile.hpp"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

inline bool TestOrdinaryWirePacer() {
	using namespace SteamNetworkingSocketsLib;
	using Permit = GargantuanOrdinaryWirePacer::Permit;
	const gargantuan::network::ReliableServiceProfile Profile = gargantuan::network::ReliableServiceProfile::PooledService();
	const auto Rate = GargantuanOrdinaryWireRate();
	const std::uint64_t NativeMaximum = 1300, WireMaximum = NativeMaximum + 48;
	const std::int64_t Now = 1000;
	const auto Delay = static_cast<std::int64_t>((WireMaximum * 1000000 + Rate - 1) / Rate);
	bool Passed = Rate == Profile.Pooled.BackendCap - Profile.Pooled.MaximumDrainGrants * Profile.BackendSendRate();
	{
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		Permit First, Denied;
		Passed = First.Reserve(Budget, Now) && !First.Reserve(Budget, Now) && Passed;
		Passed = First.Complete(static_cast<int>(NativeMaximum), true) && Passed;
		Passed = !Denied.Reserve(Budget, Now) && Budget.EligibleAt(Now) == Now + Delay && Passed;
		Passed = !Denied.Reserve(Budget, Now + Delay - 1) && Denied.Reserve(Budget, Now + Delay) && Passed;
		Denied.Cancel();
		Passed = Budget.EligibleAt(Now + Delay) == Now + Delay && Passed;
		// Long idle creates one packet's credit, never accumulated grant/session bursts.
		Permit AfterIdle, StillDenied;
		Passed = AfterIdle.Reserve(Budget, Now + 60000000) &&
			AfterIdle.Complete(1300, true) && !StillDenied.Reserve(Budget, Now + 60000000) && Passed;
	}
	{
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		{ Permit Unwound; Passed = Unwound.Reserve(Budget, Now) && Passed; }
		Permit Failed, AckOnly, Data, Next;
		Passed = Failed.Reserve(Budget, Now) && Failed.Complete(0, true) && Passed;
		Passed = AckOnly.Reserve(Budget, Now) && AckOnly.Complete(200, false) && Passed;
		Passed = Data.Reserve(Budget, Now) && Data.Complete(1174, true) && Passed;
		const auto ActualDelay = static_cast<std::int64_t>((1222ULL * 1000000 + Rate - 1) / Rate);
		Passed = Budget.EligibleAt(Now) == Now + ActualDelay && !Next.Reserve(Budget, Now) && Passed;
		// Backward timestamps cannot mint credit or move an existing future deadline.
		Passed = Budget.EligibleAt(Now - 1) == Now + ActualDelay && !Next.Reserve(Budget, 0) && Passed;
		Passed = Next.Reserve(Budget, Now + ActualDelay) && Passed;
		Next.Cancel();
	}
	{
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		Permit Invalid;
		Passed = Invalid.Reserve(Budget, Now) && !Invalid.Complete(1301, true) && Passed;
		Passed = Budget.EligibleAt(Now) == Now + Delay &&
			Budget.EligibleAt(0) == std::numeric_limits<std::int64_t>::max() && Passed;
	}
	{
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		std::atomic<bool> Begin{false};
		std::atomic<int> Granted{0};
		std::vector<std::thread> Threads;
		for (int Index = 0; Index < 8; ++Index) Threads.emplace_back([&] {
			while (!Begin.load(std::memory_order_acquire)) std::this_thread::yield();
			Permit Packet;
			if (Packet.Reserve(Budget, Now)) {
				++Granted;
				if (!Packet.Complete(1300, true)) Granted.store(-100);
			}
		});
		Begin.store(true, std::memory_order_release);
		for (auto &Thread : Threads) Thread.join();
		Passed = Granted == 1 && Budget.EligibleAt(Now) == Now + Delay && Passed;
	}
	// Native attribution is generation-owned. New/FULL/client senders have no
	// marker; own unsent structural work always bypasses this ordinary budget.
	Passed = GargantuanOrdinaryDataIsPaced(true, 0, 7, 0, 0) &&
		GargantuanOrdinaryDataIsPaced(true, 7, 0, 100, 100) &&
		!GargantuanOrdinaryDataIsPaced(true, 7, 0, 100, 99) &&
		!GargantuanOrdinaryDataIsPaced(true, 0, 0, 0, 0) &&
		!GargantuanOrdinaryDataIsPaced(false, 0, 7, 0, 0) && Passed;
	Passed = GargantuanOrdinaryDataDeadline(0, 1001, 1001) == 0 &&
		GargantuanOrdinaryDataDeadline(999, 1001, 1001) == 999 &&
		GargantuanOrdinaryDataDeadline(0, 1001, 1050) == 1050 &&
		GargantuanOrdinaryDataDeadline(1100, 1001, 1050) == 1100 &&
		std::min<std::int64_t>(GargantuanOrdinaryDataDeadline(0, 1001, 1050), 1002) == 1002 && Passed;
	std::fprintf(stderr, "[Gns:OrdinaryPacer] WireCreditRefundClockMarkerConcurrency=%s\n", Passed ? "PASS" : "FAIL");
	return Passed;
}

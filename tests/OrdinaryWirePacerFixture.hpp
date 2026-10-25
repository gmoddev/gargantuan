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
		Passed = Budget.EligibleAt(Now + ActualDelay - 1) == Now + ActualDelay - 1 && Passed;
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
	{
		// Reused native leases do not create 32 unpaced retry senders. Every
		// ACK-retained completed grant shares the existing process wire budget
		// while any of the at-most-four fresh structural grants is running.
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		unsigned Sent = 0;
		for (std::uint64_t Token = 1; Token <= 32; ++Token) {
			Passed = GargantuanOrdinaryDataIsPaced(true, Token, 0, 524288, 524288) &&
				GargantuanOrdinaryDataIsPaced(false, Token, 0, 524288, 524288, true) && Passed;
			Permit Retry;
			if (Retry.Reserve(Budget, Now)) {
				++Sent; Passed = Retry.Complete(1300, true) && Passed;
			}
		}
		Passed = Sent == 1 && Budget.EligibleAt(Now) == Now + Delay &&
			Rate + 4 * Profile.BackendSendRate() == Profile.Pooled.BackendCap && Passed;
	}
	{
		// Concrete candidate counterexample, using the actual production bucket:
		// 31 retained structural retry flows and one protected ordinary flow are
		// all paced alike. Even fair packet rotation cannot prove the 50-ms FIFO
		// allocation after a legal 37324-us structural prefix. This is not a
		// measured GNS/OS run and does not assert the native actor used this order.
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		constexpr std::uint64_t BodyQuantum = gargantuan::network::PooledReliableServiceProfile::ServiceQuantumBytes;
		const auto OrdinaryBytes = Profile.Pooled.PeerGameplayBurst;
		std::uint64_t OrdinarySent = 0, ChargedWire = 0, RetryBody = 0;
		const std::int64_t CompletedStructuralAt = 37'324;
		std::int64_t At = CompletedStructuralAt;
		while (OrdinarySent < OrdinaryBytes) {
			for (unsigned Flow = 0; Flow < 32; ++Flow) {
				At = Budget.EligibleAt(At);
				Permit Packet;
				Passed = Packet.Reserve(Budget, At) && Passed;
				const auto Body = Flow == 31 ? std::min(BodyQuantum, OrdinaryBytes - OrdinarySent) : BodyQuantum;
				const auto Native = Body + NativeMaximum - BodyQuantum;
				Passed = Packet.Complete(static_cast<int>(Native), true) && Passed;
				ChargedWire += Native + 48;
				if (Flow == 31) OrdinarySent += Body; else RetryBody += Body;
			}
		}
		Passed = OrdinarySent == OrdinaryBytes && At > Profile.Pooled.QueueWindowMicroseconds &&
			RetryBody <= 31 * gargantuan::network::MaximumReliableServiceGroupBytes &&
			ChargedWire <= Rate * static_cast<std::uint64_t>(At - CompletedStructuralAt) / 1'000'000 + WireMaximum && Passed;
		std::fprintf(stderr, "[Gns:CapacityCandidate] ordinary_finish_us=%lld fifo_budget_us=50000 retry_body=%llu model_only=1 safe_to_enable=0\n",
			static_cast<long long>(At), static_cast<unsigned long long>(RetryBody));
	}
	Passed = GargantuanOrdinaryDataDeadline(0, 1001, 1001) == 0 &&
		GargantuanOrdinaryDataDeadline(999, 1001, 1001) == 999 &&
		GargantuanOrdinaryDataDeadline(0, 1001, 1050) == 1050 &&
		GargantuanOrdinaryDataDeadline(1100, 1001, 1050) == 1100 &&
		std::min<std::int64_t>(GargantuanOrdinaryDataDeadline(0, 1001, 1050), 1002) == 1002 && Passed;
	{
		GargantuanOrdinaryWirePacer Protected(GargantuanProtectedWireRate, NativeMaximum);
		GargantuanOrdinaryWirePacer Retained(GargantuanRetainedWireRate, NativeMaximum);
		GargantuanOrdinaryWirePacer Aggregate(Rate, NativeMaximum);
		Passed = Protected.Rate() + Retained.Rate() == Rate && Passed;
		// Every retry requester precedes the protected producer. Independent
		// production budgets must prevent this original permitted interference.
		for (unsigned Flow = 1; Flow <= 31; ++Flow) (void)Retained.EligibleAt(37324, Flow);
		std::int64_t At = 37324;
		std::uint64_t Body = 0, Wire = 0;
		unsigned RetryFlow = 1;
		while (Body < Profile.Pooled.PeerGameplayBurst) {
			At = std::max(Aggregate.EligibleAt(At), std::min(Protected.EligibleAt(At, 32), Retained.EligibleAt(At, RetryFlow)));
			Permit Parent, Packet;
			Passed = Parent.Reserve(Aggregate, At) && Passed;
			// Adversarial native actor order: a ready retry takes the parent first.
			if (Retained.EligibleAt(At, RetryFlow) <= At) {
				Passed = Packet.Reserve(Retained, At, RetryFlow) && Packet.Complete(1300, true) && Parent.Complete(1300, true) && Passed;
				(void)Retained.EligibleAt(At, RetryFlow);
				RetryFlow = RetryFlow == 31 ? 1 : RetryFlow + 1;
				Wire += 1348;
			} else {
				Passed = Packet.Reserve(Protected, At, 32) && Passed;
				const auto Bytes = std::min<std::uint64_t>(1248, Profile.Pooled.PeerGameplayBurst - Body);
				Passed = Packet.Complete(int(Bytes + 52), true) && Parent.Complete(int(Bytes + 52), true) && Passed;
				Body += Bytes; Wire += Bytes + 100;
			}
		}
		Passed = At <= 50000 && Wire <= Rate * std::uint64_t(At - 37324) / 1000000 + WireMaximum && Passed;
		std::fprintf(stderr, "[Gns:DataArbitration] historical_ordinary_finish_us=66646 protected_finish_us=%lld model_only=1 execution_service_unproved=1\n", static_cast<long long>(At));
	}
	{
		// Independent child buckets alone expose two initial packets. The parent
		// is mandatory: retain exactly the original one-packet aggregate burst.
		GargantuanOrdinaryWirePacer Aggregate(Rate, NativeMaximum), Protected(GargantuanProtectedWireRate, NativeMaximum), Retained(GargantuanRetainedWireRate, NativeMaximum);
		Permit Parent, Pro, Retry, Denied;
		Passed = Parent.Reserve(Aggregate, Now) && Pro.Reserve(Protected, Now) &&
			Parent.Complete(1300, true) && Pro.Complete(1300, true) && Passed;
		Passed = Retry.Reserve(Retained, Now) && !Denied.Reserve(Aggregate, Now) && Passed;
		Retry.Cancel();
	}
	{
		// FIFO readiness, exact partial-positive debit, stale request removal and
		// 32-slot exhaustion use the production arbiter, including generations.
		GargantuanOrdinaryWirePacer Budget(GargantuanRetainedWireRate, NativeMaximum);
		for (unsigned Flow = 1; Flow <= 32; ++Flow) (void)Budget.EligibleAt(Now, Flow);
		Passed = Budget.EligibleAt(Now, 33) == std::numeric_limits<std::int64_t>::max() && Passed;
		Permit Wrong, First;
		Passed = !Wrong.Reserve(Budget, Now, 2) && First.Reserve(Budget, Now, 1) && First.Complete(17, true) && Passed;
		const auto PartialDelay = (65LL * 1000000 + Budget.Rate() - 1) / Budget.Rate();
		Passed = Budget.EligibleAt(Now, 2) == Now + PartialDelay && Passed;
		for (unsigned Flow = 2; Flow <= 32; ++Flow) Budget.Forget(Flow);
		Permit Reused;
		Passed = Reused.Reserve(Budget, Now + PartialDelay, 0x10001) && Passed;
		Reused.Cancel(); Budget.Forget(1); Budget.Forget(0x10001);
	}
	{
		GargantuanOrdinaryWirePacer Budget(Rate, NativeMaximum);
		(void)Budget.EligibleAt(Now, 1); (void)Budget.EligibleAt(Now, 2); (void)Budget.EligibleAt(Now, 3);
		Budget.Suspend(1);
		Permit Second, Third, Oldest;
		Passed = Second.Reserve(Budget, Now, 2) && Second.Complete(0, true) && Passed;
		// Resuming retains age; pending/native-rate-blocked flows neither hold
		// a ready head nor lose retry priority on every protected packet.
		Passed = Budget.EligibleAt(Now, 1) == Now && !Third.Reserve(Budget, Now, 3) && Oldest.Reserve(Budget, Now, 1) && Passed;
		Oldest.Cancel(); Budget.Forget(3);
	}
	{
		unsigned Quantum = 0;
		for (unsigned Packet = 0; Packet <= GargantuanProtectedPacketQuantum; ++Packet) {
			const auto Class = GargantuanSelectDataClass(false, true, true, true, true, Quantum);
			Passed = Class == (Packet == GargantuanProtectedPacketQuantum ? GargantuanDataClass::Retained : GargantuanDataClass::Protected) && Passed;
			GargantuanCommitDataClass(Quantum, Class, false);
			Passed = Quantum == Packet && Passed; // ACK/failure cannot advance fairness.
			GargantuanCommitDataClass(Quantum, Class, true);
		}
		Passed = Quantum == 0 && GargantuanSelectDataClass(true, true, true, true, true, 5) == GargantuanDataClass::Structural && Passed;
		// Specified 1248-B BODY packetization model for the SHARED source
		// 160-KiB + 256-KiB/s source envelope, 128 + 512/s message envelope.
		const std::uint64_t Bytes = 160 * 1024 + (256 * 1024 * 50 + 999) / 1000;
		const std::uint64_t Messages = 128 + (512 * 50 + 999) / 1000;
		const auto Packets = (Bytes + 1247) / 1248 + Messages;
		const auto Wire = Bytes + Packets * 100;
		// Bound parent interference for EVERY possible positive packet size,
		// including tiny retry packets; no minimum legal BODY is introduced.
		std::uint64_t Numerator = 0, Denominator = 1;
		for (std::uint64_t PacketWire = 49; PacketWire <= WireMaximum; ++PacketWire) {
			const auto ParentTime = (PacketWire * 1000000 + Rate - 1) / Rate;
			const auto RetryTime = (PacketWire * 1000000 + GargantuanRetainedWireRate - 1) / GargantuanRetainedWireRate;
			if (ParentTime * Denominator > Numerator * RetryTime) { Numerator = ParentTime; Denominator = RetryTime; }
		}
		const auto Base = (Wire * 1000000 + GargantuanProtectedWireRate - 1) / GargantuanProtectedWireRate + Packets + Delay;
		const auto Service = (Base * Denominator + Denominator - Numerator - 1) / (Denominator - Numerator);
		Passed = 37325 + Service < 50000 && Passed;
		std::fprintf(stderr, "[Gns:DataArbitration] shared_source_bytes=%llu messages=%llu wire_bound=%llu allocation_service_bound_us=%llu prefix_plus_service_us=%llu model_only=1\n",
			static_cast<unsigned long long>(Bytes), static_cast<unsigned long long>(Messages), static_cast<unsigned long long>(Wire),
			static_cast<unsigned long long>(Service), static_cast<unsigned long long>(37325 + Service));
	}
	std::fprintf(stderr, "[Gns:OrdinaryPacer] WireCreditRefundClockMarkerConcurrency=%s\n", Passed ? "PASS" : "FAIL");
	return Passed;
}

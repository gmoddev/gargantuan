#include <steam/steamnetworkingsockets.h>
#include <steamnetworkingsockets_thinker.h>
#include <clientlib/steamnetworkingsockets_lowlevel.h>
#include <clientlib/steamnetworkingsockets_snp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include "OrdinaryWirePacerFixture.hpp"

// Private exported test controls in this exact pinned socketthread.cpp. They
// keep dispatch on this test thread; production continues using its GNS thread.
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_SetManualPollMode(bool);
STEAMNETWORKINGSOCKETS_INTERFACE void SteamNetworkingSockets_Poll(int);

// Deliberately tests the pinned dependency's private timer contract. This target
// links no Gargantuan runtime and follows GNS's platform RTTI settings.
namespace {
bool ReliableMessageRetirement() {
	using namespace SteamNetworkingSocketsLib;
	SSNPSenderState Sender;
	auto *Message = CSteamNetworkingMessage::New(129);
	if (!Message) return false;
	Message->m_nFlags = k_nSteamNetworkingSend_Reliable;
	Message->m_cbSize = 132; // 129 submitted bytes (including GGNS), 3 private header bytes.
	auto &Info = Message->ReliableSendInfo();
	Info.m_nStreamPos = 1;
	Info.m_cbHdr = 3;
	Info.m_nSentReliableSegRefCount = 2;
	Message->LinkToQueueTail(&CSteamNetworkingMessage::m_links, &Sender.m_unackedReliableMessages);
	const auto First = Sender.m_listSentReliableSegments.AddToTail();
	const auto Last = Sender.m_listSentReliableSegments.AddToTail();
	for (const auto Segment : {First, Last}) {
		auto &Value = Sender.m_listSentReliableSegments[Segment];
		Value.m_pMsg = Message;
		Value.m_nOffset = Segment == First ? 0 : 66;
		Value.m_cbSize = 66;
		Value.m_nRefCount = Segment == First ? 1 : 2; // Last range also has a retry packet reference.
		Value.m_hStatusOrRetry = SNPSendReliableSegment_t::k_nStatus_Acked;
	}
	Sender.GargantuanFeedback.FirstSend(132);
	Sender.GargantuanFeedback.AckSegment(66, false);
	Sender.RemoveRefCountReliableSegment(First);
	bool Passed = Sender.GargantuanFeedback.ReliablePayloadBytesAcked == 0 && Info.m_nSentReliableSegRefCount == 1;
	Sender.GargantuanFeedback.Retransmit(66);
	Sender.GargantuanFeedback.AckSegment(66, false);
	Sender.RemoveRefCountReliableSegment(Last);
	Passed = Passed && Sender.GargantuanFeedback.ReliablePayloadBytesAcked == 0;
	Sender.GargantuanFeedback.AckSegment(66, true);
	Sender.RemoveRefCountReliableSegment(Last); // Exact pinned native final-reference retirement.
	Passed = Passed && Sender.GargantuanFeedback.ReliablePayloadBytesAcked == 129 &&
		Sender.GargantuanFeedback.UniqueReliableStreamBytesAcked == 132 && Sender.m_unackedReliableMessages.empty();
	Sender.Shutdown();
	Passed = Passed && Sender.GargantuanFeedback.Purged && Sender.GargantuanFeedback.ReliablePayloadBytesAcked == 129;
	std::fprintf(stderr, "[Gns:ReliableFeedback] PartialFinalDuplicateRetirement=%s\n", Passed ? "PASS" : "FAIL");
	return Passed;
}
class RepeatingTimer final : public SteamNetworkingSocketsLib::IThinker {
public:
	int Calls = 0;
	SteamNetworkingMicroseconds LastTimestamp = 0;
	bool Monotonic = true;
private:
	void Think(SteamNetworkingMicroseconds Now) override {
		Monotonic = Monotonic && Now >= LastTimestamp;
		LastTimestamp = Now;
		++Calls;
		if (Calls < 3) SetNextThinkTime(Now + 1000);
		// A rescheduled timer becomes due during its own callback. The next
		// socket-service pass must run before dispatching it again.
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
};
class OnceTimer final : public SteamNetworkingSocketsLib::IThinker {
public:
	int Calls = 0;
private:
	void Think(SteamNetworkingMicroseconds) override { ++Calls; }
};
bool PooledSenderQuantumIsolation() {
	GargantuanReliableServiceCounters Counters;
	bool Passed = Counters.SenderPacketQuantum(false) == 0 && Counters.SenderPacketQuantum(true) == 0;
	Counters.ActiveAttributedRetirementToken = 7;
	Passed = Passed && Counters.SenderPacketQuantum(true) == 1 && Counters.SenderPacketQuantum(false) == 0;
	Counters.StructuralActiveGrantBytes = 1258;
	Passed = Passed && Counters.SenderPacketQuantum(false) == 4;
	Counters.StructuralActiveGrantFirstSentBytes = 1135;
	Passed = Passed && Counters.SenderPacketQuantum(true) == 4;
	Counters.StructuralActiveGrantFirstSentBytes = 1258;
	Passed = Passed && Counters.SenderPacketQuantum(true) == 1;
	Counters.StructuralActiveGrantBytes = Counters.StructuralActiveGrantFirstSentBytes = 0;
	Counters.LastAttributedRetirementToken = Counters.ActiveAttributedRetirementToken;
	Counters.ActiveAttributedRetirementToken = 0;
	Passed = Passed && Counters.SenderPacketQuantum(true) == 1;
	Counters = {};
	Passed = Passed && Counters.SenderPacketQuantum(true) == 0;
	std::fprintf(stderr, "[Gns:Fairness] PooledSenderQuantumIsolation=%s\n", Passed ? "PASS" : "FAIL");
	return Passed;
}
class BirthTimer final : public SteamNetworkingSocketsLib::IThinker {
	OnceTimer &Child;
public:
	int Calls = 0;
	explicit BirthTimer(OnceTimer &Input) : Child(Input) {}
private:
	void Think(SteamNetworkingMicroseconds) override { ++Calls; Child.SetNextThinkTimeASAP(); }
};
bool AsapInsertionOrdering() {
	using namespace SteamNetworkingSocketsLib;
	bool Passed = true;
	{
		OnceTimer Before;
		Before.SetNextThinkTimeASAP();
		const auto Inserted = Before.GetNextThinkTime();
		Passed = Passed && Inserted != k_nThinkTime_ASAP && Inserted != k_nThinkTime_Never;
		// Only advance the native microsecond tick if this immediate poll ties it.
		while (SteamNetworkingSockets_GetLocalTimestamp() <= Inserted) {}
		SteamNetworkingSockets_Poll(0);
		Passed = Passed && Before.Calls == 1;
	}
	{
		OnceTimer Child;
		BirthTimer Parent(Child);
		Parent.SetNextThinkTime(SteamNetworkingSockets_GetLocalTimestamp() - 1);
		SteamNetworkingSockets_Poll(0);
		Passed = Passed && Parent.Calls == 1 && Child.Calls == 0;
		const auto Inserted = Child.GetNextThinkTime();
		if (Child.Calls == 0 && Child.IsScheduled())
			while (SteamNetworkingSockets_GetLocalTimestamp() <= Inserted) {}
		SteamNetworkingSockets_Poll(0);
		Passed = Passed && Child.Calls == 1;
	}
	{
		OnceTimer Earlier;
		const auto Deadline = SteamNetworkingSockets_GetLocalTimestamp() - 1;
		Earlier.SetNextThinkTime(Deadline);
		Earlier.EnsureMinThinkTime(k_nThinkTime_ASAP);
		Passed = Passed && Earlier.GetNextThinkTime() == Deadline;
		Earlier.ClearNextThinkTime();
		const auto Future = SteamNetworkingSockets_GetLocalTimestamp() + 1000;
		Earlier.SetNextThinkTime(Future);
		Earlier.EnsureMinThinkTime(Future + 1000);
		Passed = Passed && Earlier.GetNextThinkTime() == Future;
		Earlier.SetNextThinkTime(k_nThinkTime_Never);
		Earlier.EnsureMinThinkTime(k_nThinkTime_Never);
		Passed = Passed && !Earlier.IsScheduled();
	}
	std::fprintf(stderr, "[Gns:Fairness] AsapBirthEarlierFiniteNever=%s\n", Passed ? "PASS" : "FAIL");
	return Passed;
}
}

int main() {
	SteamNetworkingSockets_SetManualPollMode(true);
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) {
		std::fprintf(stderr, "[Gns:Fairness] initialization failed: %s\n", Error);
		return 1;
	}
	bool Passed = TestOrdinaryWirePacer();
	Passed = ReliableMessageRetirement() && Passed;
	Passed = PooledSenderQuantumIsolation() && Passed;
	Passed = AsapInsertionOrdering() && Passed;
	{
		RepeatingTimer First, Second;
		First.SetNextThinkTimeASAP();
		Second.SetNextThinkTimeASAP();
		// Both timers must be strictly due at the pass-start cutoff. ASAP now
		// has a real native timestamp; equality legitimately waits one pass.
		const auto InitialDeadline = std::max(First.GetNextThinkTime(), Second.GetNextThinkTime());
		while (SteamNetworkingSockets_GetLocalTimestamp() <= InitialDeadline) {}
		for (int Pass = 1; Pass <= 3; ++Pass) {
			const auto FirstDue = First.GetNextThinkTime(), SecondDue = Second.GetNextThinkTime();
			const auto Before = SteamNetworkingSockets_GetLocalTimestamp();
			SteamNetworkingSockets_Poll(0);
			const auto After = SteamNetworkingSockets_GetLocalTimestamp();
			const auto Gap = First.LastTimestamp > Second.LastTimestamp
				? First.LastTimestamp - Second.LastTimestamp : Second.LastTimestamp - First.LastTimestamp;
			const bool PassValid = First.Calls == Pass && Second.Calls == Pass && First.Monotonic && Second.Monotonic && Gap >= 1000;
			if (!PassValid) std::fprintf(stderr, "[Gns:Fairness] RepeatingPassFailure pass=%d first_due=%lld second_due=%lld before=%lld after=%lld first_calls=%d second_calls=%d first_last=%lld second_last=%lld first_monotonic=%d second_monotonic=%d gap=%lld\n",
				Pass, static_cast<long long>(FirstDue), static_cast<long long>(SecondDue), static_cast<long long>(Before),
				static_cast<long long>(After), First.Calls, Second.Calls, static_cast<long long>(First.LastTimestamp),
				static_cast<long long>(Second.LastTimestamp), First.Monotonic, Second.Monotonic, static_cast<long long>(Gap));
			Passed = Passed && PassValid;
		}
		First.ClearNextThinkTime();
		Second.ClearNextThinkTime();
		SteamNetworkingSockets_Poll(0);
		Passed = Passed && First.Calls == 3 && Second.Calls == 3;
	}
	GameNetworkingSockets_Kill();
	SteamNetworkingSockets_SetManualPollMode(false);
	std::fprintf(stderr, "[Gns:Fairness] %s\n", Passed ? "passed" : "failed");
	return Passed ? 0 : 1;
}

#include <steam/steamnetworkingsockets.h>
#include <steamnetworkingsockets_thinker.h>
#include <clientlib/steamnetworkingsockets_snp.h>
#include <chrono>
#include <cstdio>
#include <thread>

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
}

int main() {
	SteamNetworkingSockets_SetManualPollMode(true);
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) {
		std::fprintf(stderr, "[Gns:Fairness] initialization failed: %s\n", Error);
		return 1;
	}
	bool Passed = ReliableMessageRetirement();
	{
		RepeatingTimer First, Second;
		First.SetNextThinkTimeASAP();
		Second.SetNextThinkTimeASAP();
		for (int Pass = 1; Pass <= 3; ++Pass) {
			SteamNetworkingSockets_Poll(0);
			Passed = Passed && First.Calls == Pass && Second.Calls == Pass;
			Passed = Passed && First.Monotonic && Second.Monotonic;
			const auto Gap = First.LastTimestamp > Second.LastTimestamp
				? First.LastTimestamp - Second.LastTimestamp : Second.LastTimestamp - First.LastTimestamp;
			Passed = Passed && Gap >= 1000;
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

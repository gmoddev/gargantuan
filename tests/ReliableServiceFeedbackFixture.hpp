#pragma once
#include "../src/network/ReliableServiceFeedback.hpp"
#include "../cmake/gns/ReliableServiceFeedback.hpp"
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>
#include <stdexcept>

// Included after the existing real-transport pair fixture. Shares its bounded
// localhost setup and pump, not the reference model's implementation.
namespace FeedbackFixture {
using namespace gargantuan::network;
using Feedback = detail::ReliableServiceFeedback;
using Access = detail::ReliableServiceFeedbackAccess;
using namespace std::chrono_literals;

inline void Require(bool Condition, const char *Text) {
	if (!Condition) throw std::runtime_error(Text);
}
struct OwnedPair {
	PairFixture Pair = StartPair({.MaximumConnections = 1, .SendRate = 16 * 1024 * 1024});
	~OwnedPair() { StopPair(Pair); }
};
struct Impairment {
	Impairment() = default;
	~Impairment() {
		SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0);
		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, 0);
		SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Recv, 0);
	}
};
inline Feedback Sample(PairFixture &Pair) {
	const auto Value = Access::Observe(*Pair.Server, Pair.ServerConnection);
	Require(Value.has_value(), "coherent feedback available");
	Require(Value->State == ConnectionState::Connected && Value->Connection == Pair.ServerConnection && Value->UniqueReliableStreamBytesAcked <=
		Value->UniqueReliableStreamBytesFirstSent, "generation/unique progress invariant");
	return *Value;
}
inline void Monotonic(const Feedback &Before, const Feedback &After) {
	Require(Before.Connection == After.Connection && Before.ObservedAtMicroseconds <= After.ObservedAtMicroseconds &&
		Before.UniqueReliableStreamBytesFirstSent <= After.UniqueReliableStreamBytesFirstSent &&
		Before.UniqueReliableStreamBytesAcked <= After.UniqueReliableStreamBytesAcked &&
		Before.ReliablePayloadBytesAcked <= After.ReliablePayloadBytesAcked &&
		Before.ReliableStreamBytesRetransmitted <= After.ReliableStreamBytesRetransmitted,
		"same-generation snapshot is monotonic");
}
inline void Send(PairFixture &Pair, std::size_t PayloadBytes) {
	auto Intent = Message(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
		std::vector<std::byte>(PayloadBytes, std::byte{0x37}), Pair.Limits);
	Require(Intent && Pair.Server->Send(*Intent).Succeeded(), "known payload accepted");
}
inline Feedback Complete(PairFixture &Pair, std::uint64_t ExpectedPayload) {
	auto Previous = Sample(Pair);
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 8s, [&] {
		const auto Current = Sample(Pair);
		Monotonic(Previous, Current); Previous = Current;
		return Current.ReliablePayloadBytesAcked == ExpectedPayload &&
			!Current.PendingReliableStreamBytes && !Current.SentUnackedReliableStreamBytes;
	});
	const auto Final = Sample(Pair);
	Require(Final.ReliablePayloadBytesAcked == ExpectedPayload &&
		Final.UniqueReliableStreamBytesAcked == Final.UniqueReliableStreamBytesFirstSent &&
		!Final.PendingReliableStreamBytes && !Final.SentUnackedReliableStreamBytes,
		"complete ACK retirement matches exact submitted payload plus GGNS envelope");
	return Final;
}

struct NativePair {
	// Keep Gargantuan's normal adapter owner alive so the shared GNS library and
	// service thread are initialized through the production path.
	OwnedPair Runtime;
	ISteamNetworkingSockets *Sockets = SteamNetworkingSockets();
	HSteamNetConnection Sender = k_HSteamNetConnection_Invalid;
	HSteamNetConnection Receiver = k_HSteamNetConnection_Invalid;
	NativePair() {
		Require(Sockets != nullptr, "native GNS interface available");
		Require(Sockets->CreateSocketPair(&Sender, &Receiver, true, nullptr, nullptr),
			"native network-loopback socket pair created");
	}
	~NativePair() {
		if (Sockets && Sender != k_HSteamNetConnection_Invalid)
			(void)Sockets->CloseConnection(Sender, 0, "attribution test cleanup", false);
		if (Sockets && Receiver != k_HSteamNetConnection_Invalid)
			(void)Sockets->CloseConnection(Receiver, 0, "attribution test cleanup", false);
	}
};

inline GargantuanReliableServiceSnapshot NativeSample(NativePair &Pair) {
	GargantuanReliableServiceSnapshot Result;
	Require(SteamNetworkingSocketsLib::GargantuanGetReliableServiceFeedback(
		Pair.Sockets, static_cast<std::uint32_t>(Pair.Sender), Result), "native attributed feedback available");
	return Result;
}

inline std::int64_t NativeSend(NativePair &Pair, std::size_t Bytes, std::uint64_t Token = 0) {
	std::vector<std::byte> Payload(Bytes, std::byte{0x5a});
	if (Token) Require(SteamNetworkingSocketsLib::GargantuanBeginReliableRetirementAttribution(Token),
		"attribution scope begins");
	int64 MessageNumber = -1; // GNS's output-pointer type differs from std::int64_t on Linux.
	const auto Result = Pair.Sockets->SendMessageToConnection(
		Pair.Sender, Payload.data(), static_cast<std::uint32_t>(Payload.size()),
		k_nSteamNetworkingSend_Reliable, &MessageNumber);
	if (Token) SteamNetworkingSocketsLib::GargantuanEndReliableRetirementAttribution();
	Require(Result == k_EResultOK && MessageNumber > 0, "native reliable message accepted with identity");
	return MessageNumber;
}

inline GargantuanReliableServiceSnapshot WaitNativeRetirement(NativePair &Pair, std::uint64_t Sequence) {
	GargantuanReliableServiceSnapshot Current;
	const auto Deadline = std::chrono::steady_clock::now() + 8s;
	while (std::chrono::steady_clock::now() < Deadline) {
		Pair.Sockets->RunCallbacks();
		for (;;) {
			SteamNetworkingMessage_t *MessageValue = nullptr;
			const auto Count = Pair.Sockets->ReceiveMessagesOnConnection(Pair.Receiver, &MessageValue, 1);
			if (Count <= 0) break;
			if (MessageValue) MessageValue->Release();
		}
		Current = NativeSample(Pair);
		if (Current.Counters.AttributedRetirementSequence >= Sequence) return Current;
		std::this_thread::sleep_for(1ms);
	}
	return NativeSample(Pair);
}

inline bool Run() {
	std::size_t Passed = 0;
	auto Case = [&](const char *Name, auto Function) {
		try { Function(); ++Passed; std::cout << "[Network:ReliableFeedback] " << Name << "=PASS\n"; }
		catch (const std::exception &Error) {
			std::cerr << "[Network:ReliableFeedback] " << Name << "=FAIL " << Error.what() << '\n';
		}
	};
	Case("FirstSendAndMessageBoundaries", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair;
		Require(Pair.ServerConnection.IsValid(), "pair connected");
		const auto Initial = Sample(Pair);
		Require(!Initial.UniqueReliableStreamBytesFirstSent && !Initial.UniqueReliableStreamBytesAcked &&
			!Initial.ReliablePayloadBytesAcked && !Initial.ReliableStreamBytesRetransmitted, "new native generation starts at zero");
		std::uint64_t Expected = 0;
		for (const std::size_t Size : {std::size_t{97}, std::size_t{64 * 1024}, std::size_t{524288 - 32}}) {
			Send(Pair, Size); Expected += Size + 32;
			const auto Final = Complete(Pair, Expected);
			Require(!Final.ReliableStreamBytesRetransmitted && Final.UniqueReliableStreamBytesFirstSent > Expected,
				"no-loss first send includes private stream headers and no retries");
		}
		for (int Index = 0; Index < 3; ++Index) { Send(Pair, 1024 + Index); Expected += 1056 + Index; }
		const auto Final = Complete(Pair, Expected);
		Require(Payloads(Pair.ClientEvents).size() == 6, "all complete message boundaries delivered");
		std::cout << "[Network:ReliableFeedback] payload_acked=" << Final.ReliablePayloadBytesAcked
			<< " unique_stream=" << Final.UniqueReliableStreamBytesAcked << '\n';
	});
	Case("RepeatedRetransmission", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair; Impairment Settings;
		Require(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 100), "loss injection enabled");
		Send(Pair, 16 * 1024);
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
			const auto Value = Sample(Pair);
			return Value.UniqueReliableStreamBytesFirstSent &&
				Value.ReliableStreamBytesRetransmitted >= 2 * Value.UniqueReliableStreamBytesFirstSent;
		});
		const auto Lost = Sample(Pair);
		Require(!Lost.ReliablePayloadBytesAcked && !Lost.UniqueReliableStreamBytesAcked &&
			Lost.ReliableStreamBytesRetransmitted >= 2 * Lost.UniqueReliableStreamBytesFirstSent &&
			Lost.UniqueReliableStreamBytesFirstSent > 16 * 1024,
			"repeated retry adds physical cost without unique first-send or ACK progress");
		Require(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0), "loss removed");
		const auto Final = Complete(Pair, 16 * 1024 + 32);
		Require(Final.UniqueReliableStreamBytesFirstSent == Lost.UniqueReliableStreamBytesFirstSent,
			"ACK after retry counts the unique range once");
		std::cout << "[Network:ReliableFeedback] retry_bytes=" << Final.ReliableStreamBytesRetransmitted << '\n';
	});
	Case("DelayedAck", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair; Impairment Settings;
		Require(SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Recv, 200), "delay enabled");
		Send(Pair, 64 * 1024);
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 60ms, [] { return false; });
		const auto Waiting = Sample(Pair);
		Require(Waiting.UniqueReliableStreamBytesFirstSent && !Waiting.ReliablePayloadBytesAcked &&
			Waiting.PendingReliableStreamBytes + Waiting.SentUnackedReliableStreamBytes,
			"configured 16 MiB/s cannot manufacture ACK progress during recipient delay");
		const auto Final = Complete(Pair, 64 * 1024 + 32); Monotonic(Waiting, Final);
	});
	Case("DuplicatePacketsAndAcks", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair; Impairment Settings;
		Require(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Recv, 100), "duplicate packet injection enabled");
		Send(Pair, 32 * 1024);
		const auto Final = Complete(Pair, 32 * 1024 + 32);
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 250ms, [] { return false; });
		const auto Later = Sample(Pair); Monotonic(Final, Later);
		Require(Later.ReliablePayloadBytesAcked == Final.ReliablePayloadBytesAcked &&
			Later.UniqueReliableStreamBytesAcked == Final.UniqueReliableStreamBytesAcked &&
			Payloads(Pair.ClientEvents).size() == 1, "duplicate traffic cannot double-retire stream or payload");
	});
	Case("TeardownAndGenerationReset", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair; Impairment Settings;
		Require(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 100), "loss enabled");
		Send(Pair, 64 * 1024);
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 40ms, [] { return false; });
		const auto Before = Sample(Pair);
		Require(!Before.ReliablePayloadBytesAcked && Before.UniqueReliableStreamBytesFirstSent, "outstanding data exists");
		const auto OldId = Pair.ServerConnection;
		Require(Pair.Server->Disconnect(OldId, {DisconnectReason::LocalShutdown, "Feedback terminal test"}).Succeeded(), "disconnect succeeds");
		const auto Terminal = Access::Observe(*Pair.Server, OldId);
		Require(Terminal && Terminal->State == ConnectionState::Closed && !Terminal->ReliablePayloadBytesAcked &&
			!Terminal->PendingReliableStreamBytes && !Terminal->SentUnackedReliableStreamBytes, "purge has terminal snapshot without false ACK");
		Monotonic(Before, *Terminal);
		Require(SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0), "loss removed");
		Pair.Client->Stop({DisconnectReason::LocalShutdown, "Replace feedback client"});
		Pair.Client = std::make_unique<GameNetworkingSocketsTransport>();
		Pair.ServerEvents.clear(); Pair.ClientEvents.clear();
		Require(Pair.Client->Start({.Role = TransportRole::Client, .Endpoint = {"127.0.0.1", Pair.Port}, .AdvertisedLimits = Pair.Limits}).Succeeded(), "replacement starts");
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
			return ConnectedId(Pair.ServerEvents).IsValid() && ConnectedId(Pair.ClientEvents).IsValid();
		});
		Pair.ServerConnection = ConnectedId(Pair.ServerEvents);
		Pair.ClientConnection = ConnectedId(Pair.ClientEvents);
		Require(Pair.ServerConnection.Slot == OldId.Slot && Pair.ServerConnection.Generation > OldId.Generation,
			"adapter slot reused with a fresh generation");
		const auto New = Sample(Pair);
		Require(!New.UniqueReliableStreamBytesFirstSent && !New.UniqueReliableStreamBytesAcked &&
			!New.ReliablePayloadBytesAcked && !New.ReliableStreamBytesRetransmitted &&
			!Access::Observe(*Pair.Server, OldId), "replacement has zero counters and rejects old feedback identity");
	});
	Case("MixedRetirementAttribution", [] {
		std::size_t AttributionPassed = 0;
		auto RetirementCase = [&](const char *Name, auto Body) {
			Body(); ++AttributionPassed;
			std::cout << "[Network:ReliableAttribution] " << Name << "=PASS\n";
		};
		auto Retire = [](GargantuanReliableServiceCounters &Value, std::int64_t Message, int Payload) {
			Value.AckMessage(Message, Payload + 3, 3);
		};
		RetirementCase("StructuralThenGameplay_GameplayRetiresFirst", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(1, 10);
			Retire(Value, 11, 100); Require(!Value.AttributedRetirementSequence && Value.ActiveAttributedRetirementToken == 1, "gameplay cannot retire structural A");
			Retire(Value, 10, 512); Require(Value.AttributedRetirementSequence == 1 && Value.LastAttributedRetirementToken == 1 && Value.LastAttributedRetiredPayloadBytes == 512, "structural A retires itself");
		});
		RetirementCase("GameplayThenStructural_StructuralRetiresFirst", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(2, 21);
			Retire(Value, 21, 256); Retire(Value, 20, 64);
			Require(Value.AttributedRetirementSequence == 1 && Value.LastAttributedRetirementToken == 2 && Value.LastAttributedRetirementMessageNumber == 21, "later structural B owns its own retirement");
		});
		RetirementCase("AlternatingStructuralGameplayControl", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(3, 30); Retire(Value, 31, 80); Retire(Value, 32, 40);
			Require(!Value.AttributedRetirementSequence, "ordinary classes do not advance structural retirement"); Retire(Value, 30, 300);
			Value.AttributeMessage(4, 33); Retire(Value, 34, 90); Retire(Value, 33, 301);
			Require(Value.AttributedRetirementSequence == 2 && Value.LastAttributedRetirementToken == 4, "alternating classes remain isolated");
		});
		RetirementCase("TwoStructuralGrantsWithGameplayBetween", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(5, 40); Retire(Value, 40, 400); Retire(Value, 41, 75);
			Value.AttributeMessage(6, 42); Retire(Value, 42, 401);
			Require(Value.AttributedRetirementSequence == 2 && Value.LastAttributedRetirementToken == 6 && Value.ReliablePayloadBytesAcked == 876, "sequential grant ownership is exact");
		});
		RetirementCase("StructuralRetransmission", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(7, 50); Value.FirstSend(512); Value.Retransmit(512); Value.Retransmit(512); Value.AckSegment(512, false); Retire(Value, 50, 500);
			Require(Value.AttributedRetirementSequence == 1 && Value.LastAttributedRetiredPayloadBytes == 500 && Value.ReliableStreamBytesRetransmitted == 1024, "structural retry increases physical cost only");
		});
		RetirementCase("GameplayRetransmissionIgnored", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(8, 60); Value.Retransmit(300); Retire(Value, 61, 250);
			Require(!Value.AttributedRetirementSequence && Value.ActiveAttributedRetirementToken == 8 && Value.ReliableStreamBytesRetransmitted == 300, "gameplay retry cannot manufacture structural drain"); Retire(Value, 60, 260);
		});
		RetirementCase("DelayedAck", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(9, 70);
			Require(Value.ActiveAttributedRetirementToken == 9 && !Value.AttributedRetirementSequence, "time without retirement retains active debt"); Retire(Value, 70, 270);
		});
		RetirementCase("DuplicateAck", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(10, 80); Value.FirstSend(100); Value.AckSegment(100, false); Value.AckSegment(100, true); Retire(Value, 80, 280);
			Require(Value.AttributedRetirementSequence == 1 && Value.UniqueReliableStreamBytesAcked == 100, "duplicate segment ACK does not duplicate logical retirement");
		});
		RetirementCase("MultiSegmentStructural", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(11, 90); Value.FirstSend(200); Value.FirstSend(300); Value.AckSegment(200, false);
			Require(!Value.AttributedRetirementSequence, "partial segment ACK cannot retire message"); Value.AckSegment(300, false); Retire(Value, 90, 490);
			Require(Value.AttributedRetirementSequence == 1 && Value.LastAttributedRetiredPayloadBytes == 490, "final message reference retires once after all segments");
		});
		RetirementCase("SharedPacketMessageIsolation", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(12, 100); Value.FirstSend(200); Value.AckSegment(200, false);
			Retire(Value, 101, 50); Require(!Value.AttributedRetirementSequence, "co-processed unrelated message cannot retire attributed message"); Retire(Value, 100, 150);
		});
		RetirementCase("MixedOutstandingTerminalTeardown", [&] {
			GargantuanReliableServiceCounters Value; Value.AttributeMessage(13, 110); Retire(Value, 111, 60); Value.Purged = true;
			Require(!Value.AttributedRetirementSequence && Value.ActiveAttributedRetirementToken == 13 && Value.Purged, "terminal purge retains unretired structural identity for terminal release");
		});
		RetirementCase("ReconnectGenerationIsolation", [&] {
			GargantuanReliableServiceCounters Old; Old.AttributeMessage(14, 120); GargantuanReliableServiceCounters Fresh;
			Retire(Old, 120, 320); Require(Old.AttributedRetirementSequence == 1 && !Fresh.AttributedRetirementSequence && !Fresh.ActiveAttributedRetirementToken, "old generation retirement cannot mutate fresh sender state");
		});
		Require(AttributionPassed == 12, "complete mixed-retirement attribution matrix passed");
	});
	Case("RealGnsAttributedRetirement", [] {
		NativePair Pair;
		constexpr std::uint64_t Token = 41;
		const auto MessageNumber = NativeSend(Pair, 48 * 1024, Token);
		const auto Active = NativeSample(Pair);
		Require(Active.Counters.ActiveAttributedRetirementToken == Token &&
			Active.Counters.ActiveAttributedMessageNumber == static_cast<std::uint64_t>(MessageNumber) &&
			!Active.Counters.AttributedRetirementSequence, "real GNS send binds token to assigned message number before service");
		const auto Retired = WaitNativeRetirement(Pair, 1);
		Require(Retired.Counters.AttributedRetirementSequence == 1 &&
			Retired.Counters.LastAttributedRetirementToken == Token &&
			Retired.Counters.LastAttributedRetirementMessageNumber == static_cast<std::uint64_t>(MessageNumber) &&
			Retired.Counters.LastAttributedRetiredPayloadBytes == 48 * 1024 &&
			!Retired.Counters.ActiveAttributedRetirementToken,
			"real GNS final-reference retirement reports exact sender-local identity and payload once");
		const auto BaselineSequence = Retired.Counters.AttributedRetirementSequence;
		(void)NativeSend(Pair, 4 * 1024);
		const auto Deadline = std::chrono::steady_clock::now() + 2s;
		GargantuanReliableServiceSnapshot After = Retired;
		while (std::chrono::steady_clock::now() < Deadline) {
			Pair.Sockets->RunCallbacks();
			SteamNetworkingMessage_t *MessageValue = nullptr;
			if (Pair.Sockets->ReceiveMessagesOnConnection(Pair.Receiver, &MessageValue, 1) > 0 && MessageValue) MessageValue->Release();
			After = NativeSample(Pair);
			if (!After.PendingReliableStreamBytes && !After.SentUnackedReliableStreamBytes) break;
			std::this_thread::sleep_for(1ms);
		}
		Require(After.Counters.AttributedRetirementSequence == BaselineSequence &&
			After.Counters.LastAttributedRetirementToken == Token,
			"unattributed reliable traffic cannot overwrite structural retirement identity");
	});
	Case("CheckedCounterExhaustion", [] {
		const auto Maximum = std::numeric_limits<std::uint64_t>::max();
		GargantuanReliableServiceCounters First;
		First.UniqueReliableStreamBytesFirstSent = Maximum - 1; First.FirstSend(2);
		Require(First.Invalid && First.UniqueReliableStreamBytesFirstSent == Maximum - 1, "first-send overflow invalidates without wrapping");
		First.FirstSend(1); Require(First.UniqueReliableStreamBytesFirstSent == Maximum - 1, "invalid state is sticky");
		GargantuanReliableServiceCounters Retry;
		Retry.ReliableStreamBytesRetransmitted = Maximum; Retry.Retransmit(1);
		Require(Retry.Invalid && Retry.ReliableStreamBytesRetransmitted == Maximum, "retry overflow invalidates");
		GargantuanReliableServiceCounters Payload;
		Payload.ReliablePayloadBytesAcked = Maximum; Payload.AckMessage(1, 33, 1);
		Require(Payload.Invalid && Payload.ReliablePayloadBytesAcked == Maximum, "payload overflow invalidates");
		GargantuanReliableServiceCounters Ack;
		Ack.FirstSend(100); Ack.AckSegment(100, false); Ack.AckSegment(100, true);
		Require(!Ack.Invalid && Ack.UniqueReliableStreamBytesAcked == 100, "duplicate native ACK transition does not double count");
		Ack.AckSegment(1, false); Require(Ack.Invalid, "ACK cannot exceed unique first-send");
		GargantuanReliableServiceCounters Negative; Negative.FirstSend(-1); Require(Negative.Invalid, "negative native range fails conservatively");
		GargantuanReliableServiceCounters TokenReuse; TokenReuse.AttributeMessage(10, 1); TokenReuse.AckMessage(1, 10, 0); TokenReuse.AttributeMessage(10, 2);
		Require(TokenReuse.Invalid, "attribution token cannot alias within one sender generation");
		GargantuanReliableServiceCounters SequenceOverflow; SequenceOverflow.AttributedRetirementSequence = Maximum; SequenceOverflow.AttributeMessage(11, 3);
		Require(SequenceOverflow.Invalid && !SequenceOverflow.ActiveAttributedRetirementToken, "retirement sequence exhaustion fails before a new attributed obligation");
		GargantuanReliableServiceCounters Concurrent; Concurrent.AttributeMessage(12, 4); Concurrent.AttributeMessage(13, 5);
		Require(Concurrent.Invalid, "more than one active attributed obligation per connection fails closed");
		Require(SteamNetworkingSocketsLib::GargantuanBeginReliableRetirementAttribution(20), "thread-local attribution token accepted");
		Require(!SteamNetworkingSocketsLib::GargantuanBeginReliableRetirementAttribution(21), "nested attribution scope rejected");
		SteamNetworkingSocketsLib::GargantuanEndReliableRetirementAttribution();
		GargantuanReliableServiceSnapshot Native;
		const auto Before = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		SteamNetworkingSocketsLib::GargantuanCopyReliableServiceFeedback(First, 0, 0, 0, Native);
		const auto After = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		Require(Native.Counters.Invalid && Native.ObservedAtMicroseconds >= static_cast<std::uint64_t>(Before) &&
			Native.ObservedAtMicroseconds <= static_cast<std::uint64_t>(After), "native copy preserves invalidity and stamps its own observation time");
	});
	Case("AdapterAttributedQueueAndTerminalLease", [] {
		auto Pair = StartPair({.MaximumConnections = 1, .SendRate = 16 * 1024 * 1024}, TestLimits(), true);
		struct Cleanup { PairFixture &Value; ~Cleanup() { StopPair(Value); } } Guard{Pair};
		Require(Pair.ServerConnection.IsValid(), "leased pair connected");
		constexpr std::size_t StructuralBytes = 512 * 1024 - 32;
		constexpr std::size_t GameplayBytes = 16 * 1024;
		auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence{1}},
			std::vector<std::byte>(StructuralBytes, std::byte{0x31}), Pair.Limits);
		Require(Intent && Access::Attribute(*Intent, 81), "private queued attribution attached");
		NetworkScheduler Scheduler(*Pair.Server);
		Require(Scheduler.RegisterConnection(Pair.ServerConnection, Pair.Limits) && Scheduler.Submit(std::move(*Intent)).Accepted(), "scheduler acceptance");
		Require(!Sample(Pair).ActiveAttributedRetirementToken, "queued work has not reached native sender");
		Require(Scheduler.Flush(Pair.ServerConnection, SchedulerTickBudget::FromNetworkLimits(Pair.Limits)).MessagesSubmitted == 1, "existing scheduler handoff");
		auto Gameplay = Message(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
			std::vector<std::byte>(GameplayBytes, std::byte{0x42}), Pair.Limits);
		Require(Gameplay && Scheduler.Submit(std::move(*Gameplay)).Accepted() &&
			Scheduler.Flush(Pair.ServerConnection, SchedulerTickBudget::FromNetworkLimits(Pair.Limits)).MessagesSubmitted == 1,
			"ordinary gameplay follows the maximum complete structural group in the same reliable FIFO");
		const auto Final = Complete(Pair, StructuralBytes + GameplayBytes + 64);
		Require(Final.AttributedRetirementSequence == 1 && Final.LastAttributedRetirementToken == 81 &&
			Final.LastAttributedRetiredPayloadBytes == StructuralBytes + 32 && Final.LastAttributedRetirementMessageNumber,
			"adapter preserves exact native message receipt");
		const auto Received = Payloads(Pair.ClientEvents);
		Require(Received.size() == 2 && Received[0] == std::vector<std::byte>(StructuralBytes, std::byte{0x31}) &&
			Received[1] == std::vector<std::byte>(GameplayBytes, std::byte{0x42}), "complete G and gameplay preserve FIFO bytes");
		const auto Old = Pair.ServerConnection;
		Require(Pair.Server->Disconnect(Old, {DisconnectReason::LocalShutdown, "leased terminal test"}).Succeeded(), "purge");
		const auto Terminal = Access::Observe(*Pair.Server, Old);
		Require(Terminal && Terminal->State == ConnectionState::Closed && Terminal->LastAttributedRetirementToken == 81,
			"closed generation retains its receipt");
		Require(!Access::Release(*Pair.Server, {Old.Slot, Old.Generation + 1}), "wrong generation cannot consume terminal");
		Require(Access::Release(*Pair.Server, Old) && !Access::Release(*Pair.Server, Old) && !Access::Observe(*Pair.Server, Old),
			"terminal consumed exactly once before reuse");
	});
	Case("SnapshotOverhead", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair;
		const auto Before = Sample(Pair);
		constexpr std::size_t Count = 10000;
		const auto Start = std::chrono::steady_clock::now();
		for (std::size_t Index = 0; Index < Count; ++Index) (void)Sample(Pair);
		const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count();
		Monotonic(Before, Sample(Pair));
		constexpr std::size_t AttributionScalarBytes = 6 * sizeof(std::uint64_t);
		std::cout << "[Network:ReliableFeedback] native_counter_bytes=" << sizeof(GargantuanReliableServiceCounters)
			<< " attribution_scalar_bytes=" << AttributionScalarBytes
			<< " option_c_32_peer_attribution_bytes=" << AttributionScalarBytes * 32
			<< " native_4096_connection_attribution_bytes=" << AttributionScalarBytes * 4096
			<< " feedback_bytes=" << sizeof(Feedback) << " terminal_slot_bytes=" << sizeof(std::optional<Feedback>)
			<< " terminal_vector_bytes=" << sizeof(std::vector<std::optional<Feedback>>)
			<< " snapshot_mean_ns=" << Elapsed / Count << " samples=" << Count << '\n';
	});
	std::cout << "[Network:ReliableFeedback] passed=" << Passed << " total=10\n";
	return Passed == 10;
}
}

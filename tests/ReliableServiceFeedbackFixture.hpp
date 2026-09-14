#pragma once
#include "../src/network/ReliableServiceFeedback.hpp"
#include "../cmake/gns/ReliableServiceFeedback.hpp"
#include <steam/isteamnetworkingutils.h>
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
		Payload.ReliablePayloadBytesAcked = Maximum; Payload.AckMessage(33, 1);
		Require(Payload.Invalid && Payload.ReliablePayloadBytesAcked == Maximum, "payload overflow invalidates");
		GargantuanReliableServiceCounters Ack;
		Ack.FirstSend(100); Ack.AckSegment(100, false); Ack.AckSegment(100, true);
		Require(!Ack.Invalid && Ack.UniqueReliableStreamBytesAcked == 100, "duplicate native ACK transition does not double count");
		Ack.AckSegment(1, false); Require(Ack.Invalid, "ACK cannot exceed unique first-send");
		GargantuanReliableServiceCounters Negative; Negative.FirstSend(-1); Require(Negative.Invalid, "negative native range fails conservatively");
		GargantuanReliableServiceSnapshot Native;
		const auto Before = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		SteamNetworkingSocketsLib::GargantuanCopyReliableServiceFeedback(First, 0, 0, 0, Native);
		const auto After = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		Require(Native.Counters.Invalid && Native.ObservedAtMicroseconds >= static_cast<std::uint64_t>(Before) &&
			Native.ObservedAtMicroseconds <= static_cast<std::uint64_t>(After), "native copy preserves invalidity and stamps its own observation time");
	});
	Case("SnapshotOverhead", [] {
		OwnedPair Owner; auto &Pair = Owner.Pair;
		const auto Before = Sample(Pair);
		constexpr std::size_t Count = 10000;
		const auto Start = std::chrono::steady_clock::now();
		for (std::size_t Index = 0; Index < Count; ++Index) (void)Sample(Pair);
		const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count();
		Monotonic(Before, Sample(Pair));
		std::cout << "[Network:ReliableFeedback] native_counter_bytes=" << sizeof(GargantuanReliableServiceCounters)
			<< " feedback_bytes=" << sizeof(Feedback) << " terminal_slot_bytes=" << sizeof(std::optional<Feedback>)
			<< " terminal_vector_bytes=" << sizeof(std::vector<std::optional<Feedback>>)
			<< " snapshot_mean_ns=" << Elapsed / Count << " samples=" << Count << '\n';
	});
	std::cout << "[Network:ReliableFeedback] passed=" << Passed << " total=7\n";
	return Passed == 7;
}
}

#pragma once

#include "../src/network/FiniteGrantServiceCurve.hpp"
#include "../src/network/GnsAckDiagnosticsAccess.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Included after the production-adapter PairFixture. These tests do not replace
// admission/fairness qualification: they exercise the adapter's exact pooled
// token projection and the un-tokened FULL_RESERVATION projection on real GNS.
namespace GnsFundedAckCompatibilityFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;
using Feedback = detail::ReliableServiceFeedback;
using Access = detail::ReliableServiceFeedbackAccess;
using AckAccess = detail::GnsAckDiagnosticsAccess;

inline void Require(bool Condition, const char *Text) {
	if (!Condition) throw std::runtime_error(Text);
}
inline std::uint64_t Now() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct OwnedPair {
	PairFixture Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
	~OwnedPair() { StopPair(Pair); }
};
struct WireSnapshot {
	Feedback Sender, Receiver;
};
inline WireSnapshot ReadWire(PairFixture &Pair) {
	const auto Sender = Access::Observe(*Pair.Server, Pair.ServerConnection);
	const auto Receiver = Access::Observe(*Pair.Client, Pair.ClientConnection);
	Require(Sender && Receiver && Sender->CountersValid && Receiver->CountersValid,
		"both endpoint packet counters must be valid");
	Require(Sender->Connection == Pair.ServerConnection && Receiver->Connection == Pair.ClientConnection &&
		Sender->State == ConnectionState::Connected && Receiver->State == ConnectionState::Connected,
		"wire snapshots must retain both live generations");
	return {*Sender, *Receiver};
}
inline std::uint64_t Delta(std::uint64_t Before, std::uint64_t After) {
	Require(After >= Before, "native wire counter reset during a live generation");
	return After - Before;
}
struct WireCost {
	std::uint64_t SenderPackets = 0, ReceiverPackets = 0, UdpBytes = 0;
	std::uint64_t Ipv4Bytes = 0, ConservativeIpv6Bytes = 0;
};
inline WireCost Cost(const WireSnapshot &Before, const WireSnapshot &After) {
	WireCost Result;
	Result.SenderPackets = Delta(Before.Sender.NativePacketsSent, After.Sender.NativePacketsSent);
	Result.ReceiverPackets = Delta(Before.Receiver.NativePacketsSent, After.Receiver.NativePacketsSent);
	Result.UdpBytes = Delta(Before.Sender.NativePacketBytesSent, After.Sender.NativePacketBytesSent) +
		Delta(Before.Receiver.NativePacketBytesSent, After.Receiver.NativePacketBytesSent);
	Result.Ipv4Bytes = Result.UdpBytes + 28 * (Result.SenderPackets + Result.ReceiverPackets);
	Result.ConservativeIpv6Bytes = Result.UdpBytes + 48 * (Result.SenderPackets + Result.ReceiverPackets);
	Require(After.Sender.NativeMaximumPacketBytes <= 1300 && After.Receiver.NativeMaximumPacketBytes <= 1300,
		"native packet exceeds pinned UDP maximum");
	return Result;
}
inline std::uint64_t Requests(const GargantuanAckDiagnostics &Trace, std::uint64_t Token) {
	Require(!Trace.Overflow, "ACK observer overflow cannot qualify mode isolation");
	std::uint64_t Result = 0;
	for (std::uint32_t Index = 0; Index < Trace.Count; ++Index) {
		const auto &Event = Trace.Events[Index];
		Require(Event.Type != GargantuanAckDiagnostics::PromptRequestFailed,
			"healthy compatibility case had a failed native prompt packet");
		if (Event.Type == GargantuanAckDiagnostics::PromptRequestSent) {
			Require(Token != 0 && Event.Identity == static_cast<std::int64_t>(Token),
				"prompt belongs to an ordinary message or stale grant");
			++Result;
		}
	}
	return Result;
}
inline NetworkMessageIntent Intent(PairFixture &Pair, TrafficClass Traffic, std::size_t CompleteBytes,
	std::byte Marker, std::uint64_t Sequence = 0) {
	Require(CompleteBytes > ReliableServiceEnvelopeBytes, "fixture message must have a real payload");
	MessageOrder Order = std::monostate{};
	if (Traffic == TrafficClass::StructuralReplication)
		Order = ReliableReplicationOrder{ReliableReplicationSequence(Sequence)};
	auto Result = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
		Traffic, Order, std::vector<std::byte>(CompleteBytes - ReliableServiceEnvelopeBytes, Marker), Pair.Limits);
	Require(Result.has_value(), "legal compatibility message rejected before native submission");
	return std::move(*Result);
}
inline void Send(PairFixture &Pair, const NetworkMessageIntent &Value) {
	Require(Pair.Server->Send(Value).Succeeded(), "compatibility message native submission failed");
}
inline void StartObserver(PairFixture &Pair) {
	Require(Pair.ServerConnection.IsValid() && Pair.ClientConnection.IsValid(), "real GNS pair must connect");
	Require(AckAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, true, 1348),
		"funded ACK prototype configuration failed");
	GargantuanAckDiagnostics Sender, Receiver;
	Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender, true) &&
		AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver, true), "both ACK observers must start");
	Pair.ServerEvents.clear(); Pair.ClientEvents.clear();
}
struct Completion {
	WireSnapshot AtAck, Settled;
	GargantuanAckDiagnostics Sender, Receiver;
	WireCost Whole, Tail;
};
inline Completion Complete(PairFixture &Pair, const WireSnapshot &Before,
	std::uint64_t AllAcceptedBytes, std::uint64_t StructuralBytes, std::uint64_t Token) {
	// Observe after the native finite envelope; do not repeatedly take the GNS
	// lock while the independent service thread is draining the grant.
	std::this_thread::sleep_for(50ms);
	Completion Result;
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 2s, [&] {
		Result.AtAck = ReadWire(Pair);
		return Result.AtAck.Sender.ReliablePayloadBytesAcked == AllAcceptedBytes &&
			Result.AtAck.Sender.PendingReliableStreamBytes == 0 &&
			Result.AtAck.Sender.SentUnackedReliableStreamBytes == 0;
	});
	Result.AtAck = ReadWire(Pair);
	const auto &Final = Result.AtAck.Sender;
	Require(Final.ReliablePayloadBytesAcked == AllAcceptedBytes &&
		Final.StructuralPayloadBytesFirstSent == StructuralBytes && Final.StructuralPayloadBytesAcked == StructuralBytes &&
		Final.PendingReliableStreamBytes == 0 && Final.SentUnackedReliableStreamBytes == 0 &&
		Final.UniqueReliableStreamBytesFirstSent == Final.UniqueReliableStreamBytesAcked &&
		Final.ReliableStreamBytesRetransmitted == 0,
		"ordinary and structural payload, ACK, or queue conservation failed");
	Require(Final.ActiveAttributedRetirementToken == 0 && Final.LastAttributedRetirementToken == Token &&
		Final.LastAttributedRetiredPayloadBytes == StructuralBytes,
		"retirement must consume exactly the attributed grant, never ordinary payload");
	if (Token) {
		Require(Final.StructuralLastCompletedGrantToken == Token &&
			Final.StructuralLastCompletedGrantBytes == StructuralBytes &&
			!Final.StructuralLastCompletedGrantFailed && !Final.StructuralServiceFailed &&
			Final.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds <=
				FiniteGrantServiceCurve::RunningBoundByteMicroseconds &&
			Final.StructuralMaximumFiniteShortfallByteMicroseconds == 0,
			"mixed native grant must retain the exact F1 finite and running gates");
	}
	// This is a stated observation horizon, not a claim that ACK convergence
	// prevents all later periodic control. Include both endpoints after retirement.
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 100ms, [] { return false; });
	Result.Settled = ReadWire(Pair);
	Result.Whole = Cost(Before, Result.Settled);
	Result.Tail = Cost(Result.AtAck, Result.Settled);
	Require(Result.Whole.SenderPackets > 0 && Result.Whole.ReceiverPackets > 0,
		"whole-wire qualification must include data and reverse ACK packets");
	Require(Result.Settled.Sender.StructuralPayloadBytesFirstSent == StructuralBytes &&
		Result.Settled.Sender.StructuralPayloadBytesAcked == StructuralBytes &&
		Result.Settled.Sender.ReliablePayloadBytesAcked == AllAcceptedBytes &&
		Result.Settled.Sender.PendingReliableStreamBytes == 0 && Result.Settled.Sender.SentUnackedReliableStreamBytes == 0,
		"post-retirement control must not add service or resurrect debt");
	Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Result.Sender) &&
		AckAccess::Read(*Pair.Client, Pair.ClientConnection, Result.Receiver), "terminal ACK observations missing");
	Require(Requests(Result.Receiver, 0) == 0, "receiving an immediate ACK request must not produce another grant prompt");
	return Result;
}
inline void ExactPayloads(const PairFixture &Pair, const std::vector<std::vector<std::byte>> &Expected) {
	Require(Payloads(Pair.ClientEvents) == Expected, "reliable FIFO, message boundaries, or exact bytes changed");
	Require(!HasDisconnect(Pair.ServerEvents, DisconnectReason::ProtocolViolation) &&
		!HasDisconnect(Pair.ClientEvents, DisconnectReason::ProtocolViolation), "mixed traffic caused protocol rejection");
}
inline std::vector<std::byte> Payload(std::size_t CompleteBytes, std::byte Marker) {
	return std::vector<std::byte>(CompleteBytes - ReliableServiceEnvelopeBytes, Marker);
}
inline void Print(const char *Case, const Completion &Result, std::uint64_t Token) {
	std::cout << "[Network:FundedAckCompatibility] case=" << Case << " requests=" << Requests(Result.Sender, Token)
		<< " sender_packets=" << Result.Whole.SenderPackets << " receiver_packets=" << Result.Whole.ReceiverPackets
		<< " whole_wire_ipv4=" << Result.Whole.Ipv4Bytes << " whole_wire_ipv6_bound=" << Result.Whole.ConservativeIpv6Bytes
		<< " post_retirement_us=100000 tail_wire_ipv6_bound=" << Result.Tail.ConservativeIpv6Bytes
		<< " segments=" << Result.AtAck.Sender.LastCompletedStructuralSegmentEventCount
		<< " maximum_running_deficit=" << Result.AtAck.Sender.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
		<< " result=PASS\n";
}

inline void OrdinaryAndFullReservationIsolation() {
	OwnedPair Owner; auto &Pair = Owner.Pair; StartObserver(Pair);
	const auto Before = ReadWire(Pair);
	const std::array<std::size_t, 3> Sizes{96, 128, 8192};
	Send(Pair, Intent(Pair, TrafficClass::Control, Sizes[0], std::byte{0x11}));
	Send(Pair, Intent(Pair, TrafficClass::ReliableApplication, Sizes[1], std::byte{0x22}));
	// FULL_RESERVATION's actual transport projection has no pooled retirement token.
	Send(Pair, Intent(Pair, TrafficClass::StructuralReplication, Sizes[2], std::byte{0x33}, 1));
	const auto Result = Complete(Pair, Before, Sizes[0] + Sizes[1] + Sizes[2], 0, 0);
	Require(Requests(Result.Sender, 0) == 0 && !Result.Sender.PromptFinalWireAllowed &&
		Result.Sender.GrantWholeWireBytes == 0 && Result.AtAck.Sender.StructuralCompletedGrantSequence == 0,
		"ordinary/FULL_RESERVATION traffic must not activate a pooled prompt or F1 clock");
	ExactPayloads(Pair, {Payload(Sizes[0], std::byte{0x11}), Payload(Sizes[1], std::byte{0x22}),
		Payload(Sizes[2], std::byte{0x33})});
	Print("ordinary-and-full-reservation", Result, 0);
}

inline void FundedMixedGrant() {
	OwnedPair Owner; auto &Pair = Owner.Pair; StartObserver(Pair);
	constexpr std::uint64_t Token = 41, Bytes = 393652;
	auto Control = Intent(Pair, TrafficClass::Control, 96, std::byte{0x11});
	auto Structural = Intent(Pair, TrafficClass::StructuralReplication, Bytes, std::byte{0x37}, Token);
	auto Gameplay = Intent(Pair, TrafficClass::ReliableApplication, 128, std::byte{0x55});
	Require(Access::Attribute(Structural, Token), "mixed grant token attribution failed");
	const auto Before = ReadWire(Pair);
	const auto SubmittedAt = Now();
	Send(Pair, Control);
	Require(Access::Activate(Structural, Token, Now()), "mixed grant activation failed");
	Send(Pair, Structural); Send(Pair, Gameplay);
	const auto Result = Complete(Pair, Before, Bytes + 96 + 128, Bytes, Token);
	Require(Requests(Result.Sender, Token) == 1 && Result.Sender.PromptFinalWireAllowed &&
		!Result.Sender.GrantWireInvalid, "funded multi-packet grant must request exactly one immediate ACK");
	// Independent endpoint counters include startup packets, reverse ACK, ordinary
	// traffic and the post-retirement tail, not merely the predicate's own ledger.
	Require(Result.Sender.GrantWholeWireCeiling > Bytes &&
		Result.Whole.ConservativeIpv6Bytes <= Result.Sender.GrantWholeWireCeiling,
		"complete mixed bidirectional wire cost exceeds the funded grant ceiling");
	ExactPayloads(Pair, {Payload(96, std::byte{0x11}), Payload(Bytes, std::byte{0x37}),
		Payload(128, std::byte{0x55})});
	std::size_t OrdinaryReceived = 0;
	for (std::uint32_t Index = 0; Index < Result.Receiver.Count; ++Index) {
		const auto &Event = Result.Receiver.Events[Index];
		if (Event.Type == GargantuanAckDiagnostics::MessageReceived && (Event.Value == 96 || Event.Value == 128)) {
			Require(Event.AtMicroseconds >= SubmittedAt && Event.AtMicroseconds - SubmittedAt <= 150000,
				"mixed reliable ordinary delivery exceeded the canonical 150 ms latency target");
			++OrdinaryReceived;
		}
	}
	Require(OrdinaryReceived == 2, "ordinary latency must be checked against actual native delivery");
	Print("funded-mixed-grant", Result, Token);
}

inline void TinySplitByOrdinaryPacketOccupancy() {
	std::size_t SplitCases = 0;
	// Bounded packet-boundary sweep, not a synthetic segment counter. The preceding
	// ordinary Reliable message is followed immediately by production NoNagle.
	// Different native framing/header states may place one boundary differently.
	for (const std::size_t PrefixBytes : {std::size_t{1072}, std::size_t{1088}, std::size_t{1104}, std::size_t{1120}}) {
		OwnedPair Owner; auto &Pair = Owner.Pair; StartObserver(Pair);
		constexpr std::uint64_t Token = 9, Bytes = 77;
		auto Ordinary = Intent(Pair, TrafficClass::ReliableApplication, PrefixBytes, std::byte{0x61});
		auto Tiny = Intent(Pair, TrafficClass::StructuralReplication, Bytes, std::byte{0x62}, Token);
		Require(Access::Attribute(Tiny, Token), "tiny grant attribution failed");
		const auto Before = ReadWire(Pair);
		Require(Access::Activate(Tiny, Token, Now()), "tiny grant activation failed");
		Send(Pair, Ordinary); Send(Pair, Tiny);
		const auto Result = Complete(Pair, Before, PrefixBytes + Bytes, Bytes, Token);
		Require(Requests(Result.Sender, Token) == 0 && !Result.Sender.PromptFinalWireAllowed,
			"multi-packet tiny grant must not bypass the funded-wire predicate");
		ExactPayloads(Pair, {Payload(PrefixBytes, std::byte{0x61}), Payload(Bytes, std::byte{0x62})});
		const auto &Final = Result.AtAck.Sender;
		std::uint64_t NativeBytes = 0;
		for (std::uint32_t Index = 0; Index < Final.LastCompletedStructuralSegmentEventCount; ++Index)
			NativeBytes += Final.LastCompletedStructuralSegmentEvents[Index].PayloadBytes;
		Require(NativeBytes == Bytes, "tiny native segments must conserve exact attributed bytes");
		if (Final.LastCompletedStructuralSegmentEventCount > 1) {
			Require(Final.LastCompletedStructuralSegmentEvents[0].PayloadBytes > 0 &&
				Final.LastCompletedStructuralSegmentEvents[0].PayloadBytes < Bytes,
				"tiny split regression must observe a real partial unique first send");
			++SplitCases;
		}
		Print("tiny-mixed-boundary", Result, Token);
	}
	Require(SplitCases > 0, "native boundary sweep did not exercise a tiny grant split by ordinary traffic");
	std::cout << "[Network:FundedAckCompatibility] tiny_split_cases=" << SplitCases << " result=PASS\n";
}

inline bool Run() {
	try {
		OrdinaryAndFullReservationIsolation();
		FundedMixedGrant();
		TinySplitByOrdinaryPacketOccupancy();
		std::cout << "[Network:FundedAckCompatibility] 3 cases PASS\n";
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:FundedAckCompatibility] FAIL " << Error.what() << '\n';
		return false;
	}
}
}

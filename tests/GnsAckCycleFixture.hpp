#pragma once
#include "../src/network/GnsAckDiagnosticsAccess.hpp"

namespace GnsAckCycleFixture {
using namespace gargantuan::network;
using namespace std::chrono_literals;

inline std::uint64_t Now() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void Dump(const char *Side, std::uint64_t Token, const GargantuanAckDiagnostics &Trace) {
	std::cout << "[Network:AckCycle] side=" << Side << " token=" << Token
		<< " first_reliable_us=" << Trace.FirstReliablePacketAt << " last_reliable_us="
		<< Trace.LastReliablePacketAt << " reliable_packets=" << Trace.ReliablePackets
		<< " last_packet=" << Trace.LastReliablePacketNumber << " overflow=" << Trace.Overflow
		<< " repeated_ack_serializations=" << Trace.RepeatedAckSerializations
		<< " ack_packets=" << Trace.AckPacketsSent << " ack_packet_bytes=" << Trace.AckPacketBytes
		<< " latest_ack_packet_us=" << Trace.LastAckPacketSentAt
		<< " maximum_prompt_reserve_bytes=" << Trace.MaximumPromptReserveBytes << '\n';
	for (std::uint32_t Index = 0; Index < Trace.Count; ++Index) {
		const auto &Event = Trace.Events[Index];
		std::cout << "[Network:AckCycle:Native] side=" << Side << " token=" << Token
			<< " kind=" << Event.Type << " steady_us=" << Event.AtMicroseconds
			<< " native_us=" << Event.NativeAtMicroseconds << " identity=" << Event.Identity
			<< " value=" << Event.Value << '\n';
	}
}

// Production adapter + real pinned GNS, two successive obligations on the same
// connection. Submission is explicitly gated by actual native ACK retirement.
// This isolates ACK/polling; it does not claim to run GameSession credit/fairness.
inline void Observe(std::size_t Bytes, std::chrono::microseconds PollPeriod, bool Prompt = false) {
	PairFixture Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
	struct Cleanup { PairFixture &Value; ~Cleanup() { StopPair(Value); } } Guard{Pair};
	if (!Pair.ServerConnection.IsValid()) throw std::runtime_error("ACK cycle connection failed");
	if (Prompt && !detail::GnsAckDiagnosticsAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, true))
		throw std::runtime_error("ACK prototype enable failed");
	std::uint64_t Accepted = 0;
	for (std::uint64_t Token = 1; Token <= 2; ++Token) {
		const auto Before = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
		const auto ReceiverBefore = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Client, Pair.ClientConnection);
		if (!Before || !ReceiverBefore) throw std::runtime_error("ACK cycle initial counters missing");
		GargantuanAckDiagnostics Sender, Receiver;
		if (!detail::GnsAckDiagnosticsAccess::Read(*Pair.Server, Pair.ServerConnection, Sender, true) ||
			!detail::GnsAckDiagnosticsAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver, true))
			throw std::runtime_error("ACK trace activation failed");
		auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence(Token)},
			std::vector<std::byte>(Bytes - ReliableServiceEnvelopeBytes, std::byte{0x37}), Pair.Limits);
		const auto Activated = Now();
		if (!Intent || !detail::ReliableServiceFeedbackAccess::Attribute(*Intent, Token) ||
			!detail::ReliableServiceFeedbackAccess::Activate(*Intent, Token, Activated) ||
			!Pair.Server->Send(*Intent).Succeeded()) throw std::runtime_error("ACK cycle submission failed");
		Accepted += Bytes;
		if (detail::GnsAckDiagnosticsAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, !Prompt))
			throw std::runtime_error("ACK policy changed while attributed obligation is owned");
		std::optional<detail::ReliableServiceFeedback> Final;
		std::uint64_t ObservedRetirement = 0, ReceivedBytes = 0;
		const auto Deadline = std::chrono::steady_clock::now() + 2s;
		auto NextPoll = std::chrono::steady_clock::now() + PollPeriod;
		while (std::chrono::steady_clock::now() < Deadline) {
			std::this_thread::sleep_until(NextPoll);
			NextPoll += PollPeriod;
			(void)Drain(*Pair.Server);
			for (const auto &Event : Drain(*Pair.Client))
				if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event)) ReceivedBytes += Message->Payload.size();
			Final = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
			if (!Final || !Final->CountersValid) throw std::runtime_error("ACK cycle invalid native feedback");
			if (Final->LastAttributedRetirementToken == Token) { ObservedRetirement = Now(); break; }
		}
		if (!ObservedRetirement || !Final || Final->StructuralPayloadBytesFirstSent != Accepted ||
			Final->StructuralPayloadBytesAcked != Accepted || Final->LastAttributedRetiredPayloadBytes != Bytes ||
			ReceivedBytes != Bytes - ReliableServiceEnvelopeBytes || Final->PendingReliableStreamBytes != 0 ||
			Final->SentUnackedReliableStreamBytes != 0)
			throw std::runtime_error("ACK cycle conservation/convergence failed");
		if (!detail::GnsAckDiagnosticsAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) ||
			!detail::GnsAckDiagnosticsAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver))
			throw std::runtime_error("ACK cycle missing trace");
		Dump("sender", Token, Sender); Dump("receiver", Token, Receiver);
		const auto ReceiverAfter = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Client, Pair.ClientConnection);
		if (!ReceiverAfter) throw std::runtime_error("ACK cycle receiver terminal counters missing");
		std::uint64_t NativeAckedAt = 0, ReceiverCompletedAt = 0, AckSentAt = 0, Requests = 0;
		for (std::uint32_t Index = 0; Index < Sender.Count; ++Index) {
			const auto &Event = Sender.Events[Index];
			if (Event.Type == GargantuanAckDiagnostics::PromptRequestSent) {
				if (Event.Identity != static_cast<std::int64_t>(Token))
					throw std::runtime_error("ACK request has stale grant identity");
				++Requests;
			}
			if (Event.Type == GargantuanAckDiagnostics::MessageAcked &&
				Event.Identity == static_cast<std::int64_t>(Final->LastAttributedRetirementMessageNumber))
				NativeAckedAt = Event.AtMicroseconds;
		}
		for (std::uint32_t Index = 0; Index < Receiver.Count; ++Index) {
			const auto &Event = Receiver.Events[Index];
			if (Event.Type == GargantuanAckDiagnostics::MessageReceived && Event.Value == static_cast<std::int64_t>(Bytes))
				ReceiverCompletedAt = Event.AtMicroseconds;
			if (Event.Type == GargantuanAckDiagnostics::AckPacketSent && Event.Identity >= Receiver.LastReliablePacketNumber)
				AckSentAt = Event.AtMicroseconds;
		}
		if (Sender.Overflow || Receiver.Overflow || !NativeAckedAt || !ReceiverCompletedAt || !AckSentAt ||
			NativeAckedAt < Final->StructuralLastCompletedGrantCompletedAtMicroseconds ||
			ObservedRetirement < NativeAckedAt || AckSentAt < ReceiverCompletedAt)
			throw std::runtime_error("ACK cycle missing/inconsistent chronology");
		const auto ExpectedRequests = Prompt && Bytes >= FiniteGrantServiceCurve::QuantumBytes &&
			Final->LastCompletedStructuralSegmentEventCount > 1 ? 1u : 0u;
		if (Requests != ExpectedRequests) throw std::runtime_error("final grant ACK request was missing or duplicated");
		std::cout << "[Network:AckCycle:Grant] bytes=" << Bytes << " token=" << Token
			<< " prompt=" << Prompt << " requests=" << Requests
			<< " poll_us=" << PollPeriod.count() << " activated_us=" << Activated
			<< " first_us=" << Final->StructuralLastCompletedGrantFirstSendAtMicroseconds
			<< " complete_us=" << Final->StructuralLastCompletedGrantCompletedAtMicroseconds
			<< " receiver_complete_us=" << ReceiverCompletedAt << " ack_sent_us=" << AckSentAt
			<< " sender_ack_us=" << NativeAckedAt << " observed_retire_us=" << ObservedRetirement
			<< " native_ack_wait_us=" << NativeAckedAt - Final->StructuralLastCompletedGrantCompletedAtMicroseconds
			<< " observer_lag_us=" << ObservedRetirement - NativeAckedAt
			<< " retransmitted=" << Final->ReliableStreamBytesRetransmitted
			<< " sender_packets=" << Final->NativePacketsSent - Before->NativePacketsSent
			<< " sender_udp_bytes=" << Final->NativePacketBytesSent - Before->NativePacketBytesSent
			<< " receiver_ack_packets=" << Receiver.AckPacketsSent
			<< " receiver_ack_udp_bytes=" << Receiver.AckPacketBytes
			<< " receiver_packets=" << ReceiverAfter->NativePacketsSent - ReceiverBefore->NativePacketsSent
			<< " receiver_udp_bytes=" << ReceiverAfter->NativePacketBytesSent - ReceiverBefore->NativePacketBytesSent
			<< " segments=" << Final->LastCompletedStructuralSegmentEventCount
			<< " maximum_running_deficit=" << Final->StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
			<< " service_failed=" << Final->StructuralLastCompletedGrantFailed << '\n';
	}
}

inline bool Run(bool Prompt = false) {
	try {
		GargantuanAckDiagnostics Repeated;
		for (int Index = 0; Index < 1000; ++Index) {
			Repeated.Record(GargantuanAckDiagnostics::AckSerialized, Index, 1, 0);
			Repeated.Record(GargantuanAckDiagnostics::AckPacketSent, Index, 1, 100);
		}
		Repeated.Record(GargantuanAckDiagnostics::MessageAcked, 1001, 2, 393652);
		Repeated.Record(GargantuanAckDiagnostics::AckSerialized, 1002, 2, 0);
		Repeated.Record(GargantuanAckDiagnostics::AckPacketSent, 1003, 2, 120);
		if (Repeated.Overflow || Repeated.Count != 5 || Repeated.AckPacketsSent != 1001 ||
			Repeated.AckPacketBytes != 100120 || Repeated.RepeatedAckSerializations != 999 ||
			Repeated.Events[2].Type != GargantuanAckDiagnostics::MessageAcked ||
			Repeated.Events[4].Identity != 2)
			throw std::runtime_error("repeated ACKs displaced critical native events");
		for (const auto Bytes : {std::size_t{393652}, std::size_t{524288}})
			for (const auto PollPeriod : {1000us, 16667us}) Observe(Bytes, PollPeriod, Prompt);
		if (Prompt) for (const auto Bytes : {std::size_t{77}, std::size_t{1135}, std::size_t{1136}, std::size_t{1258}})
			Observe(Bytes, 1000us, Prompt);
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:AckCycle] FAIL " << Error.what() << '\n';
		return false;
	}
}
}

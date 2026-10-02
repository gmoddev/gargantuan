#pragma once
#include "../src/network/GnsAckDiagnosticsAccess.hpp"
#include "../cmake/gns/PromptAckWireBudget.hpp"

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
		<< " associated_recv_packets=" << Trace.AssociatedReceivedPackets
		<< " associated_recv_udp_bytes=" << Trace.AssociatedReceivedUdpBytes
		<< " latest_ack_packet_us=" << Trace.LastAckPacketSentAt
		<< " maximum_prompt_reserve_bytes=" << Trace.MaximumPromptReserveBytes << '\n';
	std::cout << "[Network:AckCycle:Wire] side=" << Side << " token=" << Token
		<< " bytes=" << Trace.GrantWholeWireBytes << " ceiling=" << Trace.GrantWholeWireCeiling
		<< " tail_budget=" << Trace.PromptTailBudget << " invalid=" << Trace.GrantWireInvalid
		<< " final_allowed=" << Trace.PromptFinalWireAllowed << " prior_wire=" << Trace.PromptFinalPriorWireBytes
		<< " background_rate=" << Trace.BackgroundRate << " background_burst=" << Trace.BackgroundBurst << '\n';
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
enum class Fault { None, NativeSendFailure, ReceiveLoss, SocketSendFailure, ReceiveDuplicate };
inline void Observe(std::size_t Bytes, std::chrono::microseconds PollPeriod, bool Prompt = false,
	std::uint64_t TailBudget = 0, Fault Failure = Fault::None, bool MixedAfter = false) {
	PairFixture Pair = StartPair({.MaximumConnections = 1, .SendRate = 18 * 1024 * 1024}, TestLimits(), true);
	struct Cleanup { PairFixture &Value; ~Cleanup() { StopPair(Value); } } Guard{Pair};
	struct LossCleanup {
		bool Enabled = false;
		bool Duplicate = false;
		~LossCleanup() {
			if (Enabled) SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0);
			if (Duplicate) SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Recv, 0);
		}
	} Loss;
	if (!Pair.ServerConnection.IsValid()) throw std::runtime_error("ACK cycle connection failed");
	if (Prompt && !detail::GnsAckDiagnosticsAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, true, TailBudget))
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
		if ((Failure == Fault::NativeSendFailure || Failure == Fault::SocketSendFailure) &&
			!detail::GnsAckDiagnosticsAccess::FailNextFinalPacket(*Pair.Server, Pair.ServerConnection, Failure == Fault::SocketSendFailure))
			throw std::runtime_error("native final-packet failure could not be armed");
		auto Intent = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication, ReliableReplicationOrder{ReliableReplicationSequence(Token)},
			std::vector<std::byte>(Bytes - ReliableServiceEnvelopeBytes, std::byte{0x37}), Pair.Limits);
		const auto Activated = Now();
		if (Failure == Fault::ReceiveDuplicate) {
			if (!SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Recv, 100))
				throw std::runtime_error("native receive-duplicate injection failed");
			Loss.Duplicate = true;
		}
		if (Failure == Fault::ReceiveLoss) {
			if (!SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 100))
				throw std::runtime_error("native receive-loss injection failed");
			Loss.Enabled = true;
		}
		if (!Intent || !detail::ReliableServiceFeedbackAccess::Attribute(*Intent, Token) ||
			!detail::ReliableServiceFeedbackAccess::Activate(*Intent, Token, Activated) ||
			!Pair.Server->Send(*Intent).Succeeded()) throw std::runtime_error("ACK cycle submission failed");
		Accepted += Bytes;
		constexpr std::size_t OrdinaryBytes = 128;
		if (MixedAfter) {
			auto Ordinary = MakeNetworkMessageIntent(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
				TrafficClass::ReliableApplication, std::monostate{},
				std::vector<std::byte>(OrdinaryBytes - ReliableServiceEnvelopeBytes, std::byte{0x52}), Pair.Limits);
			if (!Ordinary || !Pair.Server->Send(*Ordinary).Succeeded())
				throw std::runtime_error("mixed failed-packet ordinary submission failed");
		}
		if (detail::GnsAckDiagnosticsAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, !Prompt))
			throw std::runtime_error("ACK policy changed while attributed obligation is owned");
		std::optional<detail::ReliableServiceFeedback> Final;
		std::uint64_t ObservedRetirement = 0, ReceivedBytes = 0;
		std::vector<std::vector<std::byte>> ReceivedPayloads;
		auto ReceiveClient = [&] {
			for (const auto &Event : Drain(*Pair.Client))
				if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event)) {
					ReceivedBytes += Message->Payload.size();
					ReceivedPayloads.push_back(Message->Payload);
				}
		};
		const auto Deadline = std::chrono::steady_clock::now() + 2s;
		auto NextPoll = std::chrono::steady_clock::now() + PollPeriod;
		while (std::chrono::steady_clock::now() < Deadline) {
			std::this_thread::sleep_until(NextPoll);
			NextPoll += PollPeriod;
			if (Loss.Enabled && Now() - Activated >= 150000) {
				if (!SteamNetworkingUtils()->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, 0))
					throw std::runtime_error("native receive-loss removal failed");
				Loss.Enabled = false;
			}
			(void)Drain(*Pair.Server);
			ReceiveClient();
			Final = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
			if (!Final || !Final->CountersValid) throw std::runtime_error("ACK cycle invalid native feedback");
			if (Final->LastAttributedRetirementToken == Token) {
				ObservedRetirement = Now();
				// Native receipt/ACK can happen after the earlier client drain and
				// before this sender sample. Consume the already received message;
				// do not add a latency grace period or infer delivery from ACK.
				ReceiveClient();
				break;
			}
		}
		if (Failure == Fault::ReceiveDuplicate && ObservedRetirement) {
			// Observe delayed duplicate delivery within the original two-second
			// bound; the native duplicate filter must not create new service.
			const auto DuplicateDeadline = std::min(Deadline, std::chrono::steady_clock::now() + 250ms);
			while (std::chrono::steady_clock::now() < DuplicateDeadline) {
				std::this_thread::sleep_for(1ms);
				(void)Drain(*Pair.Server);
				ReceiveClient();
			}
			Final = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
		}
		if (!ObservedRetirement || !Final || Final->StructuralPayloadBytesFirstSent != Accepted ||
			Final->StructuralPayloadBytesAcked != Accepted || Final->LastAttributedRetiredPayloadBytes != Bytes ||
			ReceivedBytes != Bytes - ReliableServiceEnvelopeBytes + (MixedAfter ? OrdinaryBytes - ReliableServiceEnvelopeBytes : 0) ||
			Final->ReliablePayloadBytesAcked != Accepted + (MixedAfter ? Token * OrdinaryBytes : 0) || Final->PendingReliableStreamBytes != 0 ||
			Final->SentUnackedReliableStreamBytes != 0) {
			std::cerr << "[Network:AckCycle:ConvergenceFailure] bytes=" << Bytes << " token=" << Token
				<< " fault=" << static_cast<int>(Failure) << " observed_retire=" << ObservedRetirement
				<< " accepted=" << Accepted << " received_payload=" << ReceivedBytes;
			if (Final) std::cerr << " first=" << Final->StructuralPayloadBytesFirstSent
				<< " ack=" << Final->StructuralPayloadBytesAcked << " retired=" << Final->LastAttributedRetiredPayloadBytes
				<< " retired_token=" << Final->LastAttributedRetirementToken << " pending=" << Final->PendingReliableStreamBytes
				<< " unacked=" << Final->SentUnackedReliableStreamBytes << " retransmitted=" << Final->ReliableStreamBytesRetransmitted;
			std::cerr << '\n';
			if (detail::GnsAckDiagnosticsAccess::Read(*Pair.Server, Pair.ServerConnection, Sender)) Dump("sender", Token, Sender);
			if (detail::GnsAckDiagnosticsAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver)) Dump("receiver", Token, Receiver);
			throw std::runtime_error("ACK cycle conservation/convergence failed");
		}
		std::vector<std::vector<std::byte>> ExpectedPayloads{
			std::vector<std::byte>(Bytes - ReliableServiceEnvelopeBytes, std::byte{0x37})};
		if (MixedAfter) ExpectedPayloads.emplace_back(OrdinaryBytes - ReliableServiceEnvelopeBytes, std::byte{0x52});
		if (ReceivedPayloads != ExpectedPayloads)
			throw std::runtime_error("failed packet changed reliable FIFO/payload identity");
		if (!detail::GnsAckDiagnosticsAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) ||
			!detail::GnsAckDiagnosticsAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver))
			throw std::runtime_error("ACK cycle missing trace");
		Dump("sender", Token, Sender); Dump("receiver", Token, Receiver);
		const auto ReceiverAfter = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Client, Pair.ClientConnection);
		if (!ReceiverAfter) throw std::runtime_error("ACK cycle receiver terminal counters missing");
		std::uint64_t NativeAckedAt = 0, ReceiverCompletedAt = 0, AckSentAt = 0, Requests = 0, FailedRequests = 0;
		for (std::uint32_t Index = 0; Index < Sender.Count; ++Index) {
			const auto &Event = Sender.Events[Index];
			if (Event.Type == GargantuanAckDiagnostics::PromptRequestFailed) ++FailedRequests;
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
		const auto ExpectedRequests = (TailBudget ? Sender.PromptFinalWireAllowed : Prompt &&
			Bytes >= FiniteGrantServiceCurve::QuantumBytes && Final->LastCompletedStructuralSegmentEventCount > 1) ? 1u : 0u;
		if (Requests != ExpectedRequests) throw std::runtime_error("final grant ACK request was missing or duplicated");
		if (Failure == Fault::None && Final->StructuralLastCompletedGrantFailed)
			throw std::runtime_error("healthy ACK cycle failed canonical finite-grant service");
		if (Failure == Fault::NativeSendFailure && (Sender.InjectedNativeSendFailures != 1 || FailedRequests != 1 ||
			!Sender.FirstSentBytesAtInjectedFailure || Sender.FirstSentBytesAtInjectedFailure >= Bytes || Requests != 1))
			throw std::runtime_error("native failed final-send retry lost or duplicated its finite obligation");
		if (Failure == Fault::SocketSendFailure && (Sender.InjectedSocketSendFailures != 1 || FailedRequests != 1 ||
			Sender.InjectedNativeSendFailures != 0 || !Sender.FirstSentBytesAtInjectedFailure ||
			Sender.FirstSentBytesAtInjectedFailure >= Bytes || Requests != 1))
			throw std::runtime_error("post-bookkeeping socket failure lost or duplicated its finite obligation");
		if ((Failure == Fault::NativeSendFailure || Failure == Fault::SocketSendFailure) &&
			Sender.FailedPacketReferencesReleased == 0)
			throw std::runtime_error("failed local packet retained an unreachable segment reference");
		if (MixedAfter && Sender.FailedPacketReferencesReleased < 2)
			throw std::runtime_error("mixed failure did not exercise both ordinary and structural packet ownership");
		if (Failure == Fault::ReceiveDuplicate &&
			(Receiver.AssociatedReceivedPackets <= Final->NativePacketsSent - Before->NativePacketsSent ||
			Receiver.AssociatedReceivedUdpBytes <= Final->NativePacketBytesSent - Before->NativePacketBytesSent))
			throw std::runtime_error("duplicate datagrams did not increase independently observed associated wire");
		if (Failure == Fault::ReceiveLoss && Final->ReliableStreamBytesRetransmitted <= Before->ReliableStreamBytesRetransmitted)
			throw std::runtime_error("receive-loss case did not exercise retransmission");
		if (Prompt && TailBudget == 1348 && ((Bytes >= 393652 && Requests != 1) || (Bytes <= 1258 && Requests != 0)))
			throw std::runtime_error("known funded/denied transport shapes did not match independent expectation");
		if (TailBudget && Requests && (Sender.GrantWireInvalid ||
			Sender.PromptFinalPriorWireBytes > Sender.GrantWholeWireCeiling ||
			1348 + TailBudget > Sender.GrantWholeWireCeiling - Sender.PromptFinalPriorWireBytes))
			throw std::runtime_error("emitted prompt exceeded its independently reconstructed wire ceiling");
		std::cout << "[Network:AckCycle:Grant] bytes=" << Bytes << " token=" << Token
			<< " prompt=" << Prompt << " requests=" << Requests << " tail_budget=" << TailBudget
			<< " fault=" << static_cast<int>(Failure) << " failed_requests=" << FailedRequests
			<< " mixed_ordinary=" << MixedAfter
			<< " first_bytes_at_failed_send=" << Sender.FirstSentBytesAtInjectedFailure
			<< " socket_send_failures=" << Sender.InjectedSocketSendFailures
			<< " failed_packet_refs_released=" << Sender.FailedPacketReferencesReleased
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
	// Observe a stated finite post-retirement interval, rather than declaring
	// that ACK convergence bounds every future periodic control packet.
	std::this_thread::sleep_for(100ms);
	(void)Drain(*Pair.Server); (void)Drain(*Pair.Client);
	const auto SenderSettled = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Server, Pair.ServerConnection);
	const auto ReceiverSettled = detail::ReliableServiceFeedbackAccess::Observe(*Pair.Client, Pair.ClientConnection);
	if (!SenderSettled || !ReceiverSettled) throw std::runtime_error("ACK cycle post-retirement counters missing");
	std::cout << "[Network:AckCycle:PostRetirement] bytes=" << Bytes << " prompt=" << Prompt
		<< " tail_budget=" << TailBudget << " observation_us=100000"
		<< " sender_packets=" << SenderSettled->NativePacketsSent << " sender_udp_bytes=" << SenderSettled->NativePacketBytesSent
		<< " receiver_packets=" << ReceiverSettled->NativePacketsSent << " receiver_udp_bytes=" << ReceiverSettled->NativePacketBytesSent << '\n';
}

inline bool RunRetrySafety() {
	try {
		// Scheduling delay is injected deliberately. These cases assert native
		// lifetime/accounting/FIFO only, never a physical or wall-time F1 PASS.
		Observe(393652, 1000us, true, 1348, Fault::NativeSendFailure);
		Observe(393652, 1000us, true, 1348, Fault::SocketSendFailure);
		Observe(393652, 1000us, true, 1348, Fault::NativeSendFailure, true);
		Observe(393652, 1000us, true, 1348, Fault::SocketSendFailure, true);
		Observe(393652, 1000us, true, 1348, Fault::ReceiveLoss);
		Observe(393652, 1000us, true, 1348, Fault::ReceiveDuplicate);
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:AckRetrySafety] FAIL " << Error.what() << '\n'; return false;
	}
}

inline bool Run(bool Prompt = false, std::uint64_t TailBudget = 0) {
	try {
		GargantuanPromptAckWireBudget Budget;
		if (!Budget.ConfigureBackground(8 * 1024 * 1024, 32, 1348, 5, 20, 120) ||
			Budget.BackgroundRate != 44574 || Budget.BackgroundBurst != 517632 || Budget.Reserve != 8344034)
			throw std::runtime_error("ACK background reserve derivation mismatch");
		if (Budget.ConfigureBackground(1, 32, 1348, 5, 20, 120) ||
			Budget.ConfigureBackground(8 * 1024 * 1024, std::numeric_limits<std::uint64_t>::max(), 1348, 5, 20, 120))
			throw std::runtime_error("ACK unfunded/overflowing background budget accepted");
		Budget.Reserve = 8; Budget.StructuralPool = 64; Budget.TailBudget = 225;
		Budget.Begin(1, 1000); Budget.Charge(452, 48);
		if (!Budget.CanRequest(400) || Budget.CanRequest(401))
			throw std::runtime_error("ACK wire exact boundary is not integer safe");
		Budget.Charge(1, 0);
		if (Budget.CanRequest(400)) throw std::runtime_error("ACK wire unfunded packet accepted");
		Budget.Begin(2, 1000);
		Budget.Charge(std::numeric_limits<std::uint64_t>::max(), 48);
		if (!Budget.Invalid || Budget.CanRequest(1)) throw std::runtime_error("ACK wire charge overflow accepted");
		Budget.Begin(3, 1000); Budget.Reserve = std::numeric_limits<std::uint64_t>::max();
		if (Budget.CanRequest(1)) throw std::runtime_error("ACK wire ratio overflow accepted");
		Budget.Reserve = 8; Budget.StructuralPool = 0;
		if (Budget.CanRequest(1)) throw std::runtime_error("ACK wire zero divisor accepted");
		Budget.StructuralPool = 64; Budget.TailBudget = 1348; Budget.Begin(4, 77);
		if (Budget.CanRequest(1348)) throw std::runtime_error("ACK wire unfunded tiny grant accepted");
		Budget.Begin(5, 524288); Budget.Charge(500000, 48);
		if (!Budget.CanRequest(1348)) throw std::runtime_error("ACK wire funded finite grant rejected");
		Budget.Charge(100000, 48); // Whole shared/retransmitted packets remain chargeable.
		if (Budget.CanRequest(1348)) throw std::runtime_error("ACK wire retry/shared traffic was ignored");
		GargantuanAckDiagnostics Repeated;
		Repeated.Incoming(128);
		Repeated.Incoming(128);
		if (Repeated.AssociatedReceivedPackets != 2 || Repeated.AssociatedReceivedUdpBytes != 256)
			throw std::runtime_error("associated duplicate datagram accounting was collapsed");
		GargantuanAckDiagnostics Overflowing;
		Overflowing.AssociatedReceivedUdpBytes = UINT64_MAX;
		Overflowing.Incoming(1);
		if (!Overflowing.Overflow || Overflowing.AssociatedReceivedPackets != 0)
			throw std::runtime_error("associated datagram byte overflow accepted");
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
			for (const auto PollPeriod : {1000us, 16667us}) Observe(Bytes, PollPeriod, Prompt, TailBudget);
		if (Prompt) for (const auto Bytes : {std::size_t{77}, std::size_t{1135}, std::size_t{1136}, std::size_t{1258}})
			Observe(Bytes, 1000us, Prompt, TailBudget);
		if (Prompt && TailBudget == 1348) {
			Observe(393652, 1000us, true, TailBudget, Fault::NativeSendFailure);
			Observe(393652, 1000us, true, TailBudget, Fault::SocketSendFailure);
			Observe(393652, 1000us, true, TailBudget, Fault::ReceiveLoss);
			Observe(393652, 1000us, true, TailBudget, Fault::ReceiveDuplicate);
		}
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:AckCycle] FAIL " << Error.what() << '\n';
		return false;
	}
}
}

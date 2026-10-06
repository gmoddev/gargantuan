#pragma once
#include <iterator>
#include "AckStatsTimingEvidence.hpp"

// Real native timers, no clock override. Included after the shared production
// adapter fixture and compatibility helpers.
namespace GnsAckStatsBoundaryFixture {
using namespace GnsFundedAckCompatibilityFixture;

struct Totals {
	std::uint64_t Tracers = 0, Instantaneous = 0, InstantaneousReceived = 0;
	std::uint64_t ImmediateSent = 0, ImmediateReceived = 0, Requests = 0;
	std::uint64_t FirstTracer = 0, FirstInstantaneous = 0;
	std::uint32_t NeedMask = 0;
	void Add(const GargantuanAckDiagnostics &Value) {
		Require(!Value.Overflow, "stats-boundary observer overflow");
		Tracers += Value.TracerRequestsSent;
		Instantaneous += Value.StatsInstantaneousSent;
		InstantaneousReceived += Value.StatsInstantaneousReceived;
		ImmediateSent += Value.StatsImmediateSent;
		ImmediateReceived += Value.StatsImmediateReceived;
		Requests += Value.StatsRequestsSent;
		NeedMask |= Value.ObservedStatsNeedMask;
		if (Value.FirstTracerAt && (!FirstTracer || Value.FirstTracerAt < FirstTracer)) FirstTracer = Value.FirstTracerAt;
		if (Value.FirstInstantaneousAt && (!FirstInstantaneous || Value.FirstInstantaneousAt < FirstInstantaneous))
			FirstInstantaneous = Value.FirstInstantaneousAt;
	}
};

inline void PrintState(const char *Stage, const char *Side, const GargantuanAckDiagnostics &Value) {
	std::cout << "[Network:AckStatsState] stage=" << Stage << " side=" << Side
		<< " native_now=" << Value.NativeSnapshotNow << " last_ping_sent=" << Value.NativeLastPingSent
		<< " last_ping_received=" << Value.NativeLastPingReceived << " tracer_ready=" << Value.NativeTracerReady
		<< " stats_in_flight=" << Value.NativeStatsInFlight << " activity=" << Value.NativeActivity
		<< " need_mask=" << Value.ObservedStatsNeedMask << " tracer_requests=" << Value.TracerRequestsSent
		<< " instantaneous_sent=" << Value.StatsInstantaneousSent << " instantaneous_received=" << Value.StatsInstantaneousReceived << '\n';
}

inline void Observe(bool Prompt) {
    AckStatsTimingEvidence::Arm DiagnosticArm(Prompt);
    OwnedPair Owner; auto &Pair = Owner.Pair;
	Require(Pair.ServerConnection.IsValid(), "stats-boundary real connection missing");
	// The prompt arm exercises production activation; the control arm must
	// explicitly disable it before its first attributed grant.
	if (!Prompt) Require(AckAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, false),
		"stats-boundary control override rejected");
	GargantuanAckDiagnostics Sender, Receiver;
	Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender, true) &&
		AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver, true), "stats observers missing");
	const auto WireBefore = ReadWire(Pair);
	const auto Start = std::chrono::steady_clock::now();
	const auto StartUs = Now();
	const auto CycleStart = Start + 8s;
	const auto End = CycleStart + 24s;
	// Ordinary reliable ACKs themselves supply RTT samples. Continuous tiny
	// grants every500ms therefore suppress the native5/7s tracer eligibility.
	// Let the real idle interval elapse before resuming; never alter a timer.
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 8s, [] { return false; });
    Require(AckStatsTimingEvidence::ReadNativeSnapshot("after-quiet", "sender", Prompt, 0, Sender,
            [&] { return AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender); }) &&
        AckStatsTimingEvidence::ReadNativeSnapshot("after-quiet", "receiver", Prompt, 0, Receiver,
            [&] { return AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver); }), "quiet interval stats missing");
	PrintState("after-quiet", "sender", Sender); PrintState("after-quiet", "receiver", Receiver);
	Totals SenderTotals, ReceiverTotals;
	std::uint64_t Accepted = 0, Grants = 0, Prompts = 0;
	while (std::chrono::steady_clock::now() < End && Grants < 48) {
		// Collect the entire interval before resetting the bounded event log.
		Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) &&
			AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver), "interval stats missing");
		SenderTotals.Add(Sender); ReceiverTotals.Add(Receiver);
		Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender, true) &&
			AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver, true), "interval reset failed");
		Pair.ServerEvents.clear(); Pair.ClientEvents.clear();
		const std::uint64_t Token = ++Grants;
		// Ordinary tiny-grant ACK behavior crosses the native 5s tracer period.
		// Later alternate funded/tiny grants through the native 20s stats period.
		const std::size_t Bytes = Token > 14 && Token % 2 == 0 ? 393652 : 77;
		auto Message = Intent(Pair, TrafficClass::StructuralReplication, Bytes, std::byte{0x49}, Token);
		Require(Access::Attribute(Message, Token), "stats-boundary finite grant activation failed");
		const auto ActivatedAt = Now();
		Require(Access::Activate(Message, Token, ActivatedAt),
			"stats-boundary finite grant activation failed");
		Send(Pair, Message); Accepted += Bytes;
		std::this_thread::sleep_for(50ms);
		const auto Deadline = std::min(End + 2s, std::chrono::steady_clock::now() + 2s);
		WireSnapshot Final;
		while (std::chrono::steady_clock::now() < Deadline) {
			Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 1ms, [] { return false; });
			Final = ReadWire(Pair);
			if (Final.Sender.LastAttributedRetirementToken == Token) break;
		}
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5ms, [] { return false; });
		Final = ReadWire(Pair);
		const bool Conserved = Final.Sender.StructuralPayloadBytesFirstSent == Accepted && Final.Sender.StructuralPayloadBytesAcked == Accepted &&
			Final.Sender.LastAttributedRetirementToken == Token && Final.Sender.LastAttributedRetiredPayloadBytes == Bytes &&
			Final.Sender.ActiveAttributedRetirementToken == 0 && Final.Sender.PendingReliableStreamBytes == 0 &&
			Final.Sender.SentUnackedReliableStreamBytes == 0 && Final.Sender.ReliableStreamBytesRetransmitted == 0;
		const bool F1Healthy = !Final.Sender.StructuralLastCompletedGrantFailed && !Final.Sender.StructuralServiceFailed;
		if (!Conserved || !F1Healthy) {
			// Failure-only evidence: never extend the preceding deadline or hide
			// which independent delivery/accounting or F1 service predicate failed.
			std::cerr << "[Network:AckStatsFailure] prompt=" << Prompt << " token=" << Token
				<< " conserved=" << Conserved << " f1_healthy=" << F1Healthy
				<< " bytes=" << Bytes << " accepted=" << Accepted << " activated_us=" << ActivatedAt
				<< " elapsed_us=" << Now() - ActivatedAt
				<< " deadline_elapsed=" << (std::chrono::steady_clock::now() >= Deadline)
				<< " first_sent_equal=" << (Final.Sender.StructuralPayloadBytesFirstSent == Accepted)
				<< " ack_equal=" << (Final.Sender.StructuralPayloadBytesAcked == Accepted)
				<< " retired_token_equal=" << (Final.Sender.LastAttributedRetirementToken == Token)
				<< " retired_bytes_equal=" << (Final.Sender.LastAttributedRetiredPayloadBytes == Bytes)
				<< " active_clear=" << (Final.Sender.ActiveAttributedRetirementToken == 0)
				<< " pending_clear=" << (Final.Sender.PendingReliableStreamBytes == 0)
				<< " unacked_clear=" << (Final.Sender.SentUnackedReliableStreamBytes == 0)
				<< " retransmission_zero=" << (Final.Sender.ReliableStreamBytesRetransmitted == 0)
				<< " first_sent=" << Final.Sender.StructuralPayloadBytesFirstSent
				<< " acked=" << Final.Sender.StructuralPayloadBytesAcked
				<< " retired_token=" << Final.Sender.LastAttributedRetirementToken
				<< " retired_bytes=" << Final.Sender.LastAttributedRetiredPayloadBytes
				<< " active_token=" << Final.Sender.ActiveAttributedRetirementToken
				<< " pending=" << Final.Sender.PendingReliableStreamBytes
				<< " unacked=" << Final.Sender.SentUnackedReliableStreamBytes
				<< " retransmitted=" << Final.Sender.ReliableStreamBytesRetransmitted
				<< " sender_packets=" << Final.Sender.NativePacketsSent
				<< " sender_packet_bytes=" << Final.Sender.NativePacketBytesSent
				<< " receiver_packets=" << Final.Receiver.NativePacketsSent
				<< " receiver_packet_bytes=" << Final.Receiver.NativePacketBytesSent
				<< " receiver_pending=" << Final.Receiver.PendingReliableStreamBytes
				<< " receiver_unacked=" << Final.Receiver.SentUnackedReliableStreamBytes
				<< " receiver_retransmitted=" << Final.Receiver.ReliableStreamBytesRetransmitted
				<< " grant_failed=" << Final.Sender.StructuralLastCompletedGrantFailed
				<< " service_failed=" << Final.Sender.StructuralServiceFailed << '\n';
			// Use the original Final snapshot. Its completed segment record survives
			// ACK/retirement; never relabel an older completed token as this grant.
			const auto &Sample = Final.Sender;
			std::cerr << "[Network:AckStatsFailureCurve] token=" << Token
				<< " connection_slot=" << Sample.Connection.Slot << " connection_generation=" << Sample.Connection.Generation
				<< " observed_us=" << Sample.ObservedAtMicroseconds
				<< " active_token=" << Sample.ActiveAttributedRetirementToken
				<< " active_bytes=" << Sample.StructuralActiveGrantBytes
				<< " active_first_sent=" << Sample.StructuralActiveGrantFirstSentBytes
				<< " active_started_us=" << Sample.StructuralActiveGrantStartedAtMicroseconds
				<< " completed_sequence=" << Sample.StructuralCompletedGrantSequence
				<< " completed_token=" << Sample.StructuralLastCompletedGrantToken
				<< " completed_token_matches=" << (Sample.StructuralLastCompletedGrantToken == Token)
				<< " completed_bytes=" << Sample.StructuralLastCompletedGrantBytes
				<< " activated_us=" << Sample.StructuralLastCompletedGrantActivatedAtMicroseconds
				<< " first_send_us=" << Sample.StructuralLastCompletedGrantFirstSendAtMicroseconds
				<< " completed_us=" << Sample.StructuralLastCompletedGrantCompletedAtMicroseconds
				<< " completed_max_running_byte_us=" << Sample.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds
				<< " generation_max_finite_shortfall_byte_us=" << Sample.StructuralMaximumFiniteShortfallByteMicroseconds
				<< " peer_rate_bytes_per_second=" << FiniteGrantServiceCurve::PeerRateBytesPerSecond
				<< " finite_intercept_byte_us=" << FiniteGrantServiceCurve::FiniteInterceptByteMicroseconds
				<< " running_bound_byte_us=" << FiniteGrantServiceCurve::RunningBoundByteMicroseconds
				<< " segment_events=" << Sample.LastCompletedStructuralSegmentEventCount
				<< " segment_count_invalid=" << (Sample.LastCompletedStructuralSegmentEventCount > std::size(Sample.LastCompletedStructuralSegmentEvents))
				<< '\n';
			for (std::size_t Index = 0; Index < std::min<std::size_t>(Sample.LastCompletedStructuralSegmentEventCount,
				std::size(Sample.LastCompletedStructuralSegmentEvents)); ++Index) {
				const auto &Event = Sample.LastCompletedStructuralSegmentEvents[Index];
				std::cerr << "[Network:AckStatsFailureSegment] completed_token=" << Sample.StructuralLastCompletedGrantToken
					<< " index=" << Index << " at_us=" << Event.AtMicroseconds << " bytes=" << Event.PayloadBytes << '\n';
			}
			const auto PrintFailureState = [&](const char *Side, auto &Transport, auto Connection) {
				GargantuanAckDiagnostics Trace;
                const bool Available = AckStatsTimingEvidence::ReadNativeSnapshot("grant-failure", Side, Prompt, Token, Trace,
                    [&] { return AckAccess::Read(Transport, Connection, Trace); });
				std::cerr << "[Network:AckStatsFailureState] side=" << Side << " available=" << Available;
				if (Available) std::cerr << " overflow=" << Trace.Overflow << " events=" << Trace.Count
					<< " associated_received_packets=" << Trace.AssociatedReceivedPackets
					<< " associated_received_udp_bytes=" << Trace.AssociatedReceivedUdpBytes
					<< " reliable_packets=" << Trace.ReliablePackets
					<< " first_reliable_us=" << Trace.FirstReliablePacketAt << " last_reliable_us=" << Trace.LastReliablePacketAt
					<< " last_deadline=" << Trace.LastDeadline << " serialized_ack_packet=" << Trace.SerializedAckPacket
					<< " ack_packets=" << Trace.AckPacketsSent << " ack_packet_bytes=" << Trace.AckPacketBytes
					<< " last_ack_us=" << Trace.LastAckPacketSentAt
					<< " grant_wire=" << Trace.GrantWholeWireBytes << " grant_wire_ceiling=" << Trace.GrantWholeWireCeiling
					<< " grant_wire_invalid=" << Trace.GrantWireInvalid << " prompt_allowed=" << Trace.PromptFinalWireAllowed
					<< " prompt_prior_wire=" << Trace.PromptFinalPriorWireBytes << " prompt_tail_budget=" << Trace.PromptTailBudget;
				std::cerr << '\n';
				if (!Available) return;
				PrintState("grant-failure", Side, Trace);
				for (std::size_t Index = 0; Index < std::min<std::size_t>(Trace.Count, std::size(Trace.Events)); ++Index) {
					const auto &Event = Trace.Events[Index];
					std::cerr << "[Network:AckStatsFailureEvent] side=" << Side << " index=" << Index
						<< " kind=" << static_cast<int>(Event.Type) << " identity=" << Event.Identity
						<< " at_us=" << Event.AtMicroseconds << " native_at_us=" << Event.NativeAtMicroseconds
						<< " value=" << Event.Value << '\n';
				}
			};
			PrintFailureState("sender", *Pair.Server, Pair.ServerConnection);
			PrintFailureState("receiver", *Pair.Client, Pair.ClientConnection);
		}
		Require(Conserved, "stats-boundary grant did not conserve/retire its exact bytes");
		Require(F1Healthy,
			"stats boundary violated unchanged F1");
		ExactPayloads(Pair, {Payload(Bytes, std::byte{0x49})});
		Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender), "grant prompt evidence missing");
		const auto Count = Requests(Sender, Token);
		Require(Count == (Prompt && Bytes == 393652 ? 1u : 0u), "stats-boundary prompt was missing/duplicated/unfunded");
		Prompts += Count;
		std::this_thread::sleep_until(std::min(End, CycleStart + 500ms * static_cast<std::int64_t>(Token)));
	}
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 100ms, [] { return false; });
    Require(AckStatsTimingEvidence::ReadNativeSnapshot("terminal", "sender", Prompt, Grants, Sender,
            [&] { return AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender); }) &&
        AckStatsTimingEvidence::ReadNativeSnapshot("terminal", "receiver", Prompt, Grants, Receiver,
            [&] { return AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver); }), "final stats missing");
	SenderTotals.Add(Sender); ReceiverTotals.Add(Receiver);
	const auto Whole = Cost(WireBefore, ReadWire(Pair));
	const auto ObservedUs = Now() - StartUs;
	// Independent whole-connection check, including denied tiny grants and
	// the post-retirement tail. One actual peer has two endpoint emitters;
	// each periodic class can send one maximum request and confirmation.
	// Keep this arithmetic independent of the production budget helper.
	constexpr std::uint64_t MaximumWire = 1300 + 48;
	constexpr std::uint64_t PairRequestConfirmation = 2 * 2 * MaximumWire;
	constexpr std::uint64_t PeriodicNumerator = PairRequestConfirmation * (24 + 6 + 1);
	constexpr std::uint64_t PairPeriodicRate = (PeriodicNumerator + 119) / 120;
	constexpr std::uint64_t PopulationPeriodicRate = (32 * PeriodicNumerator + 119) / 120;
	const PooledReliableServiceProfile Profile;
	const auto ResidualRate = Profile.RequiredTransportReserve - PopulationPeriodicRate;
	const auto WholeWireLimit = Accepted + Accepted * ResidualRate / Profile.StructuralPool +
		3 * PairRequestConfirmation + (ObservedUs * PairPeriodicRate + 999999) / 1000000;
	PrintState("terminal", "sender", Sender); PrintState("terminal", "receiver", Receiver);
	std::cout << "[Network:AckStatsCoverage] prompt=" << Prompt << " grants=" << Grants
		<< " tracer_requests=" << SenderTotals.Tracers + ReceiverTotals.Tracers
		<< " instantaneous_sent=" << SenderTotals.Instantaneous + ReceiverTotals.Instantaneous
		<< " instantaneous_received=" << SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived
		<< " need_mask=" << (SenderTotals.NeedMask | ReceiverTotals.NeedMask) << '\n';
	Require(Whole.ConservativeIpv6Bytes <= WholeWireLimit,
		"whole connection exceeded independently derived grant plus periodic-control funding");
	Require(Grants == 48 && SenderTotals.Tracers + ReceiverTotals.Tracers > 0 &&
		SenderTotals.Instantaneous + ReceiverTotals.Instantaneous > 0 &&
		SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived > 0,
		"actual native tracer and instantaneous stats paths were not observed");
	Require(!Prompt || (SenderTotals.ImmediateSent >= Prompts && ReceiverTotals.ImmediateReceived >= Prompts),
		"successful final-packet prompts were not independently received");
	std::cout << "[Network:AckStatsBoundary] prompt=" << Prompt << " grants=" << Grants
		<< " accepted=" << Accepted << " prompts=" << Prompts << " observation_us=" << ObservedUs
		<< " tracer_requests=" << SenderTotals.Tracers + ReceiverTotals.Tracers
		<< " instantaneous_sent=" << SenderTotals.Instantaneous + ReceiverTotals.Instantaneous
		<< " instantaneous_received=" << SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived
		<< " first_sender_tracer_us=" << SenderTotals.FirstTracer
		<< " first_receiver_tracer_us=" << ReceiverTotals.FirstTracer
		<< " first_sender_instant_us=" << SenderTotals.FirstInstantaneous
		<< " first_receiver_instant_us=" << ReceiverTotals.FirstInstantaneous
		<< " sender_packets=" << Whole.SenderPackets << " receiver_packets=" << Whole.ReceiverPackets
		<< " whole_ipv6_bound=" << Whole.ConservativeIpv6Bytes << " whole_wire_limit=" << WholeWireLimit
		<< " lifetime_120s=NOT_MEASURED result=PASS\n";
}

inline bool Run() {
    AckStatsTimingEvidence::Session Diagnostic;
	try {
		Observe(false);
		Diagnostic.StopTiming(); // Control pair is destroyed/joined; both native arms still run.
		Observe(true);
		return true;
	}
	catch (const std::exception &Error) {
		std::cerr << "[Network:AckStatsBoundary] FAIL " << Error.what() << '\n'; return false;
	}
}
}

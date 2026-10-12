#pragma once
#include <iterator>
#include "AckStatsTimingEvidence.hpp"

// Real native timers, no clock override. Included after the shared production
// adapter fixture and compatibility helpers.
namespace GnsAckStatsBoundaryFixture {
using namespace GnsFundedAckCompatibilityFixture;

inline bool GrantConserved(const Feedback &Sender, std::uint64_t Accepted,
	std::uint64_t Token, std::uint64_t Bytes) noexcept {
	return Sender.CountersValid && Sender.StructuralPayloadBytesFirstSent == Accepted &&
		Sender.StructuralPayloadBytesAcked == Accepted &&
		Sender.UniqueReliableStreamBytesFirstSent == Sender.UniqueReliableStreamBytesAcked &&
		Sender.LastAttributedRetirementToken == Token && Sender.LastAttributedRetiredPayloadBytes == Bytes &&
		Sender.ActiveAttributedRetirementToken == 0 && Sender.PendingReliableStreamBytes == 0 &&
		Sender.SentUnackedReliableStreamBytes == 0;
}

inline bool GrantCompletionHealthy(const Feedback &Sender, std::uint64_t Token,
	std::uint64_t Bytes, std::uint64_t ActivatedAt) noexcept {
	using Curve = gargantuan::network::NativeGrantCompletionCurve;
	if (!Sender.CountersValid || Sender.StructuralCompletionEvidenceInvalid ||
		Sender.StructuralCompletionServiceFailed || Sender.StructuralLastCompletedCompletionFailed ||
		!Sender.StructuralCompletedGrantSequence || Sender.StructuralLastCompletedGrantToken != Token ||
		Sender.StructuralLastCompletedGrantBytes != Bytes ||
		Sender.StructuralLastCompletedGrantActivatedAtMicroseconds != ActivatedAt ||
		!Sender.StructuralLastCompletedGrantFirstSendAtMicroseconds ||
		Sender.StructuralLastCompletedGrantCompletedAtMicroseconds > Sender.ObservedAtMicroseconds ||
		!Sender.LastCompletedStructuralSegmentEventCount ||
		Sender.LastCompletedStructuralSegmentEventCount > Sender.LastCompletedStructuralSegmentEvents.size()) return false;
	// Replay the existing bounded positive-native BODY record. Legacy packet-gap
	// flags remain diagnostics; they cannot substitute for this D03 certificate.
	Curve Completion;
	if (!Completion.Activate(Token, Bytes, ActivatedAt)) return false;
	for (std::uint32_t Index = 0; Index < Sender.LastCompletedStructuralSegmentEventCount; ++Index) {
		const auto &Event = Sender.LastCompletedStructuralSegmentEvents[Index];
		if (!Completion.FirstSend(Token, Event.PayloadBytes, Event.AtMicroseconds)) return false;
	}
	return Completion.FirstSentBytes == Bytes &&
		Completion.FirstSentAtMicroseconds == Sender.StructuralLastCompletedGrantFirstSendAtMicroseconds &&
		Completion.CompletedAtMicroseconds == Sender.StructuralLastCompletedGrantCompletedAtMicroseconds;
}

inline void CheckCompletionDefinition() {
	const auto Certificate = [](std::uint64_t Bytes, std::uint64_t Elapsed) {
		Feedback Value;
		Value.StructuralCompletedGrantSequence = 34;
		Value.StructuralLastCompletedGrantToken = 34; Value.StructuralLastCompletedGrantBytes = Bytes;
		Value.StructuralLastCompletedGrantActivatedAtMicroseconds = 1000;
		Value.StructuralLastCompletedGrantFirstSendAtMicroseconds = 1000 + Elapsed;
		Value.StructuralLastCompletedGrantCompletedAtMicroseconds = 1000 + Elapsed;
		Value.ObservedAtMicroseconds = 1000 + Elapsed;
		for (std::uint64_t Remaining = Bytes; Remaining;) {
			const auto Part = std::min<std::uint64_t>(Remaining, 1248);
			Value.LastCompletedStructuralSegmentEvents[Value.LastCompletedStructuralSegmentEventCount++] =
				{1000 + Elapsed, Part};
			Remaining -= Part;
		}
		return Value;
	};
	for (const auto Bytes : {std::uint64_t{77}, std::uint64_t{393652}, std::uint64_t{524288}}) {
		Require(GrantCompletionHealthy(Certificate(Bytes, 37324), 34, Bytes, 1000) &&
			!GrantCompletionHealthy(Certificate(Bytes, 37325), 34, Bytes, 1000),
			"D03 tiny/medium/maximum grants retain the exact integer completion boundary");
	}
	auto Timely = Certificate(393652, 21794);
	Timely.StructuralLastCompletedGrantFailed = Timely.StructuralServiceFailed = true;
	Timely.StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds = 27736588736ULL;
	Require(GrantCompletionHealthy(Timely, 34, 393652, 1000),
		"legacy running failure must remain nonfatal for a complete D03-native certificate");
	for (unsigned Case = 0; Case < 19; ++Case) {
		auto Bad = Timely;
		if (Case == 0) Bad.CountersValid = false;
		if (Case == 1) Bad.StructuralCompletionEvidenceInvalid = true;
		if (Case == 2) Bad.StructuralCompletionServiceFailed = true;
		if (Case == 3) Bad.StructuralLastCompletedCompletionFailed = true;
		if (Case == 4) Bad.StructuralCompletedGrantSequence = 0;
		if (Case == 5) ++Bad.StructuralLastCompletedGrantToken;
		if (Case == 6) --Bad.StructuralLastCompletedGrantBytes;
		if (Case == 7) ++Bad.StructuralLastCompletedGrantActivatedAtMicroseconds;
		if (Case == 8) Bad.StructuralLastCompletedGrantFirstSendAtMicroseconds = 0;
		if (Case == 9) Bad.StructuralLastCompletedGrantCompletedAtMicroseconds = 0;
		if (Case == 10) --Bad.ObservedAtMicroseconds;
		if (Case == 11) Bad.LastCompletedStructuralSegmentEventCount = 0;
		if (Case == 12) Bad.LastCompletedStructuralSegmentEventCount = 513;
		if (Case == 13) --Bad.LastCompletedStructuralSegmentEvents[0].PayloadBytes;
		if (Case == 14) ++Bad.LastCompletedStructuralSegmentEvents[0].PayloadBytes;
		if (Case == 15) Bad.LastCompletedStructuralSegmentEvents[0].PayloadBytes = 0;
		if (Case == 16) Bad.LastCompletedStructuralSegmentEvents[0].AtMicroseconds = 999;
		if (Case == 17) --Bad.LastCompletedStructuralSegmentEvents[1].AtMicroseconds;
		if (Case == 18) Bad.LastCompletedStructuralSegmentEvents[0].AtMicroseconds =
			std::numeric_limits<std::uint64_t>::max();
		Require(!GrantCompletionHealthy(Bad, 34, 393652, 1000),
			"D03 certificate rejects incomplete BODY, identity/chronology/evidence errors and hard failures");
	}
	Require(!GrantCompletionHealthy(Timely, 0, 393652, 1000) &&
		!GrantCompletionHealthy(Timely, 34, 0, 1000) &&
		!GrantCompletionHealthy(Timely, 34, 524289, 1000) &&
		!GrantCompletionHealthy(Timely, 34, 393652, 0), "invalid activation cannot certify native service");
	std::cout << "[Network:AckStatsBoundary] CompletionDefinition=PASS contract=HYBRID_F1_V1\n";
}

inline std::uint64_t WholeWireCeiling(std::uint64_t Accepted, std::uint64_t ObservedUs) {
	// Independent funding arithmetic, including retries, both endpoint emitters
	// and the post-retirement tail. Reused at each prefix and at termination.
	constexpr std::uint64_t MaximumWire = 1300 + 48;
	constexpr std::uint64_t PairRequestConfirmation = 2 * 2 * MaximumWire;
	constexpr std::uint64_t PeriodicNumerator = PairRequestConfirmation * (24 + 6 + 1);
	constexpr std::uint64_t PairPeriodicRate = (PeriodicNumerator + 119) / 120;
	constexpr std::uint64_t PopulationPeriodicRate = (32 * PeriodicNumerator + 119) / 120;
	const PooledReliableServiceProfile Profile;
	const auto ResidualRate = Profile.RequiredTransportReserve - PopulationPeriodicRate;
	return Accepted + Accepted * ResidualRate / Profile.StructuralPool +
		3 * PairRequestConfirmation + (ObservedUs * PairPeriodicRate + 999999) / 1000000;
}

inline void CheckConservationDefinition() {
	Feedback Observed;
	Observed.StructuralPayloadBytesFirstSent = Observed.StructuralPayloadBytesAcked = 231;
	Observed.UniqueReliableStreamBytesFirstSent = Observed.UniqueReliableStreamBytesAcked = 237;
	Observed.LastAttributedRetirementToken = 3; Observed.LastAttributedRetiredPayloadBytes = 77;
	Observed.ReliableStreamBytesRetransmitted = 79;
	const auto Conserved = GrantConserved(Observed, 231, 3, 77);
	Require(Conserved && !(Conserved && Observed.ReliableStreamBytesRetransmitted == 0),
		"old zero-retry predicate rejects exact conserved observed token 3; retries are independent wire cost");
	for (unsigned Case = 0; Case < 9; ++Case) {
		auto Bad = Observed;
		if (Case == 0) Bad.StructuralPayloadBytesFirstSent += 79;
		if (Case == 1) Bad.StructuralPayloadBytesAcked += 79;
		if (Case == 2) Bad.UniqueReliableStreamBytesFirstSent += 79;
		if (Case == 3) ++Bad.LastAttributedRetirementToken;
		if (Case == 4) ++Bad.LastAttributedRetiredPayloadBytes;
		if (Case == 5) Bad.ActiveAttributedRetirementToken = 3;
		if (Case == 6) Bad.PendingReliableStreamBytes = 1;
		if (Case == 7) Bad.SentUnackedReliableStreamBytes = 1;
		if (Case == 8) Bad.CountersValid = false;
		Require(!GrantConserved(Bad, 231, 3, 77), "retry cannot become unique service/ACK or hide an incorrect or incomplete owner");
	}
	const auto PrefixLimit = WholeWireCeiling(231, 553375);
	const auto PrefixOvershoot = PrefixLimit + 1;
	Require(!(PrefixOvershoot <= PrefixLimit) && PrefixOvershoot <= WholeWireCeiling(231 + 393652, 553375),
		"a later grant cannot fund an earlier completed-prefix overshoot");
	std::cout << "[Network:AckStatsBoundary] ConservationDefinition=PASS\n";
}

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
		const bool Conserved = GrantConserved(Final.Sender, Accepted, Token, Bytes);
		const bool LegacyF1Healthy = !Final.Sender.StructuralLastCompletedGrantFailed && !Final.Sender.StructuralServiceFailed;
		const bool CompletionHealthy = GrantCompletionHealthy(Final.Sender, Token, Bytes, ActivatedAt);
		if (!Conserved || !CompletionHealthy || !LegacyF1Healthy) {
			// Failure-only evidence: never extend the preceding deadline or hide
			// which independent delivery/accounting or F1 service predicate failed.
			std::cerr << "[Network:AckStatsObservation] prompt=" << Prompt << " token=" << Token
				<< " conserved=" << Conserved << " legacy_f1_healthy=" << LegacyF1Healthy
				<< " completion_healthy=" << CompletionHealthy
				<< " diagnostic_only=" << (Conserved && CompletionHealthy)
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
				<< " stream_equal=" << (Final.Sender.UniqueReliableStreamBytesFirstSent == Final.Sender.UniqueReliableStreamBytesAcked)
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
				<< " service_failed=" << Final.Sender.StructuralServiceFailed
				<< " completion_failed=" << Final.Sender.StructuralCompletionServiceFailed
				<< " completed_completion_failed=" << Final.Sender.StructuralLastCompletedCompletionFailed
				<< " completion_invalid=" << Final.Sender.StructuralCompletionEvidenceInvalid << '\n';
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
		Require(Cost(WireBefore, Final).ConservativeIpv6Bytes <= WholeWireCeiling(Accepted, Now() - StartUs),
			"completed grant prefix exceeded independently derived whole-connection wire funding");
		Require(CompletionHealthy, "stats boundary violated exact D03 native completion");
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
	const auto TerminalWire = ReadWire(Pair);
	const auto Whole = Cost(WireBefore, TerminalWire);
	const auto ObservedUs = Now() - StartUs;
	// Independent whole-connection check, including denied tiny grants and
	// the post-retirement tail. One actual peer has two endpoint emitters;
	// each periodic class can send one maximum request and confirmation.
	// Keep this arithmetic independent of the production budget helper.
	const auto WholeWireLimit = WholeWireCeiling(Accepted, ObservedUs);
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
		<< " sender_retransmitted=" << TerminalWire.Sender.ReliableStreamBytesRetransmitted
		<< " receiver_retransmitted=" << TerminalWire.Receiver.ReliableStreamBytesRetransmitted
		<< " whole_ipv6_bound=" << Whole.ConservativeIpv6Bytes << " whole_wire_limit=" << WholeWireLimit
		<< " lifetime_120s=NOT_MEASURED result=PASS\n";
}

inline bool Run() {
    AckStatsTimingEvidence::Session Diagnostic;
	try {
		CheckConservationDefinition();
		CheckCompletionDefinition();
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

#pragma once

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
	OwnedPair Owner; auto &Pair = Owner.Pair;
	Require(Pair.ServerConnection.IsValid(), "stats-boundary real connection missing");
	if (Prompt) Require(AckAccess::PromptFinalGrantAck(*Pair.Server, Pair.ServerConnection, true, 1348),
		"stats-boundary funded policy rejected");
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
	Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) &&
		AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver), "quiet interval stats missing");
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
		Require(Access::Attribute(Message, Token) && Access::Activate(Message, Token, Now()),
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
		Require(Final.Sender.StructuralPayloadBytesFirstSent == Accepted && Final.Sender.StructuralPayloadBytesAcked == Accepted &&
			Final.Sender.LastAttributedRetirementToken == Token && Final.Sender.LastAttributedRetiredPayloadBytes == Bytes &&
			Final.Sender.ActiveAttributedRetirementToken == 0 && Final.Sender.PendingReliableStreamBytes == 0 &&
			Final.Sender.SentUnackedReliableStreamBytes == 0 && Final.Sender.ReliableStreamBytesRetransmitted == 0,
			"stats-boundary grant did not conserve/retire its exact bytes");
		Require(!Final.Sender.StructuralLastCompletedGrantFailed && !Final.Sender.StructuralServiceFailed,
			"stats boundary violated unchanged F1");
		ExactPayloads(Pair, {Payload(Bytes, std::byte{0x49})});
		Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender), "grant prompt evidence missing");
		const auto Count = Requests(Sender, Token);
		Require(Count == (Prompt && Bytes == 393652 ? 1u : 0u), "stats-boundary prompt was missing/duplicated/unfunded");
		Prompts += Count;
		std::this_thread::sleep_until(std::min(End, CycleStart + 500ms * static_cast<std::int64_t>(Token)));
	}
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 100ms, [] { return false; });
	Require(AckAccess::Read(*Pair.Server, Pair.ServerConnection, Sender) &&
		AckAccess::Read(*Pair.Client, Pair.ClientConnection, Receiver), "final stats missing");
	SenderTotals.Add(Sender); ReceiverTotals.Add(Receiver);
	const auto Whole = Cost(WireBefore, ReadWire(Pair));
	PrintState("terminal", "sender", Sender); PrintState("terminal", "receiver", Receiver);
	std::cout << "[Network:AckStatsCoverage] prompt=" << Prompt << " grants=" << Grants
		<< " tracer_requests=" << SenderTotals.Tracers + ReceiverTotals.Tracers
		<< " instantaneous_sent=" << SenderTotals.Instantaneous + ReceiverTotals.Instantaneous
		<< " instantaneous_received=" << SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived
		<< " need_mask=" << (SenderTotals.NeedMask | ReceiverTotals.NeedMask) << '\n';
	Require(Grants == 48 && SenderTotals.Tracers + ReceiverTotals.Tracers > 0 &&
		SenderTotals.Instantaneous + ReceiverTotals.Instantaneous > 0 &&
		SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived > 0,
		"actual native tracer and instantaneous stats paths were not observed");
	Require(!Prompt || (SenderTotals.ImmediateSent >= Prompts && ReceiverTotals.ImmediateReceived >= Prompts),
		"successful final-packet prompts were not independently received");
	std::cout << "[Network:AckStatsBoundary] prompt=" << Prompt << " grants=" << Grants
		<< " accepted=" << Accepted << " prompts=" << Prompts << " observation_us=" << Now() - StartUs
		<< " tracer_requests=" << SenderTotals.Tracers + ReceiverTotals.Tracers
		<< " instantaneous_sent=" << SenderTotals.Instantaneous + ReceiverTotals.Instantaneous
		<< " instantaneous_received=" << SenderTotals.InstantaneousReceived + ReceiverTotals.InstantaneousReceived
		<< " first_sender_tracer_us=" << SenderTotals.FirstTracer
		<< " first_receiver_tracer_us=" << ReceiverTotals.FirstTracer
		<< " first_sender_instant_us=" << SenderTotals.FirstInstantaneous
		<< " first_receiver_instant_us=" << ReceiverTotals.FirstInstantaneous
		<< " sender_packets=" << Whole.SenderPackets << " receiver_packets=" << Whole.ReceiverPackets
		<< " whole_ipv6_bound=" << Whole.ConservativeIpv6Bytes << " lifetime_120s=NOT_MEASURED result=PASS\n";
}

inline bool Run() {
	try { Observe(false); Observe(true); return true; }
	catch (const std::exception &Error) {
		std::cerr << "[Network:AckStatsBoundary] FAIL " << Error.what() << '\n'; return false;
	}
}
}

#include "gargantuan/network/SimulatedTransport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (!Condition) {
			std::cerr << "FAIL: " << Message << '\n';
			++Failures;
		}
	}

	NetworkLimits TestLimits(
		std::size_t ReliableBytes = 1024,
		std::size_t UnreliableBytes = 512,
		std::size_t ReliableQueueBytes = 4096
	) {
		return {
			.MaximumReliableMessageBytes = ReliableBytes,
			.MaximumUnreliableMessageBytes = UnreliableBytes,
			.MaximumQueuedReliableBytes = ReliableQueueBytes,
			.MaximumInFlightRemoteRequests = 32,
			.MaximumDecodedMessageBytes = std::max(ReliableBytes, UnreliableBytes),
			.MaximumSendBytesPerTick = std::max(ReliableBytes, UnreliableBytes),
			.MaximumReceiveBytesPerTick = std::max(ReliableBytes, UnreliableBytes),
			.MaximumMessagesPerTick = 4096,
		};
	}

	std::vector<TransportEvent> Drain(const std::shared_ptr<SimulatedTransport> &Transport) {
		std::vector<TransportEvent> Result;
		std::array<TransportEvent, 256> Buffer;
		for (;;) {
			const auto Count = Transport->PollEvents(Buffer);
			if (Count == 0) break;
			for (std::size_t Index = 0; Index < Count; ++Index) Result.push_back(std::move(Buffer[Index]));
		}
		return Result;
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events) {
			if (const auto *State = std::get_if<ConnectionStateEvent>(&Event);
				State && State->Current == ConnectionState::Connected) return State->Connection;
		}
		return {};
	}

	std::vector<unsigned int> PayloadValues(const std::vector<TransportEvent> &Events) {
		std::vector<unsigned int> Result;
		for (const auto &Event : Events) {
			if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event); Message && !Message->Payload.empty())
				Result.push_back(std::to_integer<unsigned int>(Message->Payload.front()));
		}
		return Result;
	}

	std::size_t CountDisconnects(const std::vector<TransportEvent> &Events) {
		std::size_t Result = 0;
		for (const auto &Event : Events) if (std::holds_alternative<DisconnectedEvent>(Event)) ++Result;
		return Result;
	}

	struct PairFixture {
		std::shared_ptr<SimulatedNetwork> Network;
		std::shared_ptr<SimulatedTransport> Server;
		std::shared_ptr<SimulatedTransport> Client;
		NetworkLimits Limits;
		ConnectionId ServerConnection;
		ConnectionId ClientConnection;
	};

	PairFixture StartPair(
		SimulatedTransportConfiguration Configuration = {},
		NetworkLimits Limits = TestLimits(),
		std::string EndpointName = "test"
	) {
		PairFixture Result;
		Result.Network = SimulatedNetwork::Create(Configuration);
		Result.Limits = Limits;
		if (!Result.Network) return Result;
		Result.Server = Result.Network->CreateTransport();
		Result.Client = Result.Network->CreateTransport();
		if (!Result.Server || !Result.Client) return Result;
		TransportStartConfiguration ServerStart{
			.Role = TransportRole::Server,
			.Endpoint = {EndpointName, 1},
			.AdvertisedLimits = Limits,
		};
		TransportStartConfiguration ClientStart{
			.Role = TransportRole::Client,
			.Endpoint = {EndpointName, 1},
			.AdvertisedLimits = Limits,
		};
		if (!Result.Server->Start(ServerStart).Succeeded() || !Result.Client->Start(ClientStart).Succeeded())
			return Result;
		Check(Drain(Result.Server).empty() && Drain(Result.Client).empty(),
			"connection events remain pending until the explicit pump");
		Result.Network->Pump();
		auto ServerEvents = Drain(Result.Server);
		auto ClientEvents = Drain(Result.Client);
		Result.ServerConnection = ConnectedId(ServerEvents);
		Result.ClientConnection = ConnectedId(ClientEvents);
		return Result;
	}

	std::optional<NetworkMessageIntent> Message(
		ConnectionId Destination,
		DeliveryMode Delivery,
		unsigned int Value,
		const NetworkLimits &Limits,
		MessageOrder Order = std::monostate{},
		std::size_t Bytes = 1
	) {
		std::vector<std::byte> Payload(Bytes, static_cast<std::byte>(Value));
		return MakeNetworkMessageIntent(
			Destination,
			Delivery,
			Delivery == DeliveryMode::ReliableOrdered ? TrafficClass::ReliableApplication : TrafficClass::EphemeralApplication,
			std::move(Order),
			std::move(Payload),
			Limits
		);
	}

	std::vector<unsigned int> RunDeterministicTrace(std::uint64_t Seed, const std::vector<std::chrono::microseconds> &Steps) {
		SimulatedTransportConfiguration Configuration;
		Configuration.Seed = Seed;
		Configuration.BaseLatency = 2ms;
		Configuration.MaximumJitter = 8ms;
		Configuration.MaximumReorderDelay = 20ms;
		Configuration.UnreliableLossProbability = 0.25;
		Configuration.UnreliableDuplicationProbability = 0.35;
		Configuration.UnreliableReorderProbability = 0.75;
		Configuration.BandwidthBytesPerSecond = 1'000'000;
		auto Pair = StartPair(Configuration);
		std::vector<unsigned int> Trace;
		for (unsigned int Value = 1; Value <= 64; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, Value, Pair.Limits);
			if (Intent) Pair.Client->Send(*Intent);
		}
		for (const auto Step : Steps) {
			(void)Pair.Network->Advance(Step);
			Pair.Network->Pump();
			auto Values = PayloadValues(Drain(Pair.Server));
			Trace.insert(Trace.end(), Values.begin(), Values.end());
		}
		return Trace;
	}
}

int main() {
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	{
		SimulatedTransportConfiguration Configuration;
		Check(Configuration.IsValid() && SimulatedNetwork::Create(Configuration),
			"the bounded default simulator configuration is valid");
		Configuration.UnreliableLossProbability = std::numeric_limits<double>::quiet_NaN();
		Check(!Configuration.IsValid(), "NaN probabilities are rejected");
		Configuration = {};
		Configuration.UnreliableDuplicationProbability = std::numeric_limits<double>::infinity();
		Check(!Configuration.IsValid(), "infinite probabilities are rejected");
		Configuration = {};
		Configuration.UnreliableReorderProbability = 1.01;
		Check(!Configuration.IsValid(), "probabilities outside zero through one are rejected");
		Configuration = {};
		Configuration.BaseLatency = -1us;
		Check(!Configuration.IsValid(), "negative durations are rejected");
		Configuration = {};
		Configuration.MaximumJitter = MaximumSimulatedDuration + 1us;
		Check(!Configuration.IsValid(), "pathological simulated durations are rejected");
		Configuration = {};
		Configuration.BandwidthBytesPerSecond = 0;
		Check(!Configuration.IsValid(), "zero bandwidth is rejected");
		Configuration = {};
		Configuration.MaximumUnreliableDatagramBytes = 0;
		Check(!Configuration.IsValid(), "zero MTU is rejected");
		Configuration = {};
		Configuration.MaximumReliableQueueBytes = NativeMaximumQueuedReliableBytes + 1;
		Check(!Configuration.IsValid(), "queue limits above native safety ceilings are rejected");
		Configuration = {};
		Configuration.ConnectionLifetime = 0us;
		Check(!Configuration.IsValid(), "zero configured connection lifetime is rejected");
		Configuration = {};
		Configuration.MaximumPendingEventsPerTransport = Configuration.MaximumConnections * 4 - 1;
		Check(!Configuration.IsValid(), "event capacity must reserve terminal lifecycle outcomes");
		Configuration = {};
		Configuration.Seed = 0;
		Check(Configuration.IsValid(), "the zero seed is a valid deterministic seed");
	}

	{
		auto Network = SimulatedNetwork::Create({});
		auto Server = Network->CreateTransport();
		TransportStartConfiguration InvalidRole{
			.Role = static_cast<TransportRole>(255),
			.Endpoint = {"invalid", 1},
			.AdvertisedLimits = TestLimits(),
		};
		Check(Server->Start(InvalidRole).Status == TransportOperationStatus::MessageRejected,
			"invalid topology roles fail before simulator activation");
		TransportStartConfiguration MissingServer{
			.Role = TransportRole::Client,
			.Endpoint = {"missing", 1},
			.AdvertisedLimits = TestLimits(),
		};
		Check(Server->Start(MissingServer).Status == TransportOperationStatus::TransportFailure,
			"client startup fails atomically when the symbolic server endpoint is absent");
		TransportStartConfiguration ServerStart{
			.Role = TransportRole::Server,
			.Endpoint = {"duplicate", 1},
			.AdvertisedLimits = TestLimits(),
		};
		auto OtherServer = Network->CreateTransport();
		Check(Server->Start(ServerStart).Succeeded() &&
			OtherServer->Start(ServerStart).Status == TransportOperationStatus::InvalidState,
			"duplicate symbolic server endpoints are rejected");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumTransports = 1;
		auto Network = SimulatedNetwork::Create(Configuration);
		Check(Network->CreateTransport() && !Network->CreateTransport(),
			"topology transport allocation obeys its validated hard ceiling");
	}

	{
		auto Network = SimulatedNetwork::Create({});
		Check(Network->Advance(std::chrono::microseconds::max()) && !Network->Advance(1us) &&
			Network->Now() == std::chrono::microseconds::max(),
			"simulated clock exhaustion fails closed without wrapping time");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.BaseLatency = 10us;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, 1, Pair.Limits);
		(void)Pair.Network->Advance(std::chrono::microseconds::max() - 5us);
		auto Before = Pair.Client->GetStatistics(Pair.ClientConnection);
		auto Rejected = Pair.Client->Send(*Intent);
		auto After = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Before && After && Rejected.Status == TransportOperationStatus::ResourceExhausted &&
			After->MessagesSent == Before->MessagesSent && After->BytesSent == Before->BytesSent &&
			After->QueuedReliableBytes == Before->QueuedReliableBytes,
			"delivery-time overflow rejects atomically without consuming bandwidth or statistics");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumReliableQueueBytes = 32;
		auto Network = SimulatedNetwork::Create(Configuration);
		auto Server = Network->CreateTransport();
		TransportStartConfiguration Start{
			.Role = TransportRole::Server,
			.Endpoint = {"small-queue", 1},
			.AdvertisedLimits = TestLimits(64, 32, 128),
		};
		Check(Server->Start(Start).Status == TransportOperationStatus::ResourceExhausted,
			"a reliable queue smaller than one valid reliable message fails before activation");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.BaseLatency = 10ms;
		Configuration.MaximumJitter = 5ms;
		Configuration.UnreliableLossProbability = 1.0;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		Check(Pair.ServerConnection.IsValid() && Pair.ClientConnection.IsValid(),
			"the in-memory topology establishes generation-safe local connection identities");
		for (unsigned int Value = 1; Value <= 3; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, Value, Pair.Limits);
			Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "healthy reliable sends are accepted");
		}
		Check(Pair.Network->Advance(9ms) && Pair.Network->Pump() == 0 && Drain(Pair.Server).empty(),
			"base latency prevents early delivery");
		Check(!Pair.Network->Advance(-1us), "simulated time rejects backward movement");
		(void)Pair.Network->Advance(20ms);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)) == std::vector<unsigned int>({1, 2, 3}),
			"reliable delivery survives simulated unreliable loss and preserves send order");
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Statistics && Statistics->MessagesSent == 3 && Statistics->MessagesDelivered == 3 &&
			Statistics->QueuedReliableBytes == 0,
			"reliable statistics report accepted, delivered, and drained work");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.UnreliableLossProbability = 1.0;
		auto Pair = StartPair(Configuration);
		auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, 7, Pair.Limits);
		Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "lossy unreliable sends remain best-effort successes");
		(void)Pair.Network->Advance(1s);
		Pair.Network->Pump();
		Check(Drain(Pair.Server).empty(), "configured unreliable loss removes delivery without retry state");
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Statistics && Statistics->DroppedUnreliableMessages == 1 && Statistics->MessageLossRatio == 1.0,
			"unreliable loss is explicit in simulator statistics");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.UnreliableDuplicationProbability = 1.0;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, 9, Pair.Limits);
		Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "duplicated unreliable send is accepted once");
		(void)Pair.Network->Advance(10us);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)) == std::vector<unsigned int>({9, 9}),
			"the fault model can duplicate unreliable messages");
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		auto ReceiverStatistics = Pair.Server->GetStatistics(Pair.ServerConnection);
		Check(Statistics && Statistics->MessagesSent == 1 && Statistics->MessagesDelivered == 2 &&
			Statistics->BytesSent == 1 && Statistics->DuplicatedUnreliableMessages == 1 &&
			ReceiverStatistics && ReceiverStatistics->MessagesReceived == 2 && ReceiverStatistics->BytesReceived == 2,
			"duplication statistics distinguish one accepted send from two delivered and received copies");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.Seed = 9;
		Configuration.BaseLatency = 10ms;
		Configuration.MaximumJitter = 5ms;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, 4, Pair.Limits);
		Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "bounded jitter test message is accepted");
		(void)Pair.Network->Advance(10ms);
		Pair.Network->Pump();
		Check(Drain(Pair.Server).empty(), "base latency plus serialization is a hard lower delivery bound");
		(void)Pair.Network->Advance(5001us);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)) == std::vector<unsigned int>({4}),
			"delivery jitter remains within its configured upper bound");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.Seed = 3;
		Configuration.MaximumReorderDelay = 100ms;
		Configuration.UnreliableReorderProbability = 1.0;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		for (unsigned int Value = 1; Value <= 8; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, Value, Pair.Limits);
			if (Intent) Pair.Client->Send(*Intent);
		}
		(void)Pair.Network->Advance(200ms);
		Pair.Network->Pump();
		auto Values = PayloadValues(Drain(Pair.Server));
		Check(Values.size() == 8 && Values != std::vector<unsigned int>({1, 2, 3, 4, 5, 6, 7, 8}),
			"bounded reorder delay can deterministically reorder unreliable messages");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.Seed = 11;
		Configuration.MaximumReorderDelay = 100ms;
		Configuration.UnreliableReorderProbability = 1.0;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		auto Older = Message(Pair.ClientConnection, DeliveryMode::UnreliableSequenced, 1, Pair.Limits,
			RealtimeStateOrder{StateChannelId(5), RealtimeStateSequence(1)});
		auto Newer = Message(Pair.ClientConnection, DeliveryMode::UnreliableSequenced, 2, Pair.Limits,
			RealtimeStateOrder{StateChannelId(5), RealtimeStateSequence(2)});
		Check(Older && Newer && Pair.Client->Send(*Older).Succeeded() && Pair.Client->Send(*Newer).Succeeded(),
			"sequenced state messages are accepted with strong channel metadata");
		(void)Pair.Network->Advance(200ms);
		Pair.Network->Pump();
		auto Values = PayloadValues(Drain(Pair.Server));
		Check(Values == std::vector<unsigned int>({2}),
			"a reordered older sequenced state is discarded after the newer sequence is accepted");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumConnections = 1;
		Configuration.MaximumPendingEventsPerTransport = 4;
		Configuration.MaximumScheduledEvents = 8;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Pair = StartPair(Configuration);
		for (unsigned int Value = 1; Value <= 2; ++Value) {
			auto Filler = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, Value, Pair.Limits);
			Check(Filler && Pair.Client->Send(*Filler).Succeeded(),
				"receive-queue sequence test fills only the unreserved message capacity");
		}
		(void)Pair.Network->Advance(2us);
		Pair.Network->Pump();
		auto Newer = Message(Pair.ClientConnection, DeliveryMode::UnreliableSequenced, 100, Pair.Limits,
			RealtimeStateOrder{StateChannelId(9), RealtimeStateSequence(100)});
		Check(Newer && Pair.Client->Send(*Newer).Succeeded(),
			"a sequenced send remains best effort when the receive-event queue is full");
		(void)Pair.Network->Advance(1us);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)) == std::vector<unsigned int>({1, 2}),
			"the newer sequenced value can be dropped by local receive-event congestion");
		auto Older = Message(Pair.ClientConnection, DeliveryMode::UnreliableSequenced, 99, Pair.Limits,
			RealtimeStateOrder{StateChannelId(9), RealtimeStateSequence(99)});
		Check(Older && Pair.Client->Send(*Older).Succeeded(),
			"an older sequence can still reach transport after receive capacity is released");
		(void)Pair.Network->Advance(1us);
		Pair.Network->Pump();
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Drain(Pair.Server).empty() && Statistics && Statistics->DroppedUnreliableMessages == 2,
			"transport-observed newer state prevents stale resurrection even when consumer admission dropped it");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.BandwidthBytesPerSecond = 100;
		auto Limits = TestLimits(100, 100, 400);
		auto Pair = StartPair(Configuration, Limits);
		auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, 1, Pair.Limits,
			std::monostate{}, 100);
		Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "bandwidth test message is queued");
		(void)Pair.Network->Advance(999ms);
		Pair.Network->Pump();
		Check(Drain(Pair.Server).empty(), "bandwidth serialization delays delivery predictably");
		(void)Pair.Network->Advance(1ms);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)).size() == 1,
			"bandwidth budget releases the message at its deterministic completion time");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumReliableQueueBytes = 128;
		Configuration.BandwidthBytesPerSecond = 1;
		auto Limits = TestLimits(64, 32, 128);
		auto Pair = StartPair(Configuration, Limits);
		for (unsigned int Value = 1; Value <= 2; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, Value, Pair.Limits,
				std::monostate{}, 64);
			Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "reliable backlog fills to its exact ceiling");
		}
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Statistics && Statistics->QueuedReliableBytes == 128,
			"reliable queue accounting never exceeds its validated ceiling");
		auto Overflow = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, 3, Pair.Limits,
			std::monostate{}, 64);
		auto Result = Pair.Client->Send(*Overflow);
		Check(Result.Status == TransportOperationStatus::ResourceExhausted && Result.IsTerminal() &&
			Result.TerminalDisconnect->Reason == DisconnectReason::ResourceExhaustion,
			"reliable overflow has a deterministic structured terminal outcome");
		Pair.Network->Pump();
		Check(CountDisconnects(Drain(Pair.Client)) == 1 && CountDisconnects(Drain(Pair.Server)) == 1,
			"resource closure emits exactly one terminal disconnect per endpoint");
		(void)Pair.Network->Advance(MaximumSimulatedDuration);
		Pair.Network->Pump();
		Check(Drain(Pair.Server).empty(), "pending reliable events cannot resurrect a closed connection");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumQueuedUnreliableMessages = 2;
		Configuration.BandwidthBytesPerSecond = 1;
		auto Pair = StartPair(Configuration);
		for (unsigned int Value = 1; Value <= 3; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, Value, Pair.Limits);
			Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "unreliable congestion remains best effort");
		}
		auto Statistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Statistics && Statistics->DroppedUnreliableMessages == 1,
			"unreliable congestion drops work instead of growing the queue");
		(void)Pair.Network->Advance(3s);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)).size() == 2,
			"unreliable congestion creates no retry backlog");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumUnreliableDatagramBytes = 32;
		auto Limits = TestLimits(64, 64, 256);
		auto Pair = StartPair(Configuration, Limits);
		auto Oversized = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered, 1, Pair.Limits,
			std::monostate{}, 33);
		Check(Oversized && Pair.Client->Send(*Oversized).Status == TransportOperationStatus::MessageRejected &&
			Pair.Client->GetAvailableDatagramBytes(Pair.ClientConnection) == 32,
			"oversized unreliable messages fail before queueing and are never fragmented");
	}

	{
		auto Pair = StartPair();
		auto FirstId = Pair.ServerConnection;
		auto PendingOldGeneration = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, 13, Pair.Limits);
		Check(PendingOldGeneration && Pair.Client->Send(*PendingOldGeneration).Succeeded(),
			"the old generation can have delayed work pending before closure");
		Check(Pair.Server->Disconnect(FirstId, {DisconnectReason::LocalShutdown, "test close"}).Succeeded() &&
			Pair.Server->Disconnect(FirstId, {DisconnectReason::LocalShutdown, "duplicate close"}).Succeeded(),
			"duplicate close requests are idempotent while closure is pending");
		Pair.Network->Pump();
		auto ClientClose = Drain(Pair.Client);
		Check(CountDisconnects(Drain(Pair.Server)) == 1 && CountDisconnects(ClientClose) == 1,
			"explicit close is terminal exactly once");
		bool SawRemoteShutdown = false;
		for (const auto &Event : ClientClose)
			if (const auto *Closed = std::get_if<DisconnectedEvent>(&Event))
				SawRemoteShutdown = Closed->Information.Reason == DisconnectReason::RemoteShutdown;
		Check(SawRemoteShutdown, "the peer observes explicit local close as remote shutdown");

		auto ReplacementClient = Pair.Network->CreateTransport();
		TransportStartConfiguration Start{
			.Role = TransportRole::Client,
			.Endpoint = {"test", 1},
			.AdvertisedLimits = Pair.Limits,
		};
		Check(ReplacementClient->Start(Start).Succeeded(), "a new client can reconnect after terminal close");
		Pair.Network->Pump();
		auto ReplacementClientId = ConnectedId(Drain(ReplacementClient));
		auto ReplacementServerId = ConnectedId(Drain(Pair.Server));
		Check(ReplacementServerId.Slot == FirstId.Slot && ReplacementServerId.Generation != FirstId.Generation,
			"reused simulator slots advance generation");
		Check(Pair.Server->Disconnect(FirstId, {DisconnectReason::LocalShutdown, {}}).Status ==
			TransportOperationStatus::InvalidConnection,
			"a stale ConnectionId cannot affect its newer slot generation");
		auto Intent = Message(ReplacementClientId, DeliveryMode::ReliableOrdered, 42, Pair.Limits);
		Check(Intent && ReplacementClient->Send(*Intent).Succeeded(), "the replacement generation remains healthy");
		(void)Pair.Network->Advance(1ms);
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)) == std::vector<unsigned int>({42}),
			"cancelled old-generation work cannot leak into a reused connection slot");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumConnections = 3;
		Configuration.MaximumPendingEventsPerTransport = 12;
		Configuration.MaximumScheduledEvents = 16;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Network = SimulatedNetwork::Create(Configuration);
		auto Server = Network->CreateTransport();
		auto Limits = TestLimits();
		Check(Server->Start({TransportRole::Server, {"activation-reserve", 1}, Limits, {}}).Succeeded(),
			"event-reservation test server starts");
		for (unsigned int Cycle = 0; Cycle < 4; ++Cycle) {
			auto Client = Network->CreateTransport();
			Check(Client->Start({TransportRole::Client, {"activation-reserve", 1}, Limits, {}}).Succeeded(),
				"terminal history setup connection starts");
			Network->Pump();
			auto ClientConnection = ConnectedId(Drain(Client));
			Check(ClientConnection.IsValid() &&
				Client->Disconnect(ClientConnection, {DisconnectReason::LocalShutdown, "reserve setup"}).Succeeded(),
				"terminal history setup connection closes");
			Network->Pump();
			(void)Drain(Client);
		}
		auto FirstPending = Network->CreateTransport();
		auto SecondPending = Network->CreateTransport();
		Check(FirstPending->Start({TransportRole::Client, {"activation-reserve", 1}, Limits, {}}).Succeeded() &&
			SecondPending->Start({TransportRole::Client, {"activation-reserve", 1}, Limits, {}}).Status ==
				TransportOperationStatus::ResourceExhausted,
			"pending activation lifecycle events are reserved before accepting another connection");
		Network->Pump();
		auto FirstConnection = ConnectedId(Drain(FirstPending));
		Check(FirstConnection.IsValid() &&
			FirstPending->Disconnect(FirstConnection, {DisconnectReason::LocalShutdown, "reserved close"}).Succeeded(),
			"the admitted pending connection remains closable");
		Network->Pump();
		auto ServerEvents = Drain(Server);
		Check(ServerEvents.size() == 10 && CountDisconnects(ServerEvents) == 5,
			"bounded saturation retains every terminal event without exceeding configured capacity");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.ConnectionLifetime = 50ms;
		auto Pair = StartPair(Configuration);
		(void)Pair.Network->Advance(49ms);
		Pair.Network->Pump();
		Check(Drain(Pair.Client).empty(), "configured timeout does not fire early");
		(void)Pair.Network->Advance(1ms);
		Pair.Network->Pump();
		auto ClientEvents = Drain(Pair.Client);
		Check(CountDisconnects(ClientEvents) == 1, "configured timeout produces one terminal disconnect");
		bool SawTimeout = false;
		for (const auto &Event : ClientEvents)
			if (const auto *Closed = std::get_if<DisconnectedEvent>(&Event))
				SawTimeout = Closed->Information.Reason == DisconnectReason::Timeout;
		Check(SawTimeout, "timeout retains its structured disconnect reason");
		(void)Pair.Network->Advance(1s);
		Pair.Network->Pump();
		Check(Drain(Pair.Client).empty(), "closed timeout connections emit no later events");
	}

	{
		auto Pair = StartPair();
		Check(Pair.Client->ScheduleDisconnect(Pair.ClientConnection, 25ms,
			{DisconnectReason::TransportFailure, "injected failure"}).Succeeded(),
			"a simulator-specific transport failure can be scheduled explicitly");
		(void)Pair.Network->Advance(24ms);
		Pair.Network->Pump();
		Check(Drain(Pair.Client).empty(), "scheduled transport failure does not fire early");
		(void)Pair.Network->Advance(1ms);
		Pair.Network->Pump();
		auto Events = Drain(Pair.Client);
		bool SawFailure = false;
		for (const auto &Event : Events)
			if (const auto *Closed = std::get_if<DisconnectedEvent>(&Event))
				SawFailure = Closed->Information.Reason == DisconnectReason::TransportFailure;
		Check(CountDisconnects(Events) == 1 && SawFailure,
			"scheduled failure produces one structured terminal outcome");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Network = SimulatedNetwork::Create(Configuration);
		auto Server = Network->CreateTransport();
		auto ClientA = Network->CreateTransport();
		auto ClientB = Network->CreateTransport();
		auto Limits = TestLimits();
		Server->Start({TransportRole::Server, {"multi", 1}, Limits, {}});
		ClientA->Start({TransportRole::Client, {"multi", 1}, Limits, {}});
		ClientB->Start({TransportRole::Client, {"multi", 1}, Limits, {}});
		Network->Pump();
		auto ServerEvents = Drain(Server);
		auto ClientAId = ConnectedId(Drain(ClientA));
		auto ClientBId = ConnectedId(Drain(ClientB));
		Check(ServerEvents.size() == 4 && ClientAId.IsValid() && ClientBId.IsValid(),
			"one symbolic server endpoint accepts multiple clients");
		auto A = Message(ClientAId, DeliveryMode::UnreliableUnordered, 1, Limits);
		auto B = Message(ClientBId, DeliveryMode::UnreliableUnordered, 2, Limits);
		ClientA->Send(*A);
		ClientB->Send(*B);
		(void)Network->Advance(1us);
		Network->Pump();
		Check(PayloadValues(Drain(Server)) == std::vector<unsigned int>({1, 2}),
			"equal-time deliveries use deterministic global insertion order");
	}

	{
		auto OneStep = RunDeterministicTrace(0x12345678, {100ms});
		auto TenSteps = RunDeterministicTrace(0x12345678,
			{10ms, 10ms, 10ms, 10ms, 10ms, 10ms, 10ms, 10ms, 10ms, 10ms});
		auto Repeated = RunDeterministicTrace(0x12345678, {100ms});
		auto DifferentSeed = RunDeterministicTrace(0x87654321, {100ms});
		Check(OneStep == TenSteps && OneStep == Repeated,
			"same seed and operations reproduce the same trace across clock chunk sizes");
		Check(OneStep != DifferentSeed,
			"different seeds produce different fault traces without changing the contracts");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.MaximumConnections = 8;
		Configuration.MaximumPendingEventsPerTransport = 8192;
		Configuration.MaximumScheduledEvents = 8192;
		Configuration.MaximumReliableQueueBytes = 16 * 1024;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Limits = TestLimits(1, 1, 16 * 1024);
		auto Pair = StartPair(Configuration, Limits);
		constexpr unsigned int MessageCount = 5000;
		bool Accepted = true;
		for (unsigned int Value = 0; Value < MessageCount; ++Value) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered, Value & 0xff, Pair.Limits);
			Accepted = Accepted && Intent && Pair.Client->Send(*Intent).Succeeded();
		}
		Check(Accepted, "large bounded reliable stress workload remains accepted within configured ceilings");
		auto Before = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(Before && Before->QueuedReliableBytes == MessageCount,
			"stress backlog accounting is exact and bounded");
		(void)Pair.Network->Advance(std::chrono::microseconds(MessageCount));
		Pair.Network->Pump();
		Check(PayloadValues(Drain(Pair.Server)).size() == MessageCount,
			"large bounded stress workload drains deterministically without loss");
		auto After = Pair.Client->GetStatistics(Pair.ClientConnection);
		Check(After && After->QueuedReliableBytes == 0 && After->MessagesDelivered == MessageCount,
			"stress queue returns to zero with exact delivery statistics");
	}

	if (Failures == 0) std::cout << "All simulated transport tests passed\n";
	return Failures == 0 ? 0 : 1;
}

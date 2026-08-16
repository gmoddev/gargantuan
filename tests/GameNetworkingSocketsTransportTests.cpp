#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <variant>
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

	NetworkLimits TestLimits() {
		return {
			.MaximumReliableMessageBytes = 512 * 1024,
			.MaximumUnreliableMessageBytes = 2048,
			.MaximumQueuedReliableBytes = 4 * 1024 * 1024,
			.MaximumInFlightRemoteRequests = 64,
			.MaximumDecodedMessageBytes = 512 * 1024,
			.MaximumSendBytesPerTick = 512 * 1024,
			.MaximumReceiveBytesPerTick = 2 * 1024 * 1024,
			.MaximumMessagesPerTick = 4096,
		};
	}

	std::vector<TransportEvent> Drain(GameNetworkingSocketsTransport &Transport) {
		std::vector<TransportEvent> Result;
		std::array<TransportEvent, 256> Buffer;
		for (;;) {
			const auto Count = Transport.PollEvents(Buffer);
			if (Count == 0) break;
			for (std::size_t Index = 0; Index < Count; ++Index)
				Result.push_back(std::move(Buffer[Index]));
		}
		return Result;
	}

	void Pump(
		GameNetworkingSocketsTransport &First,
		GameNetworkingSocketsTransport &Second,
		std::vector<TransportEvent> &FirstEvents,
		std::vector<TransportEvent> &SecondEvents,
		std::chrono::milliseconds Timeout,
		const auto &Done
	) {
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline && !Done()) {
			auto NewFirst = Drain(First);
			auto NewSecond = Drain(Second);
			FirstEvents.insert(FirstEvents.end(),
				std::make_move_iterator(NewFirst.begin()), std::make_move_iterator(NewFirst.end()));
			SecondEvents.insert(SecondEvents.end(),
				std::make_move_iterator(NewSecond.begin()), std::make_move_iterator(NewSecond.end()));
			std::this_thread::sleep_for(1ms);
		}
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events) {
			const auto *State = std::get_if<ConnectionStateEvent>(&Event);
			if (State && State->Current == ConnectionState::Connected) return State->Connection;
		}
		return {};
	}

	bool HasDisconnect(const std::vector<TransportEvent> &Events, DisconnectReason Reason) {
		return std::ranges::any_of(Events, [&](const TransportEvent &Event) {
			const auto *Disconnected = std::get_if<DisconnectedEvent>(&Event);
			return Disconnected && Disconnected->Information.Reason == Reason;
		});
	}

	std::vector<std::vector<std::byte>> Payloads(const std::vector<TransportEvent> &Events) {
		std::vector<std::vector<std::byte>> Result;
		for (const auto &Event : Events) {
			if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Event))
				Result.push_back(Message->Payload);
		}
		return Result;
	}

	std::optional<NetworkMessageIntent> Message(
		ConnectionId Destination,
		DeliveryMode Delivery,
		std::vector<std::byte> Payload,
		const NetworkLimits &Limits,
		MessageOrder Order = std::monostate{}
	) {
		return MakeNetworkMessageIntent(
			Destination,
			Delivery,
			Delivery == DeliveryMode::ReliableOrdered
				? TrafficClass::ReliableApplication : TrafficClass::EphemeralApplication,
			std::move(Order),
			std::move(Payload),
			Limits
		);
	}

	struct PairFixture {
		std::unique_ptr<GameNetworkingSocketsTransport> Server;
		std::unique_ptr<GameNetworkingSocketsTransport> Client;
		NetworkLimits Limits;
		std::uint16_t Port = 0;
		ConnectionId ServerConnection;
		ConnectionId ClientConnection;
		std::vector<TransportEvent> ServerEvents;
		std::vector<TransportEvent> ClientEvents;
	};

	PairFixture StartPair(
		GameNetworkingSocketsTransportConfiguration ServerConfiguration = {},
		NetworkLimits Limits = TestLimits()
	) {
		PairFixture Result;
		Result.Limits = Limits;
		Result.Server = std::make_unique<GameNetworkingSocketsTransport>(ServerConfiguration);
		for (std::uint32_t Candidate = 39000; Candidate < 39100; ++Candidate) {
			TransportStartConfiguration Start{
				.Role = TransportRole::Server,
				.Endpoint = {"127.0.0.1", static_cast<std::uint16_t>(Candidate)},
				.AdvertisedLimits = Limits,
			};
			if (Result.Server->Start(Start).Succeeded()) {
				Result.Port = static_cast<std::uint16_t>(Candidate);
				break;
			}
		}
		if (Result.Port == 0) return Result;
		Result.Client = std::make_unique<GameNetworkingSocketsTransport>();
		TransportStartConfiguration ClientStart{
			.Role = TransportRole::Client,
			.Endpoint = {"127.0.0.1", Result.Port},
			.AdvertisedLimits = Limits,
		};
		if (!Result.Client->Start(ClientStart).Succeeded()) return Result;
		Pump(
			*Result.Server,
			*Result.Client,
			Result.ServerEvents,
			Result.ClientEvents,
			5s,
			[&] {
				Result.ServerConnection = ConnectedId(Result.ServerEvents);
				Result.ClientConnection = ConnectedId(Result.ClientEvents);
				return Result.ServerConnection.IsValid() && Result.ClientConnection.IsValid();
			}
		);
		return Result;
	}

	void StopPair(PairFixture &Pair) {
		if (Pair.Client)
			(void)Pair.Client->Stop({DisconnectReason::LocalShutdown, "Test client shutdown"});
		if (Pair.Server)
			(void)Pair.Server->Stop({DisconnectReason::LocalShutdown, "Test server shutdown"});
	}
}

int main() {
	using namespace gargantuan::network;
	try { gargantuan::BootstrapNativeRuntimeSchema(); }
	catch (const std::exception &Error) { std::cerr << Error.what() << '\n'; return 1; }
	std::cout << "[Networking:GNS] validating configuration\n" << std::flush;

	{
		GameNetworkingSocketsTransportConfiguration Configuration;
		Check(Configuration.IsValid(), "default GNS adapter configuration is valid");
		Configuration.MaximumConnections = 0;
		Check(!Configuration.IsValid(), "zero GNS connection capacity is rejected");
		Configuration = {};
		Configuration.MaximumPendingEvents = Configuration.MaximumConnections * 5 - 1;
		Check(!Configuration.IsValid(), "GNS event capacity reserves terminal lifecycle events");
		Configuration = {};
		Configuration.MaximumPendingReceiveBytes = NativeMaximumGnsPendingReceiveBytes + 1;
		Check(!Configuration.IsValid(), "GNS receive storage above the native ceiling is rejected");

		GameNetworkingSocketsTransport Transport;
		TransportStartConfiguration Invalid{
			.Role = TransportRole::Client,
			.Endpoint = {"not a numeric address", 39000},
			.AdvertisedLimits = TestLimits(),
		};
		Check(Transport.Start(Invalid).Status == TransportOperationStatus::MessageRejected,
			"non-numeric real transport endpoints fail before partial activation");
		Invalid.Endpoint = {"127.0.0.1", 39000};
		Invalid.OpaqueHandshakeMaterial = {std::byte{0x01}};
		Check(Transport.Start(Invalid).Status == TransportOperationStatus::MessageRejected,
			"unimplemented handshake material is rejected rather than treated as authority or payload");
		Check(!Message(ConnectionId{1, 1}, DeliveryMode::ReliableOrdered, {}, TestLimits()),
			"zero-length application payloads are rejected by the backend-neutral intent boundary");
	}

	std::cout << "[Networking:GNS] connecting localhost pair\n" << std::flush;
	auto Pair = StartPair();
	Check(Pair.ServerConnection.IsValid() && Pair.ClientConnection.IsValid(),
		"real localhost server and client reach connected state");
	if (!Pair.ServerConnection.IsValid() || !Pair.ClientConnection.IsValid()) {
		StopPair(Pair);
		return 1;
	}
	for (const auto &Event : Pair.ServerEvents) Check(IsValidTransportEvent(Event, Pair.Limits),
		"server lifecycle events satisfy the backend-neutral contract");
	for (const auto &Event : Pair.ClientEvents) Check(IsValidTransportEvent(Event, Pair.Limits),
		"client lifecycle events satisfy the backend-neutral contract");
	Pair.ServerEvents.clear();
	Pair.ClientEvents.clear();

	{
		std::cout << "[Networking:GNS] reliable delivery\n" << std::flush;
		std::vector<std::vector<std::byte>> ExpectedClientToServer;
		std::vector<std::vector<std::byte>> ExpectedServerToClient;
		for (std::uint8_t Value = 1; Value <= 32; ++Value) {
			ExpectedClientToServer.push_back({static_cast<std::byte>(Value)});
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered,
				ExpectedClientToServer.back(), Pair.Limits);
			Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "client accepts bounded reliable message");
			ExpectedServerToClient.push_back({static_cast<std::byte>(Value), static_cast<std::byte>(Value)});
			Intent = Message(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
				ExpectedServerToClient.back(), Pair.Limits);
			Check(Intent && Pair.Server->Send(*Intent).Succeeded(), "server accepts bounded reliable message");
		}
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
			return Payloads(Pair.ServerEvents).size() >= ExpectedClientToServer.size() &&
				Payloads(Pair.ClientEvents).size() >= ExpectedServerToClient.size();
		});
		Check(Payloads(Pair.ServerEvents) == ExpectedClientToServer,
			"reliable client messages preserve order and boundaries over localhost");
		Check(Payloads(Pair.ClientEvents) == ExpectedServerToClient,
			"reliable server messages preserve order and boundaries over localhost");
		Pair.ServerEvents.clear();
		Pair.ClientEvents.clear();
	}

	{
		std::cout << "[Networking:GNS] unreliable delivery and limits\n" << std::flush;
		auto ClientDatagram = Pair.Client->GetAvailableDatagramBytes(Pair.ClientConnection);
		auto ServerDatagram = Pair.Server->GetAvailableDatagramBytes(Pair.ServerConnection);
		Check(ClientDatagram && ServerDatagram && *ClientDatagram == *ServerDatagram && *ClientDatagram > 0,
			"real adapter reports a bounded backend-neutral datagram payload ceiling");
		if (ClientDatagram) {
			auto AtLimit = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered,
				std::vector<std::byte>(*ClientDatagram, std::byte{0x31}), Pair.Limits);
			Check(AtLimit && Pair.Client->Send(*AtLimit).Succeeded(),
				"unreliable payload exactly at adapter datagram bound is accepted");
			auto AboveLimit = Message(Pair.ClientConnection, DeliveryMode::UnreliableUnordered,
				std::vector<std::byte>(*ClientDatagram + 1, std::byte{0x32}), Pair.Limits);
			Check(AboveLimit && Pair.Client->Send(*AboveLimit).Status == TransportOperationStatus::MessageRejected,
				"unreliable payload one byte above adapter datagram bound is rejected before GNS");
		}
		auto Reverse = Message(Pair.ServerConnection, DeliveryMode::UnreliableSequenced,
			{std::byte{0x42}}, Pair.Limits,
			RealtimeStateOrder{StateChannelId(3), RealtimeStateSequence(9)});
		Check(Reverse && Pair.Server->Send(*Reverse).Succeeded(),
			"GNS carries sequenced intent metadata without owning stale rejection");
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
			return !Payloads(Pair.ServerEvents).empty() && !Payloads(Pair.ClientEvents).empty();
		});
		Check(!Payloads(Pair.ServerEvents).empty() && !Payloads(Pair.ClientEvents).empty(),
			"bidirectional unreliable message boundaries survive localhost delivery");
		Pair.ServerEvents.clear();
		Pair.ClientEvents.clear();
	}

	{
		std::cout << "[Networking:GNS] reliable backend bounds\n" << std::flush;
		constexpr std::size_t MaximumBackendReliablePayload = 512 * 1024 - 24;
		auto AtLimit = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered,
			std::vector<std::byte>(MaximumBackendReliablePayload, std::byte{0x5a}), Pair.Limits);
		Check(AtLimit && Pair.Client->Send(*AtLimit).Succeeded(),
			"reliable payload exactly at the GNS frame ceiling is accepted");
		auto AboveLimit = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered,
			std::vector<std::byte>(MaximumBackendReliablePayload + 1, std::byte{0x5b}), Pair.Limits);
		Check(AboveLimit && Pair.Client->Send(*AboveLimit).Status == TransportOperationStatus::MessageRejected,
			"reliable payload one byte above the GNS frame ceiling is rejected before GNS");
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
			return Payloads(Pair.ServerEvents).size() == 1;
		});
		Check(Payloads(Pair.ServerEvents).size() == 1 &&
			Payloads(Pair.ServerEvents).front().size() == MaximumBackendReliablePayload,
			"maximum accepted reliable payload retains its message boundary");
		Pair.ServerEvents.clear();
		Pair.ClientEvents.clear();
	}

	{
		std::cout << "[Networking:GNS] bounded reliable stress\n" << std::flush;
		constexpr std::uint32_t StressMessages = 1000;
		for (std::uint32_t Index = 0; Index < StressMessages; ++Index) {
			auto Intent = Message(Pair.ClientConnection, DeliveryMode::ReliableOrdered,
				{static_cast<std::byte>(Index & 0xff)}, Pair.Limits);
			Check(Intent && Pair.Client->Send(*Intent).Succeeded(), "bounded reliable stress send is accepted");
		}
		Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 10s, [&] {
			return Payloads(Pair.ServerEvents).size() >= StressMessages;
		});
		const auto Received = Payloads(Pair.ServerEvents);
		Check(Received.size() == StressMessages, "bounded localhost stress delivers every reliable message");
		bool Ordered = Received.size() == StressMessages;
		for (std::uint32_t Index = 0; Ordered && Index < StressMessages; ++Index)
			Ordered = Received[Index] == std::vector<std::byte>{static_cast<std::byte>(Index & 0xff)};
		Check(Ordered, "bounded localhost reliable stress remains ordered");
		auto ClientStatistics = Pair.Client->GetStatistics(Pair.ClientConnection);
		auto ServerStatistics = Pair.Server->GetStatistics(Pair.ServerConnection);
		Check(ClientStatistics && ClientStatistics->MessagesSent &&
			*ClientStatistics->MessagesSent >= StressMessages + 33,
			"adapter statistics expose cumulative accepted application sends");
		Check(ServerStatistics && ServerStatistics->MessagesReceived &&
			*ServerStatistics->MessagesReceived >= StressMessages + 33,
			"adapter statistics expose cumulative polled application receives");
		Check(ClientStatistics && !ClientStatistics->MessagesDelivered &&
			!ClientStatistics->DroppedUnreliableMessages,
			"unsupported GNS delivery and drop metrics remain unavailable rather than fabricated");
		Pair.ServerEvents.clear();
		Pair.ClientEvents.clear();
	}

	{
		std::cout << "[Networking:GNS] real basic client replication\n" << std::flush;
		auto Game = std::make_shared<gargantuan::DataModel>();
		Game->SetName("GnsServerWorld");
		auto Child = std::make_shared<gargantuan::Folder>();
		Child->SetName("GnsReplicatedFolder");
		Child->SetParent(Game);
		gargantuan::network::ReplicationCoordinator Coordinator(Game);
		auto Baseline = Coordinator.AddPeer(Pair.ServerConnection, ReplicationEpoch(1));
		NetworkScheduler Scheduler(*Pair.Server);
		Check(Baseline.Succeeded() && Scheduler.RegisterConnection(Pair.ServerConnection, Pair.Limits),
			"real replication baseline and scheduler connection initialize");
		if (Baseline.Succeeded()) {
			auto Queued = QueueReplicationFrame(*Baseline.Frame, Pair.ServerConnection, Pair.Limits, Scheduler);
			auto Flushed = Scheduler.Flush(Pair.ServerConnection, SchedulerTickBudget::FromNetworkLimits(Pair.Limits));
			Check(Queued && Queued->Accepted() && Flushed.Status == SchedulerFlushStatus::Drained,
				"real replication baseline is admitted and submitted through NetworkScheduler");
			Pair.ServerEvents.clear();
			Pair.ClientEvents.clear();
			Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
				return std::ranges::any_of(Pair.ClientEvents, [](const TransportEvent &Event) {
					const auto *MessageValue = std::get_if<ReceivedMessageEvent>(&Event);
					return MessageValue && MessageValue->Traffic == TrafficClass::StructuralReplication;
				});
			});
			ReplicaApplier ClientReplica;
			bool Applied = false;
			for (const auto &Event : Pair.ClientEvents)
				if (const auto *MessageValue = std::get_if<ReceivedMessageEvent>(&Event);
					MessageValue && MessageValue->Traffic == TrafficClass::StructuralReplication)
					Applied = ClientReplica.ApplyBytes(MessageValue->Payload).Succeeded();
			Check(Applied && ClientReplica.GetReplicaRoot() &&
				ClientReplica.GetReplicaRoot()->FindFirstChild("GnsReplicatedFolder", false),
				"independent real localhost client materializes the authoritative server baseline");
		}
		Pair.ServerEvents.clear();
		Pair.ClientEvents.clear();
	}

	{
		std::cout << "[Networking:GNS] listener failure\n" << std::flush;
		GameNetworkingSocketsTransport DuplicateListener;
		TransportStartConfiguration Start{
			.Role = TransportRole::Server,
			.Endpoint = {"127.0.0.1", Pair.Port},
			.AdvertisedLimits = Pair.Limits,
		};
		const auto Result = DuplicateListener.Start(Start);
		Check(Result.Status == TransportOperationStatus::TransportFailure && Result.IsTerminal(),
			"listener creation failure is structured and leaves no partial adapter activation");
	}

	std::cout << "[Networking:GNS] close and identity reuse\n" << std::flush;
	const auto StaleServerConnection = Pair.ServerConnection;
	Check(Pair.Client->Disconnect(
		Pair.ClientConnection,
		{DisconnectReason::LocalShutdown, "Client requested close"}
	).Succeeded(), "explicit local close succeeds");
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
		return HasDisconnect(Pair.ServerEvents, DisconnectReason::RemoteShutdown);
	});
	Check(HasDisconnect(Pair.ServerEvents, DisconnectReason::RemoteShutdown),
		"server observes one structured remote shutdown");
	Check(HasDisconnect(Pair.ClientEvents, DisconnectReason::LocalShutdown),
		"client observes its structured local shutdown");
	Check(Pair.Client->Disconnect(
		Pair.ClientConnection,
		{DisconnectReason::LocalShutdown, "Duplicate close"}
	).Status == TransportOperationStatus::InvalidConnection, "closed identity cannot close or affect a connection twice");
	Check(Pair.Client->Stop({DisconnectReason::LocalShutdown, "First client stopped"}).Succeeded(),
		"client stop after explicit close succeeds");
	Check(Pair.Client->Stop({DisconnectReason::LocalShutdown, "Duplicate stop"}).Status ==
		TransportOperationStatus::InvalidState, "repeated transport shutdown is safe and explicit");

	Pair.Client = std::make_unique<GameNetworkingSocketsTransport>();
	TransportStartConfiguration Restart{
		.Role = TransportRole::Client,
		.Endpoint = {"127.0.0.1", Pair.Port},
		.AdvertisedLimits = Pair.Limits,
	};
	Check(Pair.Client->Start(Restart).Succeeded(), "a new client can reconnect to the existing listener");
	Pair.ServerEvents.clear();
	Pair.ClientEvents.clear();
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
		Pair.ServerConnection = ConnectedId(Pair.ServerEvents);
		Pair.ClientConnection = ConnectedId(Pair.ClientEvents);
		return Pair.ServerConnection.IsValid() && Pair.ClientConnection.IsValid();
	});
	Check(Pair.ServerConnection.IsValid() && Pair.ServerConnection != StaleServerConnection,
		"reconnected peer receives a generation-safe Gargantuan identity");
	auto StaleIntent = Message(StaleServerConnection, DeliveryMode::ReliableOrdered,
		{std::byte{0x61}}, Pair.Limits);
	Check(StaleIntent && Pair.Server->Send(*StaleIntent).Status == TransportOperationStatus::InvalidConnection,
		"stale identity cannot address a reused connection slot or backend handle");
	auto CurrentIntent = Message(Pair.ServerConnection, DeliveryMode::ReliableOrdered,
		{std::byte{0x62}}, Pair.Limits);
	Check(CurrentIntent && Pair.Server->Send(*CurrentIntent).Succeeded(),
		"new generation remains usable after stale identity rejection");

	Check(Pair.Server->Disconnect(
		Pair.ServerConnection,
		{DisconnectReason::LocalShutdown, "Server requested close"}
	).Succeeded(), "server local close succeeds");
	Pair.ServerEvents.clear();
	Pair.ClientEvents.clear();
	Pump(*Pair.Server, *Pair.Client, Pair.ServerEvents, Pair.ClientEvents, 5s, [&] {
		return HasDisconnect(Pair.ClientEvents, DisconnectReason::RemoteShutdown);
	});
	Check(HasDisconnect(Pair.ClientEvents, DisconnectReason::RemoteShutdown),
		"client observes server close as a structured remote shutdown");
	StopPair(Pair);

	{
		std::cout << "[Networking:GNS] receive queue exhaustion\n" << std::flush;
		GameNetworkingSocketsTransportConfiguration Bounded;
		Bounded.MaximumPendingReceiveBytes = 32;
		auto BoundedPair = StartPair(Bounded);
		Check(BoundedPair.ServerConnection.IsValid(), "bounded receive-queue fixture connects");
		if (BoundedPair.ServerConnection.IsValid()) {
			auto Intent = Message(BoundedPair.ClientConnection, DeliveryMode::ReliableOrdered,
				std::vector<std::byte>(33, std::byte{0x7f}), BoundedPair.Limits);
			Check(Intent && BoundedPair.Client->Send(*Intent).Succeeded(),
				"peer may submit a message larger than the receiver adapter queue budget");
			BoundedPair.ServerEvents.clear();
			BoundedPair.ClientEvents.clear();
			Pump(*BoundedPair.Server, *BoundedPair.Client,
				BoundedPair.ServerEvents, BoundedPair.ClientEvents, 5s, [&] {
					return HasDisconnect(BoundedPair.ServerEvents, DisconnectReason::ResourceExhaustion);
				});
			Check(HasDisconnect(BoundedPair.ServerEvents, DisconnectReason::ResourceExhaustion),
				"receive queue exhaustion terminates fail-closed with a structured reason");
			Check(Payloads(BoundedPair.ServerEvents).empty(),
				"message rejected by receive queue accounting is never exposed upward");
		}
		StopPair(BoundedPair);
	}

	{
		std::cout << "[Networking:GNS] failed connect cleanup\n" << std::flush;
		GameNetworkingSocketsTransport FailedClient;
		TransportStartConfiguration Start{
			.Role = TransportRole::Client,
			.Endpoint = {"127.0.0.1", Pair.Port},
			.AdvertisedLimits = TestLimits(),
		};
		Check(FailedClient.Start(Start).Succeeded(), "asynchronous connect attempt initializes cleanly");
		std::vector<TransportEvent> Events;
		const auto Deadline = std::chrono::steady_clock::now() + 15s;
		while (std::chrono::steady_clock::now() < Deadline &&
			!std::ranges::any_of(Events, [](const TransportEvent &Event) {
				return std::holds_alternative<DisconnectedEvent>(Event);
			})) {
			auto NewEvents = Drain(FailedClient);
			Events.insert(Events.end(), std::make_move_iterator(NewEvents.begin()), std::make_move_iterator(NewEvents.end()));
			std::this_thread::sleep_for(2ms);
		}
		Check(std::ranges::any_of(Events, [](const TransportEvent &Event) {
			return std::holds_alternative<DisconnectedEvent>(Event);
		}), "failed localhost connect produces one terminal disconnect event");
		(void)FailedClient.Stop({DisconnectReason::LocalShutdown, "Failed client cleanup"});
	}

	{
		std::cout << "[Networking:GNS] active destruction cleanup\n" << std::flush;
		auto DestructionPair = StartPair();
		Check(DestructionPair.ServerConnection.IsValid() && DestructionPair.ClientConnection.IsValid(),
			"active-destruction fixture connects");
		if (DestructionPair.ServerConnection.IsValid() && DestructionPair.ClientConnection.IsValid()) {
			DestructionPair.ServerEvents.clear();
			DestructionPair.Client.reset();
			const auto Deadline = std::chrono::steady_clock::now() + 5s;
			while (std::chrono::steady_clock::now() < Deadline &&
				!HasDisconnect(DestructionPair.ServerEvents, DisconnectReason::RemoteShutdown)) {
				auto NewEvents = Drain(*DestructionPair.Server);
				DestructionPair.ServerEvents.insert(
					DestructionPair.ServerEvents.end(),
					std::make_move_iterator(NewEvents.begin()),
					std::make_move_iterator(NewEvents.end())
				);
				std::this_thread::sleep_for(1ms);
			}
			Check(HasDisconnect(DestructionPair.ServerEvents, DisconnectReason::RemoteShutdown),
				"destroying an active adapter closes the peer and cannot leave callback ownership live");
		}
		(void)DestructionPair.Server->Stop({DisconnectReason::LocalShutdown, "Destruction test cleanup"});
	}

	if (Failures != 0) {
		std::cerr << Failures << " GameNetworkingSockets transport test(s) failed\n";
		return 1;
	}
	std::cout << "GameNetworkingSockets transport tests passed\n";
	return 0;
}

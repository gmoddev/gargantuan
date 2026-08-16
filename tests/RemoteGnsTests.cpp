#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/RemoteManager.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	NetworkLimits Limits() {
		return {
			.MaximumReliableMessageBytes = MaximumRemoteFrameBytes,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 4 * 1024 * 1024,
			.MaximumInFlightRemoteRequests = 32,
			.MaximumDecodedMessageBytes = MaximumRemoteFrameBytes,
			.MaximumSendBytesPerTick = 1024 * 1024,
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

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events) {
			const auto *State = std::get_if<ConnectionStateEvent>(&Event);
			if (State && State->Current == ConnectionState::Connected) return State->Connection;
		}
		return {};
	}

	struct Fixture {
		GameNetworkingSocketsTransport ServerTransport;
		GameNetworkingSocketsTransport ClientTransport;
		NetworkLimits Negotiated = Limits();
		std::uint16_t Port = 0;
		ConnectionId ServerConnection;
		ConnectionId ClientConnection;
		std::unique_ptr<NetworkScheduler> ServerScheduler;
		std::unique_ptr<NetworkScheduler> ClientScheduler;
		std::unique_ptr<RemoteManager> Server;
		std::unique_ptr<RemoteManager> Client;

		Fixture() {
			for (std::uint32_t Candidate = 39100; Candidate < 39200; ++Candidate) {
				if (ServerTransport
						.Start(
							{TransportRole::Server,
							 {"127.0.0.1", static_cast<std::uint16_t>(Candidate)},
							 Negotiated,
							 {}}
						)
						.Succeeded()) {
					Port = static_cast<std::uint16_t>(Candidate);
					break;
				}
			}
			Check(Port != 0, "localhost GNS Remote server binds a bounded test port");
			if (Port == 0) return;
			Check(
				ClientTransport.Start({TransportRole::Client, {"127.0.0.1", Port}, Negotiated, {}}).Succeeded(),
				"localhost GNS Remote client starts"
			);
			Connect(ReplicationEpoch(1));
		}

		~Fixture() {
			(void)ClientTransport.Stop({DisconnectReason::LocalShutdown, "Remote GNS test shutdown"});
			(void)ServerTransport.Stop({DisconnectReason::LocalShutdown, "Remote GNS test shutdown"});
		}

		void Connect(ReplicationEpoch Epoch) {
			const auto Deadline = std::chrono::steady_clock::now() + 5s;
			while (std::chrono::steady_clock::now() < Deadline &&
				   (!ServerConnection.IsValid() || !ClientConnection.IsValid())) {
				auto ServerEvents = Drain(ServerTransport);
				auto ClientEvents = Drain(ClientTransport);
				if (!ServerConnection.IsValid()) ServerConnection = ConnectedId(ServerEvents);
				if (!ClientConnection.IsValid()) ClientConnection = ConnectedId(ClientEvents);
				std::this_thread::sleep_for(1ms);
			}
			Check(ServerConnection.IsValid() && ClientConnection.IsValid(), "localhost GNS Remote peers connect");
			if (!ServerConnection.IsValid() || !ClientConnection.IsValid()) return;
			ServerScheduler = std::make_unique<NetworkScheduler>(ServerTransport);
			ClientScheduler = std::make_unique<NetworkScheduler>(ClientTransport);
			Check(
				ServerScheduler->RegisterConnection(ServerConnection, Negotiated) &&
					ClientScheduler->RegisterConnection(ClientConnection, Negotiated),
				"localhost GNS Remote schedulers register their local ConnectionIds"
			);
			Server = std::make_unique<RemoteManager>(
				RemoteManagerRole::Server,
				*ServerScheduler,
				[](ConnectionId, ObjectId) { return true; },
				[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
			);
			Client = std::make_unique<RemoteManager>(
				RemoteManagerRole::Client,
				*ClientScheduler,
				[](ConnectionId, ObjectId) { return true; },
				[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
			);
			Check(
				Server->AddPeer(ServerConnection, Epoch, Negotiated) &&
					Client->AddPeer(ClientConnection, Epoch, Negotiated),
				"localhost GNS RemoteManagers attach the negotiated peer sessions"
			);
		}

		void Register(ObjectId Remote, RemoteInstanceKind Kind) {
			Check(
				Server->RegisterRemote(Remote, Kind) && Client->RegisterRemote(Remote, Kind),
				"localhost GNS peers register matching semantic Remote identity"
			);
			Check(
				Server->PublishRemote(ServerConnection, Remote) && Client->PublishRemote(ClientConnection, Remote),
				"localhost GNS peers publish matching Remote identity"
			);
		}

		bool PumpUntil(std::chrono::milliseconds Timeout, const auto &Done) {
			const auto Deadline = std::chrono::steady_clock::now() + Timeout;
			while (std::chrono::steady_clock::now() < Deadline && !Done()) {
				if (ServerScheduler)
					(void)ServerScheduler->Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Negotiated));
				if (ClientScheduler)
					(void)ClientScheduler->Flush(ClientConnection, SchedulerTickBudget::FromNetworkLimits(Negotiated));
				for (const auto &Event : Drain(ServerTransport))
					if (Server) (void)Server->HandleTransportEvent(Event);
				for (const auto &Event : Drain(ClientTransport))
					if (Client) (void)Client->HandleTransportEvent(Event);
				if (Server) Server->Pump();
				if (Client) Client->Pump();
				std::this_thread::sleep_for(1ms);
			}
			return Done();
		}
	};
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	Fixture Test;
	if (!Test.Server || !Test.Client) return 1;
	const ObjectId ClientToServer{11, 1};
	const ObjectId ServerToClient{12, 1};
	const ObjectId Unreliable{13, 1};
	const ObjectId Sequenced{14, 1};
	const ObjectId Function{15, 1};
	Test.Register(ClientToServer, RemoteInstanceKind::ReliableEvent);
	Test.Register(ServerToClient, RemoteInstanceKind::ReliableEvent);
	Test.Register(Unreliable, RemoteInstanceKind::UnreliableEvent);
	Test.Register(Sequenced, RemoteInstanceKind::UnreliableSequencedEvent);
	Test.Register(Function, RemoteInstanceKind::Function);

	int ServerReliable = 0;
	int ClientReliable = 0;
	int ServerUnreliable = 0;
	int ClientUnreliable = 0;
	int ServerSequence = -1;
	int ClientSequence = -1;
	Test.Server->SetEventHandler(ClientToServer, [&](const RemoteInvocation &Message) {
		ServerReliable = std::get<int>(Message.Arguments.front());
	});
	Test.Client->SetEventHandler(ServerToClient, [&](const RemoteInvocation &Message) {
		ClientReliable = std::get<int>(Message.Arguments.front());
	});
	Test.Server->SetEventHandler(Unreliable, [&](const RemoteInvocation &) { ++ServerUnreliable; });
	Test.Client->SetEventHandler(Unreliable, [&](const RemoteInvocation &) { ++ClientUnreliable; });
	Test.Server->SetEventHandler(Sequenced, [&](const RemoteInvocation &Message) {
		ServerSequence = std::get<int>(Message.Arguments.front());
	});
	Test.Client->SetEventHandler(Sequenced, [&](const RemoteInvocation &Message) {
		ClientSequence = std::get<int>(Message.Arguments.front());
	});
	Check(
		Test.Client->SendEvent(Test.ClientConnection, ClientToServer, {41}).Accepted() &&
			Test.Server->SendEvent(Test.ServerConnection, ServerToClient, {42}).Accepted(),
		"localhost GNS admits reliable RemoteEvents in both directions"
	);
	for (int Index = 0; Index < 10; ++Index) {
		(void)Test.Client->SendEvent(Test.ClientConnection, Unreliable, {Index});
		(void)Test.Server->SendEvent(Test.ServerConnection, Unreliable, {Index});
	}
	for (int Index = 0; Index < 20; ++Index) {
		(void)Test.Client->SendEvent(Test.ClientConnection, Sequenced, {Index});
		(void)Test.Server->SendEvent(Test.ServerConnection, Sequenced, {Index});
	}
	Check(
		Test.PumpUntil(
			5s,
			[&] {
				return ServerReliable == 41 && ClientReliable == 42 && ServerUnreliable != 0 && ClientUnreliable != 0 &&
					   ServerSequence == 19 && ClientSequence == 19;
			}
		),
		"localhost GNS proves reliable, unreliable, and newest-wins Remote delivery bidirectionally"
	);

	Test.Server->SetRequestHandler(Function, [](const RemoteInvocation &Message, RemoteManager::RequestReply Reply) {
		Reply({std::get<int>(Message.Arguments.front()) + 1}, std::nullopt);
	});
	Test.Client->SetRequestHandler(Function, [](const RemoteInvocation &Message, RemoteManager::RequestReply Reply) {
		Reply({std::get<int>(Message.Arguments.front()) + 2}, std::nullopt);
	});
	std::optional<RemoteRequestResult> ServerResult;
	std::optional<RemoteRequestResult> ClientResult;
	Check(
		Test.Client
			->StartRequest(
				Test.ClientConnection,
				Function,
				{10},
				[&](RemoteRequestResult Result) { ClientResult = std::move(Result); },
				2s
			)
			.Accepted(),
		"localhost client starts reliable RemoteFunction request"
	);
	Check(
		Test.Server
			->StartRequest(
				Test.ServerConnection,
				Function,
				{20},
				[&](RemoteRequestResult Result) { ServerResult = std::move(Result); },
				2s
			)
			.Accepted(),
		"localhost server starts reliable client RemoteFunction request"
	);
	Check(
		Test.PumpUntil(5s, [&] { return ClientResult && ServerResult; }) &&
			ClientResult->Outcome.Status == RemoteRequestTerminalStatus::Success &&
			ServerResult->Outcome.Status == RemoteRequestTerminalStatus::Success &&
			std::get<int>(ClientResult->Results.front()) == 11 && std::get<int>(ServerResult->Results.front()) == 22,
		"localhost GNS correlates bounded request/response in both directions"
	);

	int StressDelivered = 0;
	Test.Server->SetEventHandler(ClientToServer, [&](const RemoteInvocation &) { ++StressDelivered; });
	for (int Index = 0; Index < 200; ++Index)
		Check(
			Test.Client->SendEvent(Test.ClientConnection, ClientToServer, {Index}).Accepted(),
			"bounded localhost reliable stress remains admitted"
		);
	Check(
		Test.PumpUntil(5s, [&] { return StressDelivered == 200; }),
		"localhost GNS drains a bounded 200-event reliable workload"
	);

	RemoteManager::RequestReply HeldReply;
	Test.Server->SetRequestHandler(Function, [&](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
		HeldReply = std::move(Reply);
	});
	std::optional<RemoteRequestResult> Disconnected;
	Check(
		Test.Client
			->StartRequest(
				Test.ClientConnection,
				Function,
				{},
				[&](RemoteRequestResult Result) { Disconnected = std::move(Result); },
				5s
			)
			.Accepted(),
		"localhost pending-disconnect request starts"
	);
	Check(
		Test.PumpUntil(2s, [&] { return static_cast<bool>(HeldReply); }),
		"localhost server begins the pending request handler"
	);
	const auto OldClientConnection = Test.ClientConnection;
	Check(
		Test.ClientTransport
			.Disconnect(Test.ClientConnection, {DisconnectReason::LocalShutdown, "Remote pending-request disconnect"})
			.Succeeded(),
		"localhost client disconnect is initiated"
	);
	Check(
		Test.PumpUntil(5s, [&] { return Disconnected.has_value(); }) &&
			Disconnected->Outcome.Status == RemoteRequestTerminalStatus::Disconnected,
		"localhost disconnect terminates the pending RemoteFunction exactly once"
	);
	(void)Test.PumpUntil(100ms, [] { return false; });
	Check(!HeldReply({99}, std::nullopt), "late handler completion after disconnect cannot resurrect request state");

	Check(
		Test.ClientTransport.Stop({DisconnectReason::LocalShutdown, "Remote reconnect reset"}).Succeeded(),
		"localhost client transport stops for reconnect"
	);
	(void)Drain(Test.ClientTransport);
	Test.Client.reset();
	Test.ClientScheduler.reset();
	Test.ClientConnection = {};
	Check(
		Test.ClientTransport.Start({TransportRole::Client, {"127.0.0.1", Test.Port}, Test.Negotiated, {}}).Succeeded(),
		"localhost client transport restarts for a new session"
	);
	Test.ServerConnection = {};
	Test.Server.reset();
	Test.ServerScheduler.reset();
	Test.Connect(ReplicationEpoch(2));
	Check(
		Test.ClientConnection.IsValid() && Test.ClientConnection != OldClientConnection,
		"localhost reconnect allocates a stale-safe ConnectionId generation"
	);
	Test.Register(Function, RemoteInstanceKind::Function);
	Check(
		Test.Client->StartRequest(
					   Test.ClientConnection, Function, {}, [](RemoteRequestResult) {}, 1s
		)
			.Accepted(),
		"new epoch request begins independently after localhost reconnect"
	);
	RemoteMessage LateOld{
		.Kind = RemoteMessageKind::Response, .Remote = Function, .Request = RemoteRequestId(1), .Arguments = {999}
	};
	auto LateBytes = EncodeRemoteMessage(LateOld);
	Check(
		LateBytes && !Test.Client->HandleTransportEvent({ReceivedMessageEvent{
						 OldClientConnection,
						 DeliveryMode::ReliableOrdered,
						 TrafficClass::ReliableApplication,
						 {},
						 std::move(*LateBytes)
					 }}),
		"late old-session response cannot enter the reconnected RemoteManager"
	);

	if (Failures != 0) {
		std::cerr << Failures << " real GNS Remote test(s) failed\n";
		return 1;
	}
	std::cout
		<< "Localhost GNS bounded Remote bidirectional, request, stress, disconnect, and reconnect tests passed\n";
	return 0;
}

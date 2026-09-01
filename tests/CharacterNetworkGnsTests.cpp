#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <array>
#include <chrono>
#include <cmath>
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
			.MaximumReliableMessageBytes = 4096,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 1024 * 1024,
			.MaximumInFlightRemoteRequests = 16,
			.MaximumDecodedMessageBytes = 4096,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
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

	CharacterMotionRequest Movement(const CharacterInputCommand &Command, const KinematicCharacter &) {
		return {
			.Translation =
				{Command.MoveIntent.x * 4.0f * Command.DeltaSeconds,
				 0.0f,
				 Command.MoveIntent.y * 4.0f * Command.DeltaSeconds},
			.Velocity = {Command.MoveIntent.x * 4.0f, 0.0f, Command.MoveIntent.y * 4.0f}
		};
	}

	CharacterActionDefinition Action() {
		std::array<std::uint8_t, 4> RevisionBytes{3, 2, 1, 9};
		return {
			.Token = 1,
			.Animation = AssetId::FromBuiltInName("CharacterGnsLunge"),
			.ContentRevision = AssetContentId::Hash(RevisionBytes),
			.DurationTicks = 4,
			.EvaluateRootMotion = [](std::uint64_t From, std::uint64_t To) -> std::optional<RootMotionDelta> {
				return To > From ? std::optional(RootMotionDelta{.Translation = {0.1f, 0.0f, 0.0f}}) : std::nullopt;
			}
		};
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	BootstrapNativeRuntimeSchema();
	GameNetworkingSocketsTransport ServerTransport;
	GameNetworkingSocketsTransport ClientTransport;
	const auto Negotiated = Limits();
	std::uint16_t Port = 0;
	for (std::uint32_t Candidate = 39200; Candidate < 39300; ++Candidate)
		if (ServerTransport
				.Start({TransportRole::Server, {"127.0.0.1", static_cast<std::uint16_t>(Candidate)}, Negotiated, {}})
				.Succeeded()) {
			Port = static_cast<std::uint16_t>(Candidate);
			break;
		}
	Check(Port != 0, "localhost GNS Character server binds a bounded port");
	if (Port == 0) return 1;
	Check(
		ClientTransport.Start({TransportRole::Client, {"127.0.0.1", Port}, Negotiated, {}}).Succeeded(),
		"localhost GNS Character client starts"
	);
	ConnectionId ServerConnection;
	ConnectionId ClientConnection;
	const auto ConnectDeadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < ConnectDeadline &&
		   (!ServerConnection.IsValid() || !ClientConnection.IsValid())) {
		if (!ServerConnection.IsValid()) ServerConnection = ConnectedId(Drain(ServerTransport));
		if (!ClientConnection.IsValid()) ClientConnection = ConnectedId(Drain(ClientTransport));
		std::this_thread::sleep_for(1ms);
	}
	Check(ServerConnection.IsValid() && ClientConnection.IsValid(), "localhost GNS Character peers connect");
	if (!ServerConnection.IsValid() || !ClientConnection.IsValid()) return 1;
	NetworkScheduler ServerScheduler(ServerTransport);
	NetworkScheduler ClientScheduler(ClientTransport);
	Check(
		ServerScheduler.RegisterConnection(ServerConnection, Negotiated) &&
			ClientScheduler.RegisterConnection(ClientConnection, Negotiated),
		"localhost GNS Character schedulers register"
	);
	AuthoritativeCharacterNetwork Server(ServerScheduler, Negotiated, Movement);
	PredictedCharacterNetwork Client(ClientScheduler, Negotiated, Movement);
	WorldRoot ServerWorld;
	WorldRoot ClientWorld;
	auto ServerCharacter = std::make_shared<KinematicCharacter>();
	auto ClientCharacter = std::make_shared<KinematicCharacter>();
	const auto Source = ServerCharacter->GetObjectId();
	Check(
		Server.AddPeer(ServerConnection) && Client.AddPeer(ClientConnection) &&
			Server.RegisterCharacter(ServerCharacter) &&
			Server.MarkMaterialized(ServerConnection, Source, StateChannelId(77)) &&
			Client.MarkMaterialized(Source, ClientCharacter),
		"localhost GNS Character managers establish materialization"
	);
	Check(
		Server.RegisterAction(Action()) && Client.RegisterAction(Action()),
		"localhost GNS Character peers pin matching action content"
	);
	Check(
		Server.BindControl(ServerConnection, Source, 1).has_value(), "localhost GNS server emits reliable control bind"
	);

	bool Submitted = false;
	std::uint64_t Tick = 2;
	const auto Deadline = std::chrono::steady_clock::now() + 5s;
	while (std::chrono::steady_clock::now() < Deadline) {
		(void)ServerScheduler.Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Negotiated));
		(void)ClientScheduler.Flush(ClientConnection, SchedulerTickBudget::FromNetworkLimits(Negotiated));
		for (const auto &Event : Drain(ServerTransport))
			(void)Server.HandleTransportEvent(Event);
		for (const auto &Event : Drain(ClientTransport))
			(void)Client.HandleTransportEvent(Event);
		if (!Submitted && Client.GetControl(ClientConnection)) {
			Submitted = Client.RequestAction(ClientConnection, 1, Tick) &&
						Client.SubmitInput(
							ClientConnection, ClientWorld, Tick, 1.0f / 60.0f, {1.0f, 0.0f}, 0.0f, false
						);
		} else if (Submitted && Client.GetControl(ClientConnection) && Tick < 8) {
			(void)Client.SubmitInput(ClientConnection, ClientWorld, Tick, 1.0f / 60.0f, {0.0f, 0.0f}, 0.0f, false);
		}
		Server.Step(ServerWorld, Tick);
		Client.Reconcile(ClientWorld);
		if (Submitted && ServerCharacter->GetPosition().x > 0.1f &&
			glm::length(ServerCharacter->GetPosition() - ClientCharacter->GetPosition()) < 0.05f)
			break;
		++Tick;
		std::this_thread::sleep_for(1ms);
	}
	Check(
		Submitted && ServerCharacter->GetPosition().x > 0.1f,
		"localhost GNS carries semantic input/action to authoritative server movement"
	);
	Check(
		glm::length(ServerCharacter->GetPosition() - ClientCharacter->GetPosition()) < 0.05f,
		"localhost GNS returns authoritative state and reconciles the predicted client"
	);
	(void)ClientTransport.Stop({DisconnectReason::LocalShutdown, "Character GNS test shutdown"});
	(void)ServerTransport.Stop({DisconnectReason::LocalShutdown, "Character GNS test shutdown"});
	if (Failures != 0) return 1;
	std::cout << "Character GNS test passed\n";
	return 0;
}

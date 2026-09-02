#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/Players.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

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

	GameSessionConfiguration Configuration(GameSessionRole Role, std::uint16_t Port) {
		return {
			.Role = Role,
			.Endpoint = {"127.0.0.1", Port},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 600,
			.ClientNonce = Role == GameSessionRole::Client ? 0x3dfeed1234ull : 0,
		};
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	gargantuan::BootstrapNativeRuntimeSchema();

	auto ServerWorld = std::make_shared<DataModel>();
	HeadlessRenderer ServerRenderer(Vector2(320, 240));
	Engine ServerRuntime(
		ServerWorld,
		&ServerRenderer,
		nullptr,
		EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
	);
	ServerRuntime.ProcessService->Alive = true;

	std::shared_ptr<GameNetworkingSocketsTransport> ServerTransport;
	std::unique_ptr<GameSession> Server;
	std::uint16_t Port = 0;
	for (std::uint32_t Candidate = 39400; Candidate < 39500; ++Candidate) {
		auto CandidateTransport = std::make_shared<GameNetworkingSocketsTransport>();
		auto CandidateSession = std::make_unique<GameSession>(
			CandidateTransport,
			Configuration(GameSessionRole::Server, static_cast<std::uint16_t>(Candidate)),
			&ServerRuntime
		);
		if (!CandidateSession->Start().Succeeded()) continue;
		Port = static_cast<std::uint16_t>(Candidate);
		ServerTransport = std::move(CandidateTransport);
		Server = std::move(CandidateSession);
		break;
	}
	Check(Port != 0 && Server, "real GNS GameSession server binds a bounded loopback port");
	if (!Server) return 1;

	auto ClientTransport = std::make_shared<GameNetworkingSocketsTransport>();
	GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, Port));
	Check(Client.Start().Succeeded(), "real GNS GameSession client starts");

	std::unique_ptr<HeadlessRenderer> ClientRenderer;
	std::unique_ptr<Engine> ClientRuntime;
	std::uint64_t Tick = 1;
	const auto ReadyDeadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < ReadyDeadline && Server->GetMetrics().ReadyPeers != 1) {
		(void)Server->Poll();
		(void)Client.Poll();
		if (!ClientRuntime && Client.GetClientDataModel()) {
			ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
			ClientRuntime = std::make_unique<Engine>(
				Client.GetClientDataModel(),
				ClientRenderer.get(),
				std::function<void(std::string, std::string)>{},
				EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
			);
			ClientRuntime->ProcessService->Alive = true;
			Check(Client.AttachClientRuntime(*ClientRuntime), "real GNS client attaches after trusted bootstrap");
		}
		if (ClientRuntime) ClientRuntime->Step();
		ServerRuntime.Step();
		Server->Step(Tick);
		Client.Step(Tick);
		++Tick;
		std::this_thread::sleep_for(1ms);
	}

	Check(
		Server->GetMetrics().ReadyPeers == 1 && Client.GetStatus() == GameSessionStatus::Ready,
		"real GNS completes accepted peer, trusted LocalPlayer, and gameplay-ready phases"
	);
	auto ServerPlayers = ServerRuntime.Players->GetPlayers();
	Check(
		ServerPlayers.size() == 1 && ServerPlayers.front()->GetCharacter().has_value(),
		"real GNS server owns Player and Character creation"
	);
	Check(
		ClientRuntime && ClientRuntime->Players->GetLocalPlayer().has_value(),
		"real GNS client resolves its exact trusted LocalPlayer ObjectId"
	);

	if (ClientRuntime && !ServerPlayers.empty() && ServerPlayers.front()->GetCharacter()) {
		auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*ServerPlayers.front()->GetCharacter());
		const auto InitialPosition = CharacterValue ? CharacterValue->GetPosition() : glm::vec3{};
		(void)ClientRuntime->ProcessEvent(
			KeyEvent{
				.Device = {1},
				.Physical = PhysicalKey::W,
				.Logical = LogicalKey::W,
				.State = ButtonState::Pressed,
			}
		);
		const auto MovementDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < MovementDeadline &&
			   (!CharacterValue || glm::distance(CharacterValue->GetPosition(), InitialPosition) <= 0.25f)) {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		}
		Check(
			CharacterValue && glm::distance(CharacterValue->GetPosition(), InitialPosition) > 0.25f,
			"real GNS carries ordinary Luau semantic input to authoritative Character movement"
		);
	}

	Client.Stop();
	Server->Stop();
	if (ClientRuntime) ClientRuntime->Destroy();
	ServerRuntime.Destroy();
	if (Failures == 0) std::cout << "Real GNS game-session lifecycle tests passed\n";
	return Failures == 0 ? 0 : 1;
}

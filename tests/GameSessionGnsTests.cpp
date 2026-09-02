#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/AssetService.hpp"
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
	DiskFilesystem SampleFilesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
	auto ServerAssets = std::dynamic_pointer_cast<AssetService>(ServerWorld->GetService("AssetService"));
	ServerAssets->LoadProjectAssets(SampleFilesystem);
	const auto RuntimeAssets = ServerAssets->CaptureRuntimeAssets();
	auto ServerActionPolicy = std::make_shared<Script>();
	ServerActionPolicy->SetName("GnsActionPolicy");
	ServerActionPolicy->SetRunContext(Enums::RunContext::Server);
	ServerActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
assert(CharacterControl:RegisterAction(
	"GnsLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetActionPolicy(function(Player, Character, ActionName)
	local Accepted = ActionName == "GnsLunge" and Player.Character == Character
	if Accepted then
		Character:SetAttribute("GnsActionAuthorized", true)
	end
	return Accepted
end)
)");
	ServerActionPolicy->SetParent(ServerWorld);
	auto ClientActionPolicy = std::make_shared<Script>();
	ClientActionPolicy->SetName("GnsActionRequest");
	ClientActionPolicy->SetRunContext(Enums::RunContext::Client);
	ClientActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
assert(CharacterControl:RegisterAction(
	"GnsLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName == "GnsLunge" and Accepted then
		Character:SetAttribute("GnsActionResolved", true)
	end
end)
local Requested = false
RunService.PreSimulation:Connect(function()
	if not Requested and Players.LocalPlayer and Players.LocalPlayer.Character then
		Requested = CharacterControl:RequestAction("GnsLunge")
	end
end)
)");
	ClientActionPolicy->SetParent(ServerWorld);
	auto RelevanceNpc = std::make_shared<KinematicCharacter>();
	RelevanceNpc->SetName("GnsRelevanceNpc");
	auto RelevanceRoot = std::make_shared<Part>();
	RelevanceRoot->SetName("GnsRelevanceRoot");
	RelevanceRoot->SetParent(RelevanceNpc);
	RelevanceNpc->SetRootPart(RelevanceRoot);
	RelevanceNpc->SetPosition({5'000.0f, 6.0f, 0.0f});
	RelevanceNpc->SetParent(ServerWorld);
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
			auto ClientAssets = std::dynamic_pointer_cast<AssetService>(
				Client.GetClientDataModel()->GetService("AssetService")
			);
			ClientAssets->LoadRuntimeAssetSnapshot(RuntimeAssets);
			Check(
				PackageBuilder::HydrateClientCode(ServerWorld, Client.GetClientDataModel()) >= 1,
				"real GNS client hydrates trusted packaged client policy"
			);
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
		auto FindClientRelevanceNpc = [&]() {
			return ClientRuntime ? std::dynamic_pointer_cast<KinematicCharacter>(
							   ClientRuntime->DataModel->FindFirstChild("GnsRelevanceNpc", true)
						   )
						 : nullptr;
		};
		auto StepNetwork = [&]() {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		};
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

		const auto BeforeAction = CharacterValue ? CharacterValue->GetPosition() : glm::vec3{};
		const auto ActionDeadline = std::chrono::steady_clock::now() + 10s;
		bool ActionResolved = false;
		while (std::chrono::steady_clock::now() < ActionDeadline &&
			   (!ActionResolved || !CharacterValue || CharacterValue->GetPosition().x <= BeforeAction.x + 0.4f)) {
			ClientRuntime->Step();
			ServerRuntime.Step();
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter())
				ActionResolved =
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("GnsActionResolved").has_value();
			std::this_thread::sleep_for(1ms);
		}
		Check(
			CharacterValue && CharacterValue->GetAttributeValue("GnsActionAuthorized").has_value(),
			"real GNS action reaches generic server Luau authorization"
		);
		Check(ActionResolved, "real GNS authoritative action state resolves to client Luau");
		Check(
			CharacterValue && CharacterValue->GetPosition().x > BeforeAction.x + 0.4f,
			"real GNS action state applies server-owned pinned root motion"
		);

		Check(!FindClientRelevanceNpc(), "real GNS initial materialization excludes a distant NPC");
		const auto FirstNpcTarget = CharacterValue->GetPosition() + glm::vec3(96.0f, 0.0f, 0.0f);
		RelevanceNpc->SetPosition(FirstNpcTarget);
		const auto FirstNpcDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < FirstNpcDeadline && !FindClientRelevanceNpc())
			StepNetwork();
		auto FirstNpcReplica = FindClientRelevanceNpc();
		Check(
			FirstNpcReplica && FirstNpcReplica->GetRootPart() &&
				glm::distance(FirstNpcReplica->GetPosition(), FirstNpcTarget) < 1.0f,
			"real GNS materializes an entering NPC with its RootPart and current authoritative transform"
		);

		RelevanceNpc->SetPosition({6'000.0f, 6.0f, 0.0f});
		const auto NpcLeaveDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < NpcLeaveDeadline && FindClientRelevanceNpc()) StepNetwork();
		Check(
			!FindClientRelevanceNpc() && !RelevanceNpc->GetDestroyed(),
			"real GNS peer unpublish removes the NPC replica without destroying server authority"
		);
		const auto ReentryNpcTarget = CharacterValue->GetPosition() + glm::vec3(128.0f, 0.0f, 0.0f);
		RelevanceNpc->SetPosition(ReentryNpcTarget);
		const auto NpcReentryDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < NpcReentryDeadline && !FindClientRelevanceNpc()) StepNetwork();
		auto ReenteredNpcReplica = FindClientRelevanceNpc();
		Check(
			ReenteredNpcReplica && ReenteredNpcReplica->GetRootPart() &&
				glm::distance(ReenteredNpcReplica->GetPosition(), ReentryNpcTarget) < 1.0f,
			"real GNS reentry uses the NPC's current state rather than replaying off-interest motion"
		);

		const auto ClientConnection = Client.GetPrimaryConnection();
		Check(ClientConnection.has_value(), "real GNS client retains its session connection identity");
		if (ClientConnection)
			(void)ClientTransport->Disconnect(
				*ClientConnection, {DisconnectReason::LocalShutdown, "real GNS lifecycle test disconnect"}
			);
		const auto DisconnectDeadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < DisconnectDeadline && !ServerRuntime.Players->GetPlayers().empty()) {
			(void)Server->Poll();
			(void)Client.Poll();
			Server->Step(Tick);
			Client.Step(Tick);
			++Tick;
			std::this_thread::sleep_for(1ms);
		}
		Check(
			ServerRuntime.Players->GetPlayers().empty() && Server->GetMetrics().PlayersRemoved == 1,
			"real GNS disconnect revokes control and removes the authoritative Player"
		);
		Check(
			CharacterValue && CharacterValue->GetDestroyed(), "real GNS disconnect destroys default Character policy"
		);
		Check(
			Client.GetStatus() == GameSessionStatus::Failed && !ClientRuntime->Players->GetLocalPlayer().has_value(),
			"real GNS hard disconnect stops client control and clears trusted LocalPlayer"
		);
	}

	Client.Stop();
	Server->Stop();
	if (ClientRuntime) ClientRuntime->Destroy();
	ServerRuntime.Destroy();
	if (Failures == 0) std::cout << "Real GNS game-session lifecycle tests passed\n";
	return Failures == 0 ? 0 : 1;
}

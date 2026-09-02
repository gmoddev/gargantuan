#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Players.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
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

	float HorizontalDistance(const glm::vec3 &Left, const glm::vec3 &Right) {
		return glm::distance(glm::vec2(Left.x, Left.z), glm::vec2(Right.x, Right.z));
	}

	GameSessionConfiguration Configuration(GameSessionRole Role, std::string Endpoint = "game-session") {
		return {
			.Role = Role,
			.Endpoint = {std::move(Endpoint), 27020},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 120,
			.ClientNonce = Role == GameSessionRole::Client ? 0x12345678abcdefull : 0,
		};
	}

	void Advance(
		const std::shared_ptr<SimulatedNetwork> &Network, GameSession &Server, GameSession &Client, std::uint64_t Tick
	) {
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)Server.Poll();
		(void)Client.Poll();
		Server.Step(Tick);
		Client.Step(Tick);
		(void)Network->Advance(20ms);
	}

	void TestProtocolBounds() {
		const auto Limits = GameSessionConfiguration::DefaultLimits();
		GameSessionMessage Message = GameSessionClientHello{77, Limits};
		auto Encoded = EncodeGameSessionMessage(Message);
		Check(
			Encoded.has_value() && Encoded->size() <= MaximumGameSessionFrameBytes,
			"GSES client hello has a bounded representation"
		);
		Check(Encoded && DecodeGameSessionMessage(*Encoded).has_value(), "GSES client hello round-trips");
		if (!Encoded) return;
		for (std::size_t Size = 0; Size < Encoded->size(); ++Size)
			Check(
				!DecodeGameSessionMessage(std::span(*Encoded).first(Size)),
				"every truncated GSES client hello is rejected"
			);
		auto Trailing = *Encoded;
		Trailing.push_back(std::byte{0});
		Check(!DecodeGameSessionMessage(Trailing), "GSES rejects trailing bytes");
		auto WrongVersion = *Encoded;
		WrongVersion[4] = std::byte{0xff};
		Check(!DecodeGameSessionMessage(WrongVersion), "GSES rejects an unsupported protocol version");
		auto WrongMagic = *Encoded;
		WrongMagic[0] = std::byte{0};
		Check(!DecodeGameSessionMessage(WrongMagic), "GSES rejects an invalid protocol magic");
		auto UnknownKind = *Encoded;
		UnknownKind[6] = std::byte{0xff};
		Check(!DecodeGameSessionMessage(UnknownKind), "GSES rejects an unknown message kind");
		auto ReservedHeader = *Encoded;
		ReservedHeader[7] = std::byte{1};
		Check(!DecodeGameSessionMessage(ReservedHeader), "GSES rejects non-zero reserved header data");
		auto Oversized = *Encoded;
		Oversized.resize(MaximumGameSessionFrameBytes + 1);
		Check(!DecodeGameSessionMessage(Oversized), "GSES rejects an oversized bootstrap frame before decoding");
		auto Accepted = EncodeGameSessionMessage(
			GameSessionServerAccepted{
				.Nonce = 77,
				.SessionEpoch = 1,
				.Replication = ReplicationEpoch(1),
				.Player = {1, 1},
				.PlayerId = 1,
				.Identity = SessionIdentityKind::DevelopmentLocal,
				.NegotiatedLimits = Limits,
			}
		);
		Check(Accepted.has_value(), "bounded GSES acceptance encodes");
		if (Accepted) {
			(*Accepted)[44] = std::byte{0xff};
			Check(!DecodeGameSessionMessage(*Accepted), "GSES rejects an unknown session identity kind");
		}
	}

	void TestPreAcceptanceAndTimeoutRejection() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto RawTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			[](std::string Code, std::string Message) {
				if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
			},
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "hostile-session");
		ServerConfiguration.HandshakeTimeoutTicks = 3;
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(Server.Start().Succeeded(), "hostile-session server starts");
		Check(
			RawTransport
				->Start({
					.Role = TransportRole::Client,
					.Endpoint = ServerConfiguration.Endpoint,
					.AdvertisedLimits = ServerConfiguration.Limits,
				})
				.Succeeded(),
			"raw transport reaches the production session endpoint"
		);
		Network->Pump();
		(void)Server.Poll();
		std::array<TransportEvent, 8> RawEvents;
		const auto EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<ConnectionId> RawConnection;
		for (std::size_t Index = 0; Index < EventCount; ++Index)
			if (const auto *Changed = std::get_if<ConnectionStateEvent>(&RawEvents[Index]);
				Changed && Changed->Current == ConnectionState::Connected)
				RawConnection = Changed->Connection;
		Check(RawConnection.has_value(), "raw transport exposes its generation-safe connection identity");
		if (RawConnection) {
			std::vector<std::byte> Premature{
				std::byte{'G'},
				std::byte{'C'},
				std::byte{'H'},
				std::byte{'R'},
				std::byte{0},
			};
			auto Intent = MakeNetworkMessageIntent(
				*RawConnection,
				DeliveryMode::ReliableOrdered,
				TrafficClass::ReliableApplication,
				{},
				std::move(Premature),
				ServerConfiguration.Limits
			);
			Check(Intent && RawTransport->Send(*Intent).Succeeded(), "raw transport sends a premature gameplay frame");
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
		}
		Check(
			Server.GetMetrics().RejectedPreAcceptanceMessages == 1 && ServerRuntime.Players->GetPlayers().empty(),
			"gameplay before GSES acceptance is rejected without creating a Player"
		);
		Server.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "hostile-session complete"});
		ServerRuntime.Destroy();

		Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		ServerTransport = Network->CreateTransport();
		RawTransport = Network->CreateTransport();
		ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer TimeoutRenderer(Vector2(320, 240));
		Engine TimeoutRuntime(
			ServerWorld,
			&TimeoutRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		TimeoutRuntime.ProcessService->Alive = true;
		GameSession TimeoutServer(ServerTransport, ServerConfiguration, &TimeoutRuntime);
		Check(
			TimeoutServer.Start().Succeeded() && RawTransport
													 ->Start({
														 .Role = TransportRole::Client,
														 .Endpoint = ServerConfiguration.Endpoint,
														 .AdvertisedLimits = ServerConfiguration.Limits,
													 })
													 .Succeeded(),
			"silent peer reaches the timeout fixture"
		);
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)TimeoutServer.Poll();
		TimeoutServer.Step(3);
		Check(
			TimeoutServer.GetMetrics().HandshakeTimeouts == 1 && TimeoutRuntime.Players->GetPlayers().empty(),
			"silent handshake expires without allocating a Player identity"
		);
		TimeoutServer.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "timeout fixture complete"});
		TimeoutRuntime.Destroy();
	}

	void TestSpoofedReadyPlayerRejection() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto RawTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "spoofed-ready-session");
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(
			Server.Start().Succeeded() && RawTransport
											  ->Start({
												  .Role = TransportRole::Client,
												  .Endpoint = ServerConfiguration.Endpoint,
												  .AdvertisedLimits = ServerConfiguration.Limits,
											  })
											  .Succeeded(),
			"raw peer reaches the spoofed readiness fixture"
		);
		Network->Pump();
		(void)Server.Poll();
		std::array<TransportEvent, 16> RawEvents;
		auto EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<ConnectionId> RawConnection;
		for (std::size_t Index = 0; Index < EventCount; ++Index)
			if (const auto *Changed = std::get_if<ConnectionStateEvent>(&RawEvents[Index]);
				Changed && Changed->Current == ConnectionState::Connected)
				RawConnection = Changed->Connection;
		Check(RawConnection.has_value(), "spoofed readiness fixture obtains its connection identity");
		if (!RawConnection) {
			Server.Stop();
			ServerRuntime.Destroy();
			return;
		}

		auto Hello = EncodeGameSessionMessage(GameSessionClientHello{0xabcdef12345678ull, ServerConfiguration.Limits});
		auto HelloIntent = Hello ? MakeNetworkMessageIntent(
									   *RawConnection,
									   DeliveryMode::ReliableOrdered,
									   TrafficClass::Control,
									   {},
									   std::move(*Hello),
									   ServerConfiguration.Limits
								   )
								 : std::nullopt;
		Check(
			HelloIntent && RawTransport->Send(*HelloIntent).Succeeded(), "raw peer submits a valid bounded client hello"
		);
		(void)Network->Advance(2ms);
		Network->Pump();
		(void)Server.Poll();
		Server.Step(1);
		(void)Network->Advance(2ms);
		Network->Pump();
		EventCount = RawTransport->PollEvents(RawEvents);
		std::optional<GameSessionServerAccepted> Accepted;
		for (std::size_t Index = 0; Index < EventCount; ++Index) {
			const auto *Received = std::get_if<ReceivedMessageEvent>(&RawEvents[Index]);
			if (!Received || !IsGameSessionFrame(Received->Payload)) continue;
			auto Decoded = DecodeGameSessionMessage(Received->Payload);
			if (Decoded)
				if (const auto *Value = std::get_if<GameSessionServerAccepted>(&*Decoded)) Accepted = *Value;
		}
		Check(Accepted.has_value(), "server returns an accepted identity to the raw peer");
		if (Accepted) {
			auto SpoofedPlayer = Accepted->Player;
			++SpoofedPlayer.Slot;
			auto Ready = EncodeGameSessionMessage(
				GameSessionClientReady{Accepted->SessionEpoch, Accepted->Replication, SpoofedPlayer}
			);
			auto ReadyIntent = Ready ? MakeNetworkMessageIntent(
										   *RawConnection,
										   DeliveryMode::ReliableOrdered,
										   TrafficClass::Control,
										   {},
										   std::move(*Ready),
										   Accepted->NegotiatedLimits
									   )
									 : std::nullopt;
			Check(
				ReadyIntent && RawTransport->Send(*ReadyIntent).Succeeded(),
				"raw peer submits readiness for a different Player ObjectId"
			);
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
		}
		Check(
			Server.GetMetrics().ProtocolRejects == 1 && Server.GetMetrics().ReadyPeers == 0 &&
				ServerRuntime.Players->GetPlayers().empty(),
			"spoofed Player readiness tears down the partially admitted Player without gameplay authority"
		);
		Server.Stop();
		(void)RawTransport->Stop({DisconnectReason::LocalShutdown, "spoofed readiness complete"});
		ServerRuntime.Destroy();
	}

	void TestAcceptedConnectionChurn() {
		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BaseLatency = 1ms;
		TransportConfiguration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		TransportConfiguration.MaximumTransports = 128;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto ServerConfiguration = Configuration(GameSessionRole::Server, "session-churn");
		GameSession Server(ServerTransport, ServerConfiguration, &ServerRuntime);
		Check(Server.Start().Succeeded(), "connection churn server starts");
		std::uint64_t Tick = 1;
		for (std::uint64_t Cycle = 1; Cycle <= 100; ++Cycle) {
			auto ClientTransport = Network->CreateTransport();
			GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, "session-churn"));
			Check(Client.Start().Succeeded(), "connection churn client starts");
			for (std::uint64_t Attempt = 1; Attempt <= 120 && Server.GetMetrics().AcceptedPeers != Cycle;
				 ++Attempt, ++Tick) {
				(void)Network->Advance(2ms);
				Network->Pump();
				(void)Server.Poll();
				(void)Client.Poll();
				Server.Step(Tick);
				Client.Step(Attempt);
			}
			Check(Server.GetMetrics().AcceptedPeers == Cycle, "connection churn peer is admitted");
			Client.Stop();
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
			Server.Step(Tick++);
			Check(ServerRuntime.Players->GetPlayers().empty(), "connection churn removes its authoritative Player");
		}
		const auto Metrics = Server.GetMetrics();
		std::cout << "[Network:SessionTest] churnAccepted=" << Metrics.AcceptedPeers
				  << " created=" << Metrics.PlayersCreated << " removed=" << Metrics.PlayersRemoved
				  << " handshakeRejected=" << Metrics.RejectedHandshakes
				  << " protocolRejected=" << Metrics.ProtocolRejects << " timeouts=" << Metrics.HandshakeTimeouts
				  << '\n';
		Check(
			Metrics.AcceptedPeers == 100 && Metrics.PlayersCreated == 100 && Metrics.PlayersRemoved == 100 &&
				ServerRuntime.Players->GetPlayers().empty(),
			"one hundred accepted connection lifetimes return Player gauges to baseline"
		);
		Server.Stop();
		ServerRuntime.Destroy();
	}

	void TestProductionLifecycleComposition() {
		SimulatedTransportConfiguration TransportConfiguration;
		TransportConfiguration.BaseLatency = 1ms;
		auto Network = SimulatedNetwork::Create(TransportConfiguration);
		auto ServerTransport = Network->CreateTransport();
		auto ClientTransport = Network->CreateTransport();
		Check(Network && ServerTransport && ClientTransport, "session test allocates bounded transports");
		if (!Network || !ServerTransport || !ClientTransport) return;

		auto ServerWorld = std::make_shared<DataModel>();
		DiskFilesystem SampleFilesystem(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT));
		auto ServerAssets = std::dynamic_pointer_cast<AssetService>(ServerWorld->GetService("AssetService"));
		ServerAssets->LoadProjectAssets(SampleFilesystem);
		const auto RuntimeAssets = ServerAssets->CaptureRuntimeAssets();
		auto ServerActionPolicy = std::make_shared<Script>();
		auto SessionRemote = std::make_shared<RemoteEvent>();
		SessionRemote->SetName("SessionRemote");
		SessionRemote->SetParent(ServerWorld);
		ServerActionPolicy->SetName("SessionActionPolicy");
		ServerActionPolicy->SetRunContext(Enums::RunContext::Server);
		ServerActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local SessionRemote = game:FindFirstChild("SessionRemote")
SessionRemote.OnServerEvent:Connect(function(Peer, Message)
	if type(Peer) == "table" and Message == "session-ready" then
		CharacterControl:SetAttribute("SessionRemoteReceived", true)
	end
end)
local function InstallPresentationRig(Player)
	local Character = Player.Character
	if Character == nil or Character:FindFirstChild("SessionRig") ~= nil then
		return
	end
	local Rig = Instance.new("MeshPart")
	Rig.Name = "SessionRig"
	Rig.Mesh = "asset://d549080bd1e64aaee8041f4ece3e9f75"
	Rig.Anchored = true
	Rig.CanCollide = false
	Rig.CanTouch = false
	Rig.Size = Vector3.new(2, 2, 2)
	Rig.CFrame = Character.CFrame
	Rig.Parent = Character
	local Animator = Instance.new("Animator")
	Animator.Name = "SessionAnimator"
	Animator.Parent = Rig
end
Players.PlayerAdded:Connect(InstallPresentationRig)
assert(CharacterControl:RegisterAction(
	"SessionLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
assert(CharacterControl:RegisterAction(
	"DeniedLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetAttribute("ServerActionRegistered", true)
CharacterControl:SetActionPolicy(function(Player, Character, ActionName)
	local Accepted = ActionName == "SessionLunge" and Player.Character == Character
	if ActionName == "SessionLunge" then
		Character:SetAttribute("SessionActionValidated", Accepted)
	elseif ActionName == "DeniedLunge" then
		Character:SetAttribute("DeniedActionObserved", true)
	end
	return Accepted
end)
)");
		ServerActionPolicy->SetParent(ServerWorld);
		auto ClientActionPolicy = std::make_shared<Script>();
		ClientActionPolicy->SetName("SessionActionRequest");
		ClientActionPolicy->SetRunContext(Enums::RunContext::Client);
		ClientActionPolicy->SetSource(R"(
local CharacterControl = game:GetService("CharacterControlService")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local SessionRemote = game:FindFirstChild("SessionRemote")
assert(CharacterControl:RegisterAction(
	"SessionLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
assert(CharacterControl:RegisterAction(
	"DeniedLunge",
	"asset://d9d9e9649adbad59588d137c2a642e1d",
	0.5,
	Vector3.new(0.9, 0, 0),
	0,
	true
))
CharacterControl:SetAttribute("ClientActionRegistered", true)
local Resolved = false
local DeniedDelay = 0
CharacterControl.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName == "SessionLunge" and Accepted then
		Resolved = true
		Character:SetAttribute("SessionActionResolved", true)
		DeniedDelay = 32
	elseif ActionName == "DeniedLunge" and not Accepted then
		Character:SetAttribute("DeniedActionRejected", true)
	end
end)
CharacterControl.ActionEnded:Connect(function(Character, ActionName)
	if ActionName == "SessionLunge" then
		Character:SetAttribute("SessionActionEnded", true)
	end
end)
local Requested = false
local RemoteSent = false
RunService.PreSimulation:Connect(function()
	if DeniedDelay > 0 then
		DeniedDelay -= 1
		if DeniedDelay == 0 and Players.LocalPlayer and Players.LocalPlayer.Character then
			if CharacterControl:RequestAction("DeniedLunge") then
				Players.LocalPlayer.Character:SetAttribute("DeniedActionRequested", true)
			end
		end
	end
	if not Requested and Players.LocalPlayer and Players.LocalPlayer.Character then
		Requested = CharacterControl:RequestAction("SessionLunge")
		if Requested then
			Players.LocalPlayer.Character:SetAttribute("SessionActionRequested", true)
		end
	end
	if not RemoteSent and Players.LocalPlayer and Players.LocalPlayer.Character then
		SessionRemote:FireServer("session-ready")
		RemoteSent = true
	end
end)
)");
		ClientActionPolicy->SetParent(ServerWorld);
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			[](std::string Code, std::string Message) {
				if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
			},
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		GameSession Server(ServerTransport, Configuration(GameSessionRole::Server), &ServerRuntime);
		GameSession Client(ClientTransport, Configuration(GameSessionRole::Client));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "production session endpoints start");

		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		for (std::uint64_t Tick = 1; Tick <= 80; ++Tick) {
			Advance(Network, Server, Client, Tick);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				auto ClientAssets = std::dynamic_pointer_cast<AssetService>(
					Client.GetClientDataModel()->GetService("AssetService")
				);
				ClientAssets->LoadRuntimeAssetSnapshot(RuntimeAssets);
				Check(
					PackageBuilder::HydrateClientCode(ServerWorld, Client.GetClientDataModel()) >= 1,
					"trusted packaged client code hydrates the authoritative replicated graph"
				);
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				ClientRuntime = std::make_unique<Engine>(
					Client.GetClientDataModel(),
					ClientRenderer.get(),
					[](std::string Code, std::string Message) {
						if (Code == "Error") std::cerr << "[Network:SessionTest] " << Message << '\n';
					},
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				ClientRuntime->ProcessService->Alive = true;
				Check(
					Client.AttachClientRuntime(*ClientRuntime),
					"client gameplay bridge attaches after trusted bootstrap"
				);
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
			if (Server.GetMetrics().ReadyPeers == 1) break;
		}

		Check(
			ClientRuntime && Client.GetStatus() == GameSessionStatus::Ready,
			"client reaches ready only after replicated runtime attachment"
		);
		Check(
			Server.GetMetrics().AcceptedPeers == 1 && Server.GetMetrics().ReadyPeers == 1,
			"server distinguishes accepted and gameplay-ready peer phases"
		);
		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		Check(ServerPlayers.size() == 1, "server creates exactly one authoritative Player for the accepted peer");
		Check(
			ServerPlayers.empty() || (ServerPlayers[0]->GetAuthenticationIdentity() &&
									  ServerPlayers[0]->GetAuthenticationIdentity()->Provider == "session-development"),
			"3D labels development-local identity without claiming external authentication"
		);
		Check(
			ServerPlayers.empty() || ServerPlayers[0]->GetCharacter().has_value(),
			"server-shipped Luau owns automatic Character assembly"
		);
		Check(
			ClientRuntime && ClientRuntime->Players->GetLocalPlayer().has_value(),
			"client establishes LocalPlayer from the trusted server ObjectId"
		);
		if (ClientRuntime && ClientRuntime->Players->GetLocalPlayer()) {
			auto LocalPlayer = *ClientRuntime->Players->GetLocalPlayer();
			Check(LocalPlayer->GetCharacter().has_value(), "client materializes the server-owned Player.Character");
			Check(LocalPlayer->GetPlayerId() == 1, "session PlayerId survives structural replication");
		}
		Check(
			Server.GetMetrics().CharacterControlBindings == 1,
			"Player.Character drives one authoritative Character control binding"
		);
		if (ClientRuntime && !ServerPlayers.empty() && ServerPlayers[0]->GetCharacter()) {
			auto ServerCharacter = std::dynamic_pointer_cast<KinematicCharacter>(*ServerPlayers[0]->GetCharacter());
			const auto InitialPosition = ServerCharacter ? ServerCharacter->GetPosition() : glm::vec3{};
			for (std::uint64_t Tick = 81; Tick <= 120; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				Advance(Network, Server, Client, Tick);
			}
			if (ServerCharacter)
				std::cout << "[Network:SessionTest] actionDeltaX="
						  << ServerCharacter->GetPosition().x - InitialPosition.x
						  << " validated=" << ServerCharacter->GetAttributeValue("SessionActionValidated").has_value()
						  << '\n';
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter())
				std::cout
					<< "[Network:SessionTest] actionRequested="
					<< (*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionRequested").has_value()
					<< " resolved="
					<< (*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionResolved").has_value()
					<< '\n';
			std::cout << "[Network:SessionTest] clientActionRegistered="
					  << ClientRuntime->CharacterControl->GetAttributeValue("ClientActionRegistered").has_value()
					  << '\n';
			Check(
				ServerCharacter && ServerCharacter->GetAttributeValue("SessionActionValidated").has_value(),
				"server Luau validates the semantic action against the authoritative Player.Character"
			);
			Check(
				ServerCharacter && ServerCharacter->GetAttributeValue("DeniedActionObserved").has_value(),
				"server Luau observes and rejects a registered production action"
			);
			Check(
				ServerRuntime.CharacterControl->GetAttributeValue("SessionRemoteReceived").has_value(),
				"accepted production peer routes an ordinary reliable RemoteEvent over the shared session lifetime"
			);
			Check(
				Client.GetMetrics().ActionsPresented >= 1 && Client.GetMetrics().ActionPresentationStops >= 1,
				"client starts and terminates bounded action presentation from authoritative phase state"
			);
			if (auto LocalPlayer = ClientRuntime->Players->GetLocalPlayer();
				LocalPlayer && (*LocalPlayer)->GetCharacter()) {
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionRequested").has_value(),
					"client game Luau requests the action through the high-level Character bridge"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionResolved").has_value(),
					"accepted authoritative action resolution returns to client game Luau"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("DeniedActionRequested").has_value() &&
						(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("DeniedActionRejected").has_value(),
					"rejected authoritative action terminates through the semantic client result bridge"
				);
				Check(
					(*LocalPlayer)->GetCharacter().value()->GetAttributeValue("SessionActionEnded").has_value(),
					"authoritative action lifetime exposes a semantic end independent of visual completion"
				);
			}
			Check(
				ServerCharacter && ServerCharacter->GetPosition().x > InitialPosition.x + 0.4f,
				"game-defined semantic action crosses the production session and uses server-chosen pinned root motion"
			);
			const auto BeforeInput = ServerCharacter ? ServerCharacter->GetPosition() : glm::vec3{};
			(void)ClientRuntime->ProcessEvent(
				KeyEvent{
					.Device = {1},
					.Physical = PhysicalKey::W,
					.Logical = LogicalKey::W,
					.State = ButtonState::Pressed,
				}
			);
			for (std::uint64_t Tick = 121; Tick <= 160; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				Advance(Network, Server, Client, Tick);
			}
			Check(
				ServerCharacter && glm::distance(ServerCharacter->GetPosition(), BeforeInput) > 0.25f,
				"ordinary ActionMap policy reaches authoritative Character movement through the native bridge"
			);

			auto PreviousCharacter = ServerCharacter;
			ServerPlayers[0]->LoadCharacter();
			for (std::uint64_t Tick = 161; Tick <= 190; ++Tick) {
				ClientRuntime->Step();
				ServerRuntime.Step();
				Advance(Network, Server, Client, Tick);
			}
			Check(
				ServerPlayers[0]->GetCharacter().has_value() &&
					*ServerPlayers[0]->GetCharacter() != PreviousCharacter && PreviousCharacter->GetDestroyed(),
				"server Character replacement destroys the old actor and preserves Player.Character as canonical"
			);
			Check(
				Server.GetMetrics().CharacterControlBindings >= 2 &&
					Server.GetMetrics().CharacterControlRevocations >= 1,
				"Character replacement advances through revoke and fresh control-binding lifetimes"
			);
		}

		const auto ClientConnection = Client.GetPrimaryConnection();
		Check(ClientConnection.has_value(), "client retains its generation-safe underlying ConnectionId");
		if (ClientConnection)
			(void)ClientTransport->Disconnect(
				*ClientConnection, {DisconnectReason::LocalShutdown, "session lifecycle test disconnect"}
			);
		for (std::uint64_t Tick = 191; Tick <= 200; ++Tick)
			Advance(Network, Server, Client, Tick);
		Check(
			ServerRuntime.Players->GetPlayers().empty(), "disconnect tears down the authoritative Player and Character"
		);
		Check(Server.GetMetrics().PlayersRemoved == 1, "disconnect Player teardown is measured exactly once");

		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestTwoClientIdentityAndControlIsolation() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto FirstTransport = Network->CreateTransport();
		auto SecondTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto SpatialPlacement = ServerRuntime.Players->PlayerAdded->Connect([](std::shared_ptr<Player> PlayerValue) {
			if (!PlayerValue || !PlayerValue->GetCharacter()) return;
			auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			if (CharacterValue)
				CharacterValue->SetPosition({static_cast<float>(PlayerValue->GetPlayerId() - 1) * 1000.0f, 6.0f, 0.0f});
		});
		GameSession Server(
			ServerTransport, Configuration(GameSessionRole::Server, "two-client-session"), &ServerRuntime
		);
		GameSession First(FirstTransport, Configuration(GameSessionRole::Client, "two-client-session"));
		auto SecondConfiguration = Configuration(GameSessionRole::Client, "two-client-session");
		SecondConfiguration.ClientNonce += 1;
		GameSession Second(SecondTransport, SecondConfiguration);
		Check(
			Server.Start().Succeeded() && First.Start().Succeeded() && Second.Start().Succeeded(),
			"two-client session endpoints start on one server lifetime"
		);

		std::unique_ptr<HeadlessRenderer> FirstRenderer;
		std::unique_ptr<HeadlessRenderer> SecondRenderer;
		std::unique_ptr<Engine> FirstRuntime;
		std::unique_ptr<Engine> SecondRuntime;
		for (std::uint64_t Tick = 1; Tick <= 160 && Server.GetMetrics().ReadyPeers != 2; ++Tick) {
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			if (!FirstRuntime && First.GetClientDataModel()) {
				FirstRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				FirstRuntime = std::make_unique<Engine>(
					First.GetClientDataModel(),
					FirstRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				FirstRuntime->ProcessService->Alive = true;
				Check(First.AttachClientRuntime(*FirstRuntime), "first client attaches to its trusted Player");
			}
			if (!SecondRuntime && Second.GetClientDataModel()) {
				SecondRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				SecondRuntime = std::make_unique<Engine>(
					Second.GetClientDataModel(),
					SecondRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				SecondRuntime->ProcessService->Alive = true;
				Check(Second.AttachClientRuntime(*SecondRuntime), "second client attaches to its trusted Player");
			}
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}

		Check(
			FirstRuntime && SecondRuntime && Server.GetMetrics().ReadyPeers == 2,
			"two clients independently reach gameplay readiness"
		);
		auto FirstLocal = FirstRuntime ? FirstRuntime->Players->GetLocalPlayer() : std::nullopt;
		auto SecondLocal = SecondRuntime ? SecondRuntime->Players->GetLocalPlayer() : std::nullopt;
		Check(
			FirstLocal && SecondLocal && (*FirstLocal)->GetPlayerId() != (*SecondLocal)->GetPlayerId(),
			"each client resolves a distinct LocalPlayer from its explicit accepted Player identity"
		);

		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		std::shared_ptr<KinematicCharacter> FirstCharacter;
		std::shared_ptr<KinematicCharacter> SecondCharacter;
		for (const auto &PlayerValue : ServerPlayers) {
			if (!PlayerValue->GetCharacter()) continue;
			auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			if (FirstLocal && PlayerValue->GetPlayerId() == (*FirstLocal)->GetPlayerId())
				FirstCharacter = CharacterValue;
			if (SecondLocal && PlayerValue->GetPlayerId() == (*SecondLocal)->GetPlayerId())
				SecondCharacter = CharacterValue;
		}
		const auto FirstConnection = First.GetPrimaryConnection();
		for (std::uint64_t Tick = 161; Tick <= 180; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		auto FindClientCharacter = [](Engine *RuntimeValue, std::uint32_t PlayerId) {
			if (!RuntimeValue) return std::shared_ptr<KinematicCharacter>{};
			for (const auto &PlayerValue : RuntimeValue->Players->GetPlayers())
				if (PlayerValue->GetPlayerId() == PlayerId && PlayerValue->GetCharacter())
					return std::dynamic_pointer_cast<KinematicCharacter>(*PlayerValue->GetCharacter());
			return std::shared_ptr<KinematicCharacter>{};
		};
		auto FirstOwnReplica = FirstLocal
								   ? FindClientCharacter(
										 FirstRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId())
									 )
								   : nullptr;
		auto FirstRemoteReplica =
			SecondLocal
				? FindClientCharacter(FirstRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId()))
				: nullptr;
		auto SecondOwnReplica = SecondLocal
									? FindClientCharacter(
										  SecondRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId())
									  )
									: nullptr;
		auto SecondRemoteReplica =
			FirstLocal
				? FindClientCharacter(SecondRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId()))
				: nullptr;
		Check(
			FirstOwnReplica && FirstOwnReplica->GetRootPart() && SecondOwnReplica && SecondOwnReplica->GetRootPart() &&
				FirstRemoteReplica && !FirstRemoteReplica->GetRootPart() && SecondRemoteReplica &&
				!SecondRemoteReplica->GetRootPart(),
			"two peers keep their owner Character pinned while distant remote Character descendants unpublish"
		);
		if (SecondCharacter) SecondCharacter->SetPosition({100.0f, 6.0f, 0.0f});
		for (std::uint64_t Tick = 181; Tick <= 200; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		FirstRemoteReplica = SecondLocal
								 ? FindClientCharacter(
									   FirstRuntime.get(), static_cast<std::uint32_t>((*SecondLocal)->GetPlayerId())
								   )
								 : nullptr;
		SecondRemoteReplica = FirstLocal
								  ? FindClientCharacter(
										SecondRuntime.get(), static_cast<std::uint32_t>((*FirstLocal)->GetPlayerId())
									)
								  : nullptr;
		Check(
			FirstRemoteReplica && FirstRemoteReplica->GetRootPart() && SecondRemoteReplica &&
				SecondRemoteReplica->GetRootPart(),
			"remote Characters reenter both client materialization views at current authoritative position"
		);
		const auto FirstStart = FirstCharacter ? FirstCharacter->GetPosition() : glm::vec3{};
		const auto SecondStart = SecondCharacter ? SecondCharacter->GetPosition() : glm::vec3{};
		if (FirstRuntime)
			(void)FirstRuntime->ProcessEvent(
				KeyEvent{
					.Device = {1},
					.Physical = PhysicalKey::W,
					.Logical = LogicalKey::W,
					.State = ButtonState::Pressed,
				}
			);
		for (std::uint64_t Tick = 201; Tick <= 250; ++Tick) {
			if (FirstRuntime) FirstRuntime->Step();
			if (SecondRuntime) SecondRuntime->Step();
			ServerRuntime.Step();
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			First.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		Check(
			FirstCharacter && HorizontalDistance(FirstCharacter->GetPosition(), FirstStart) > 0.25f,
			"first connection commands its own Player.Character"
		);
		Check(
			SecondCharacter && HorizontalDistance(SecondCharacter->GetPosition(), SecondStart) < 0.05f,
			"first connection cannot move the other Player's Character"
		);

		if (FirstConnection)
			(void)FirstTransport->Disconnect(
				*FirstConnection, {DisconnectReason::LocalShutdown, "two-client isolation disconnect"}
			);
		for (std::uint64_t Tick = 251; Tick <= 265; ++Tick) {
			Network->Pump();
			(void)Server.Poll();
			(void)First.Poll();
			(void)Second.Poll();
			Server.Step(Tick);
			Second.Step(Tick);
			(void)Network->Advance(20ms);
		}
		Check(
			SecondLocal && ServerRuntime.Players->GetPlayers().size() == 1 &&
				ServerRuntime.Players->GetPlayers().front()->GetPlayerId() == (*SecondLocal)->GetPlayerId(),
			"disconnect removes only the associated Player and preserves the other connection lifetime"
		);

		First.Stop();
		Second.Stop();
		Server.Stop();
		if (FirstRuntime) FirstRuntime->Destroy();
		if (SecondRuntime) SecondRuntime->Destroy();
		ServerRuntime.Destroy();
	}

	void TestServerCharacterAutoLoadsPolicy() {
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		auto ServerTransport = Network->CreateTransport();
		auto ClientTransport = Network->CreateTransport();
		auto ServerWorld = std::make_shared<DataModel>();
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(
			ServerWorld,
			&ServerRenderer,
			nullptr,
			EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		ServerRuntime.Players->SetCharacterAutoLoads(false);
		GameSession Server(ServerTransport, Configuration(GameSessionRole::Server, "autoload-policy"), &ServerRuntime);
		GameSession Client(ClientTransport, Configuration(GameSessionRole::Client, "autoload-policy"));
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "CharacterAutoLoads=false session starts");
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		for (std::uint64_t Tick = 1; Tick <= 100 && Server.GetMetrics().ReadyPeers != 1; ++Tick) {
			Advance(Network, Server, Client, Tick);
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
				ClientRuntime = std::make_unique<Engine>(
					Client.GetClientDataModel(),
					ClientRenderer.get(),
					nullptr,
					EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
				);
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "CharacterAutoLoads=false client attaches");
			}
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
		}
		auto ServerPlayers = ServerRuntime.Players->GetPlayers();
		Check(
			ServerPlayers.size() == 1 && !ServerPlayers.front()->GetCharacter() &&
				Server.GetMetrics().CharacterControlBindings == 0,
			"server CharacterAutoLoads=false permits a ready Player without inventing a Character"
		);
		if (!ServerPlayers.empty()) ServerPlayers.front()->LoadCharacter();
		for (std::uint64_t Tick = 101; Tick <= 125; ++Tick) {
			if (ClientRuntime) ClientRuntime->Step();
			ServerRuntime.Step();
			Advance(Network, Server, Client, Tick);
		}
		Check(
			!ServerPlayers.empty() && ServerPlayers.front()->GetCharacter() &&
				Server.GetMetrics().CharacterControlBindings == 1,
			"explicit server LoadCharacter materializes and binds after a Player-only ready phase"
		);
		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		ServerRuntime.Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestProtocolBounds();
		TestPreAcceptanceAndTimeoutRejection();
		TestSpoofedReadyPlayerRejection();
		TestAcceptedConnectionChurn();
		for (int Cycle = 0; Cycle < 100; ++Cycle)
			TestProductionLifecycleComposition();
		TestTwoClientIdentityAndControlIsolation();
		TestServerCharacterAutoLoadsPolicy();
	} catch (const std::exception &Error) {
		std::cerr << "Unexpected game-session test exception: " << Error.what() << '\n';
		++Failures;
	}
	if (Failures == 0) std::cout << "Game session tests passed\n";
	return Failures == 0 ? 0 : 1;
}

#include "gargantuan/Engine.hpp"
#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;
	using Clock = std::chrono::steady_clock;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	bool AttributeEquals(const Instance &Object, std::string_view Name, const WireValue &Value) {
		const auto Actual = Object.GetAttributeValue(Name);
		return Actual && *Actual == Value;
	}

	bool NumberEquals(const Instance &Object, std::string_view Name, int Expected) {
		const auto Actual = Object.GetAttributeValue(Name);
		return Actual && (*Actual == WireValue(Expected) || *Actual == WireValue(static_cast<double>(Expected)));
	}

	void VerifyStatus(const GameSession &Session) {
		if (Session.GetStatus() == GameSessionStatus::Failed)
			throw std::runtime_error(Session.GetFailure());
	}

	struct Client {
		std::unique_ptr<HeadlessRenderer> Renderer;
		std::unique_ptr<Engine> Runtime;
		std::unique_ptr<GameSession> Session;

		void Stop() {
			if (Session) Session->Stop();
			if (Runtime) Runtime->Destroy();
			Session.reset();
			Runtime.reset();
		}
	};

	void RunRelayRescue(bool UseGns) {
		auto Network = UseGns ? nullptr : SimulatedNetwork::Create({.BaseLatency = 6ms});
		const auto CreateTransport = [&]() -> std::shared_ptr<IGameTransport> {
			if (Network) return Network->CreateTransport();
#if defined(GARGANTUAN_WITH_GNS)
			return std::make_shared<GameNetworkingSocketsTransport>();
#else
			throw std::runtime_error("Relay Rescue native transport requires GNS");
#endif
		};

		DiskFilesystem Filesystem(GARGANTUAN_GAMEPLAY_SAMPLE_ROOT);
		auto Sample = Project::fromExisting(&Filesystem);
		auto TrustedWorld = Sample.DeserializeGame();
		auto ServerWorld = Sample.DeserializeGame();
		ServerWorld->InitializeLoadedProjectRevision();
		auto Assets = std::dynamic_pointer_cast<AssetService>(TrustedWorld->GetService("AssetService"));
		Require(Assets != nullptr, "sample asset catalog loads");
		const auto AssetSnapshot = Assets->CaptureRuntimeAssets();
		std::size_t ScriptErrors = 0;
		const auto ReportError = [&](std::string Code, std::string Message) {
			if (Code != "Error") return;
			++ScriptErrors;
			std::cerr << "[Gameplay:Relay] " << Message << '\n';
		};
		HeadlessRenderer ServerRenderer(Vector2(320, 240));
		Engine ServerRuntime(ServerWorld, &ServerRenderer, ReportError,
			{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		ServerRuntime.ProcessService->Alive = true;
		GameSessionConfiguration Configuration{
			.Role = GameSessionRole::Server,
			.Endpoint = {UseGns ? "127.0.0.1" : "relay-rescue", 37940},
			.Limits = GameSessionConfiguration::DefaultLimits(),
			.HandshakeTimeoutTicks = 180,
			.AllowInsecureDevelopmentNetwork = !UseGns,
		};
		std::unique_ptr<GameSession> Server;
		for (; Configuration.Endpoint.Port < 37956; ++Configuration.Endpoint.Port) {
			Server = std::make_unique<GameSession>(CreateTransport(), Configuration, &ServerRuntime);
			if (Server->Start().Succeeded()) break;
			Server.reset();
		}
		Require(Server != nullptr, "relay server accepts the session");
		Configuration.Role = GameSessionRole::Client;
		std::array<Client, 2> Clients;
		for (std::size_t Index = 0; Index < Clients.size(); ++Index) {
			Configuration.ClientNonce = 0x52454c415900ull + Index;
			Clients[Index].Session = std::make_unique<GameSession>(CreateTransport(), Configuration);
			Require(Clients[Index].Session->Start().Succeeded(), "relay client connects");
		}

		const auto Frame = [&] {
			std::this_thread::sleep_for(17ms);
			if (Network) {
				(void)Network->Advance(17ms);
				Network->Pump();
			}
			(void)Server->Poll();
			ServerRuntime.Step();
			Server->Step(ServerRuntime.GetSimulationTick());
			VerifyStatus(*Server);
			for (auto &Peer : Clients) {
				if (!Peer.Session) continue;
				(void)Peer.Session->Poll();
				if (!Peer.Runtime) {
					if (const auto World = Peer.Session->GetClientDataModel()) {
						auto ClientAssets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
						Require(ClientAssets != nullptr, "replica asset service is available");
						ClientAssets->LoadRuntimeAssetSnapshot(AssetSnapshot);
						Require(PackageBuilder::HydrateClientCode(TrustedWorld, World) == 1,
							"replica hydrates the saved RelayClient script");
						Peer.Renderer = std::make_unique<HeadlessRenderer>(Vector2(320, 240));
						Peer.Runtime = std::make_unique<Engine>(World, Peer.Renderer.get(), ReportError,
							EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient});
						Peer.Runtime->ProcessService->Alive = true;
						Require(Peer.Session->AttachClientRuntime(*Peer.Runtime),
							"relay client attaches to its trusted Player");
					}
				}
				if (Peer.Runtime) Peer.Runtime->Step();
				Peer.Session->Step(ServerRuntime.GetSimulationTick());
				VerifyStatus(*Peer.Session);
			}
			Require(ScriptErrors == 0, "sample code runs without script activation errors");
		};

		const auto Until = [&](int Frames, std::string_view Message, const auto &Predicate) {
			for (int Index = 0; Index < Frames && !Predicate(); ++Index) Frame();
			Require(Predicate(), Message);
		};
		const auto SetKey = [](Client &Peer, PhysicalKey Physical, LogicalKey Logical, ButtonState State) {
			(void)Peer.Runtime->ProcessEvent(KeyEvent{
				.Device = {1}, .Physical = Physical, .Logical = Logical, .State = State,
			});
		};
		const auto WaitForRelease = [&] {
			for (int Index = 0; Index < 5; ++Index) Frame();
		};

		Until(180, "both peers hydrate distinct stock Characters and the beacon animation", [&] {
			for (const auto &Peer : Clients) {
				if (!Peer.Runtime || Peer.Session->GetStatus() != GameSessionStatus::Ready ||
					Peer.Runtime->Animation->GetMetrics().ActiveTracks == 0)
					return false;
				auto PlayerValue = Peer.Runtime->Players->GetLocalPlayer().value_or(nullptr);
				if (!PlayerValue || !PlayerValue->GetCharacter() || !NumberEquals(*PlayerValue, "Health", 100))
					return false;
			}
			return Server->GetMetrics().ReadyPeers == 2;
		});
		const auto FindServerPlayer = [&](const Client &Peer) -> std::shared_ptr<Player> {
			const auto Id = (*Peer.Runtime->Players->GetLocalPlayer())->GetPlayerId();
			for (const auto &Value : ServerRuntime.Players->GetPlayers())
				if (Value->GetPlayerId() == Id) return Value;
			return nullptr;
		};
		auto &PilotClient = Clients.front();
		const auto Pilot = FindServerPlayer(PilotClient);
		const auto SwitchOperator = FindServerPlayer(Clients.back());
		Require(Pilot && SwitchOperator && Pilot != SwitchOperator && Pilot->GetCharacter() &&
				SwitchOperator->GetCharacter(), "stock Characters belong to distinct server Players");
		const auto OldPilotCharacter = *Pilot->GetCharacter();
		const auto Course = ServerRuntime.Workspace->FindFirstChild("RelayCourse", std::nullopt);
		Require(Course != nullptr, "saved RelayCourse is present");
		const auto PartAt = [&](std::string Name) {
			auto Value = std::dynamic_pointer_cast<Part>(Course->FindFirstChild(std::move(Name), std::nullopt));
			Require(Value != nullptr, "saved station is a Part");
			return Value->GetPosition() + glm::vec3(0, 2.0f, 0);
		};
		const auto PlaceAt = [&](const std::shared_ptr<Player> &Value, std::string Name) {
			const auto CharacterValue = Value->GetCharacter();
			Require(CharacterValue.has_value(), "placement requires a stock Character");
			(*CharacterValue)->SetCFrame(CFrame(PartAt(std::move(Name))));
		};
		PlaceAt(SwitchOperator, "GroundSwitch");
		PlaceAt(Pilot, "LaunchPad");
		// Only fixture positioning above bypasses local input. All flight and
		// landing below enter through host events and the saved RelayClient.
		const auto FlightReady = [&] {
			const auto LocalPilot = *PilotClient.Runtime->Players->GetLocalPlayer();
			return Pilot->GetPossessedEntity() && LocalPilot->GetPossessedEntity() &&
				!Pilot->GetCharacter() && OldPilotCharacter->GetDestroyed() &&
				SwitchOperator->GetCharacter() && AttributeEquals(*Course, "RelayEnabled", WireValue(true));
		};
		for (int Index = 0; Index < 90 && !FlightReady(); ++Index) Frame();
		if (!FlightReady()) {
			const auto LocalPilot = *PilotClient.Runtime->Players->GetLocalPlayer();
			const auto PilotPosition = Pilot->GetCharacter() ? (*Pilot->GetCharacter())->GetPosition() : glm::vec3{};
			const auto SwitchPosition = SwitchOperator->GetCharacter()
				? (*SwitchOperator->GetCharacter())->GetPosition() : glm::vec3{};
			std::cerr << "[Gameplay:Relay] launch server_possessed=" << bool(Pilot->GetPossessedEntity())
				<< " client_possessed=" << bool(LocalPilot->GetPossessedEntity())
				<< " stock_retired=" << OldPilotCharacter->GetDestroyed()
				<< " switch=" << AttributeEquals(*Course, "RelayEnabled", WireValue(true))
				<< " multiplayer=" << AttributeEquals(*Course, "RelayMultiplayer", WireValue(true))
				<< " pilot_pos=" << PilotPosition.x << ',' << PilotPosition.y << ',' << PilotPosition.z
				<< " switch_pos=" << SwitchPosition.x << ',' << SwitchPosition.y << ',' << SwitchPosition.z
				<< " leases=" << Server->GetMetrics().PossessionInput.Bindings << '\n';
		}
		Require(FlightReady(), "pilot enters the sample drone and receives its replica");
		const auto Drone = std::dynamic_pointer_cast<Part>(*Pilot->GetPossessedEntity());
		Require(Drone && Drone->GetAnchored(), "sample uses an independently possessed anchored drone");
		const auto Drive = [&](PhysicalKey Physical, LogicalKey Logical, int Frames, const auto &Predicate) {
			SetKey(PilotClient, Physical, Logical, ButtonState::Pressed);
			Until(Frames, "pilot intent failed to reach its terrain or relay target", Predicate);
			SetKey(PilotClient, Physical, Logical, ButtonState::Released);
			WaitForRelease();
		};
		Drive(PhysicalKey::D, LogicalKey::D, 100, [&] { return Drone->GetPosition().x >= -0.7f; });
		SetKey(PilotClient, PhysicalKey::W, LogicalKey::W, ButtonState::Pressed);
		Until(60, "northbound drone reaches the divider", [&] { return Drone->GetPosition().z <= -8.0f; });
		for (int Index = 0; Index < 12; ++Index) Frame();
		Require(Drone->GetPosition().z >= -8.25f, "the saved drone controller casts against the divider");
		SetKey(PilotClient, PhysicalKey::W, LogicalKey::W, ButtonState::Released);
		WaitForRelease();
		Drive(PhysicalKey::D, LogicalKey::D, 100, [&] { return Drone->GetPosition().x >= 6.5f; });
		for (int Charge = 1; Charge <= 3; ++Charge) {
			if (Charge > 1)
				Drive(PhysicalKey::S, LogicalKey::S, 50, [&] { return Drone->GetPosition().z >= -12.65f; });
			Drive(PhysicalKey::W, LogicalKey::W, 80, [&] { return NumberEquals(*Course, "RelayCharges", Charge); });
			Require(NumberEquals(*Course, "RelayCharges", Charge), "one charge is awarded per visit");
		}
		const auto Inventory = Pilot->FindFirstChild("RelayInventory", std::nullopt);
		Require(Inventory && Inventory->GetChildren().size() == 3 && NumberEquals(*Pilot, "RelayCharges", 3) &&
			NumberEquals(*SwitchOperator, "RelayCharges", 0), "only the pilot owns each earned cell");
		for (const auto &Cell : Inventory->GetChildren())
			Require(AttributeEquals(*Cell, "Kind", WireValue(std::string("RelayCell"))), "a charge creates a relay cell");
		Until(60, "other client receives authoritative mission completion", [&] {
			const auto &World = Clients.back().Runtime->Workspace;
			const auto Replica = World->FindFirstChild("RelayCourse", std::nullopt);
			return Replica && NumberEquals(*Replica, "RelayCharges", 3);
		});

		SetKey(PilotClient, PhysicalKey::Space, LogicalKey::Space, ButtonState::Pressed);
		Until(90, "landing intent returns the pilot to the stock controller", [&] {
			return Drone->GetDestroyed() && !Pilot->GetPossessedEntity() && Pilot->GetCharacter();
		});
		SetKey(PilotClient, PhysicalKey::Space, LogicalKey::Space, ButtonState::Released);
		const auto BeforeDeath = *Pilot->GetCharacter();
		PlaceAt(Pilot, "Hazard");
		Until(120, "server damage retires the stock Character", [&] {
			return BeforeDeath->GetDestroyed() && !Pilot->GetCharacter() && NumberEquals(*Pilot, "Health", 0);
		});
		const auto DeathTime = Clock::now();
		const auto DeathTick = ServerRuntime.GetSimulationTick();
		Until(180, "sample respawns the same Player with a new Character and full health", [&] {
			return Pilot->GetCharacter() && *Pilot->GetCharacter() != BeforeDeath && NumberEquals(*Pilot, "Health", 100);
		});
		const auto RespawnMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - DeathTime);
		std::cout << "[Gameplay:Relay] transport=" << (UseGns ? "gns-loopback" : "simulated")
			<< " respawn_wall_ms=" << RespawnMilliseconds.count()
			<< " respawn_server_ticks=" << ServerRuntime.GetSimulationTick() - DeathTick << '\n';

		PlaceAt(Pilot, "LaunchPad");
		Until(75, "respawned pilot can launch a new drone", [&] { return Pilot->GetPossessedEntity().has_value(); });
		const auto DisconnectDrone = *Pilot->GetPossessedEntity();
		PilotClient.Stop();
		Until(150, "disconnect retires pilot, drone, and its possession while another player stays", [&] {
			return Pilot->GetDestroyed() && DisconnectDrone->GetDestroyed() && !Pilot->GetPossessedEntity() &&
				ServerRuntime.Players->GetPlayers() == std::vector<std::shared_ptr<Player>>{SwitchOperator};
		});
		Require(Inventory->GetDestroyed(), "disconnect also retires player-owned inventory");

		Clients.back().Stop();
		Server->Stop();
		ServerRuntime.Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		RunRelayRescue(ArgumentCount > 1 && std::string_view(Arguments[1]) == "--gns");
		std::cout << "[Gameplay:Relay] Saved two-player mission passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Gameplay:Relay] " << Error.what() << '\n';
		return 1;
	}
}

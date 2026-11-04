#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/ProcessService.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;
	const auto Root = std::filesystem::temp_directory_path() /
					  ("gargantuan-storage-session-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
			SDL_Quit();
		}
	} CleanupValue{Root};
	try {
		if (!SDL_Init(0)) throw std::runtime_error("SDL initialization failed");
		std::filesystem::create_directories(Root / ".gargantuan");
		std::ofstream Source(Root / ".gargantuan" / "prerun.luau");
		Source << R"(Schema:RegisterClass({Namespace="Game",Name="SharedRules",Version=1,Base="Engine.Folder",
Service=true,Replicated=true,Properties={}})
Schema:RegisterClass({Namespace="Game",Name="PrivateRules",Version=1,Base="Engine.Folder",Service=true,Properties={}}))";
		Source.close();
		BootstrapProjectRuntimeSchema(Root);
		std::size_t Checks = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		auto World = std::make_shared<DataModel>();
		World->MarkPersistenceSubtreeArchivable();
		World->InitializeLoadedProjectRevision();
		auto Shared = std::make_shared<ModuleScript>();
		Shared->SetName("SessionRules");
		Shared->SetSource("return { Revision = 1 }");
		Shared->SetParent(World->GetService("Game.SharedRules"));
		auto Secret = std::make_shared<ModuleScript>();
		Secret->SetName("PrivateSessionRules");
		Secret->SetSource("return 'private'");
		Secret->SetParent(World->GetService("Game.PrivateRules"));
		auto Network = SimulatedNetwork::Create({.BaseLatency = 1ms});
		HeadlessRenderer ServerRenderer(Vector2(64, 64));
		Engine ServerRuntime(
			World, &ServerRenderer, nullptr, {.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer}
		);
		ServerRuntime.ProcessService->Alive = true;
		auto Configuration = [](GameSessionRole Role, std::uint64_t Nonce) {
			return GameSessionConfiguration{
				.Role = Role,
				.Endpoint = {"storage-session", 27020},
				.Limits = GameSessionConfiguration::DefaultLimits(),
				.HandshakeTimeoutTicks = 120,
				.ClientNonce = Nonce,
				.AllowInsecureDevelopmentNetwork = true
			};
		};
		GameSession Server(Network->CreateTransport(), Configuration(GameSessionRole::Server, 0), &ServerRuntime);
		Check(Server.Start().Succeeded(), "Production server session starts");
		auto ReadyCount = [&] {
			return std::ranges::count_if(Server.GetPeerIdentities(), [](const auto &Peer) { return Peer.Ready; });
		};
		struct ClientState {
			std::unique_ptr<HeadlessRenderer> Renderer;
			std::unique_ptr<Engine> Runtime;
			std::unique_ptr<GameSession> Session;
		};
		std::vector<ClientState> Clients;
		auto AddClient = [&](std::uint64_t Nonce) {
			ClientState State;
			State.Session = std::make_unique<GameSession>(
				Network->CreateTransport(), Configuration(GameSessionRole::Client, Nonce)
			);
			Check(State.Session->Start().Succeeded(), "Production client session starts");
			Clients.push_back(std::move(State));
		};
		std::uint64_t Tick = 0;
		auto Step = [&] {
			(void)Network->Advance(2ms);
			Network->Pump();
			(void)Server.Poll();
			for (auto &Client : Clients)
				if (Client.Session->GetStatus() != GameSessionStatus::Closed) (void)Client.Session->Poll();
			Server.Step(++Tick);
			for (auto &Client : Clients) {
				if (Client.Session->GetStatus() == GameSessionStatus::Closed) continue;
				Client.Session->Step(Tick);
				if (!Client.Runtime && Client.Session->GetClientDataModel()) {
					Client.Renderer = std::make_unique<HeadlessRenderer>(Vector2(64, 64));
					Client.Runtime = std::make_unique<Engine>(
						Client.Session->GetClientDataModel(),
						Client.Renderer.get(),
						nullptr,
						EngineProviderConfiguration{.AudioEnabled = false, .Mode = RuntimeMode::NetworkClient}
					);
					Client.Runtime->ProcessService->Alive = true;
					Check(
						Client.Session->AttachClientRuntime(*Client.Runtime),
						"Client attaches through canonical Ready lifecycle"
					);
				}
				if (Client.Runtime) Client.Runtime->Step();
			}
			ServerRuntime.Step();
			(void)Network->Advance(20ms);
		};
		auto HasSource = [&](std::size_t Index) {
			auto ClientWorld = Clients[Index].Session->GetClientDataModel();
			auto Module = ClientWorld ? std::dynamic_pointer_cast<ModuleScript>(
											ClientWorld->FindFirstDescendant("SessionRules")
										)
									  : nullptr;
			return Module && Module->GetSource() == Shared->GetSource() &&
				   Module->GetSourceVersion() == Shared->GetSourceVersion() &&
				   !ClientWorld->FindFirstDescendant("PrivateSessionRules");
		};
		auto Until = [&](auto Predicate) {
			for (int Attempt = 0; Attempt < 240 && !Predicate(); ++Attempt)
				Step();
			return Predicate();
		};
		AddClient(11);
		Check(
			Until([&] { return ReadyCount() == 1 && HasSource(0); }),
			"First actual client receives shared module and excludes private contents"
		);
		AddClient(12);
		Check(
			Until([&] { return ReadyCount() == 2 && HasSource(0) && HasSource(1); }),
			"Late actual client receives a consistent production baseline"
		);
		Shared->SetSource("return { Revision = 2 }");
		Check(
			Until([&] { return HasSource(0) && HasSource(1); }),
			"Server-authored module source updates reach both clients through production planning"
		);
		auto OldRoot = Clients[0].Session->GetClientDataModel();
		Clients[0].Session->Stop();
		if (Clients[0].Runtime) Clients[0].Runtime->Destroy();
		Check(
			Until([&] { return ReadyCount() == 1; }) && !Clients[0].Session->GetClientDataModel(),
			"Disconnect releases the exposed replica and peer lifecycle"
		);
		AddClient(13);
		Check(
			Until([&] { return ReadyCount() == 2 && HasSource(2); }) &&
				Clients[2].Session->GetClientDataModel() != OldRoot,
			"Reconnect establishes a fresh world and current source"
		);
		for (auto &Client : Clients) {
			Client.Session->Stop();
			if (Client.Runtime) Client.Runtime->Destroy();
		}
		Server.Stop();
		ServerRuntime.Destroy();
		Check(Server.GetPeerIdentities().empty(), "Terminal production session releases all ready peers");
		std::cout << "[Storage:Session] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Storage:Session] FAIL " << Error.what() << '\n';
		return 1;
	}
}

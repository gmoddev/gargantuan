#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "platform/sdl/SDLHost.hpp"

#include <SDL3/SDL.h>
#include <argparse/argparse.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif

#if defined(_WIN32)
#include <Windows.h>
#endif

using namespace gargantuan;

namespace {
	std::optional<network::TransportEndpoint> ParseEndpoint(const std::string &Value) {
		if (Value.empty() || Value.size() > network::MaximumTransportEndpointBytes) return std::nullopt;
		std::string Host;
		std::string PortText;
		if (Value.front() == '[') {
			const auto End = Value.find(']');
			if (End == std::string::npos || End + 2 > Value.size() || Value[End + 1] != ':') return std::nullopt;
			Host = Value.substr(1, End - 1);
			PortText = Value.substr(End + 2);
		} else {
			const auto Separator = Value.rfind(':');
			if (Separator == std::string::npos) return std::nullopt;
			Host = Value.substr(0, Separator);
			PortText = Value.substr(Separator + 1);
		}
		if (Host.empty() || PortText.empty()) return std::nullopt;
		std::uint32_t Port = 0;
		for (const auto Character : PortText) {
			if (Character < '0' || Character > '9' || Port > 6553) return std::nullopt;
			Port = Port * 10 + static_cast<std::uint32_t>(Character - '0');
		}
		network::TransportEndpoint Result{std::move(Host), static_cast<std::uint16_t>(Port)};
		return Result.IsValid() ? std::optional(std::move(Result)) : std::nullopt;
	}

#if defined(_WIN32)
	void ValidateNativeRuntimeClosure(const std::filesystem::path &PackageRoot) {
		const auto CanonicalRoot = std::filesystem::weakly_canonical(PackageRoot);
		for (const auto *ModuleName : {
				 L"SDL3.dll",
				 L"MSVCP140.dll",
				 L"VCRUNTIME140.dll",
				 L"VCRUNTIME140_1.dll",
			 }) {
			const auto Module = GetModuleHandleW(ModuleName);
			std::wstring Buffer(32'768, L'\0');
			const auto Length = Module ? GetModuleFileNameW(Module, Buffer.data(), static_cast<DWORD>(Buffer.size()))
									   : 0;
			if (Length == 0 || Length == Buffer.size())
				throw std::runtime_error("A required native runtime module is unavailable");
			Buffer.resize(Length);
			if (std::filesystem::weakly_canonical(Buffer).parent_path() != CanonicalRoot)
				throw std::runtime_error("A required native runtime module was not loaded from the package root");
		}
	}
#else
	void ValidateNativeRuntimeClosure(const std::filesystem::path &) {}
#endif
}

int main(int argc, char *argv[]) {
	argparse::ArgumentParser Program("GargantuanPlayer");
	Program.add_description("Gargantuan standalone game runtime");
	Program.add_argument("--headless").flag().help("disable the graphical renderer");
	Program.add_argument("--startup-smoke").flag().help("exit after a bounded runtime startup smoke");
	Program.add_argument("--session-smoke").flag().help("require a bounded packaged game-session acceptance proof");
	Program.add_argument("--max-frames").scan<'i', int>().default_value(0).help("bounded test-only frame count");
	Program.add_argument("--server-bind").default_value(std::string()).help("host:port for a network game server");
	Program.add_argument("--connect").default_value(std::string()).help("host:port for a network game client");
	Program.add_argument("--allow-insecure-development-network")
		.flag()
		.help("allow DevelopmentLocal networking beyond loopback; authentication is not provided");
	try {
		Program.parse_args(argc, argv);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer arguments are invalid.\n";
		return 2;
	}

	const auto ServerText = Program.get<std::string>("--server-bind");
	const auto ClientText = Program.get<std::string>("--connect");
	const bool AllowInsecureDevelopmentNetwork = Program.is_used("--allow-insecure-development-network");
	const auto ServerEndpoint = ServerText.empty() ? std::nullopt : ParseEndpoint(ServerText);
	const auto ClientEndpoint = ClientText.empty() ? std::nullopt : ParseEndpoint(ClientText);
	if ((!ServerText.empty() && !ClientText.empty()) || (!ServerText.empty() && !ParseEndpoint(ServerText)) ||
		(!ClientText.empty() && !ParseEndpoint(ClientText)) ||
		(ServerEndpoint && !network::IsLoopbackTransportEndpoint(*ServerEndpoint) &&
		 !AllowInsecureDevelopmentNetwork) ||
		(ClientEndpoint && !network::IsLoopbackTransportEndpoint(*ClientEndpoint) &&
		 !AllowInsecureDevelopmentNetwork) ||
		(Program.is_used("--session-smoke") && ServerText.empty() && ClientText.empty())) {
		std::cerr << "GargantuanPlayer network endpoint arguments are invalid.\n";
		return 2;
	}
	if (AllowInsecureDevelopmentNetwork && (ServerEndpoint || ClientEndpoint))
		std::cerr << "[Network:Security] DevelopmentLocal networking is exposed without peer authentication.\n";
#if !defined(GARGANTUAN_WITH_GNS)
	if (!ServerText.empty() || !ClientText.empty()) {
		std::cerr << "GargantuanPlayer was built without production game transport support.\n";
		return 2;
	}
#endif

	const auto PackageRoot = Paths::GetExecutableDirectory().lexically_normal();
	std::vector<PackageDiagnostic> Diagnostics;
	auto Payload = PackageBuilder::Load(PackageRoot, Diagnostics);
	if (!Payload) {
		std::cerr << "GargantuanPlayer could not validate this game package.\n";
		for (const auto &Diagnostic : Diagnostics)
			if (Diagnostic.Severity == PackageDiagnosticSeverity::Error)
				std::cerr << "[Package:" << Diagnostic.Category << "] " << Diagnostic.Message << '\n';
		return 3;
	}
	try {
		ValidateNativeRuntimeClosure(PackageRoot);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer could not establish its packaged native runtime closure.\n";
		return 4;
	}

	try {
		BootstrapPackagedRuntimeSchema(Payload->PreRunSource);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer could not initialize the packaged runtime schema.\n";
		return 5;
	}

	const bool Headless = Program.is_used("--headless");
	if (!SDL_Init(Headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO)) {
		std::cerr << "GargantuanPlayer could not initialize the platform runtime.\n";
		return 6;
	}
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;

	std::unique_ptr<BaseRenderer> Renderer;
	std::unique_ptr<Engine> Runtime;
	std::unique_ptr<network::GameSession> Session;
	try {
		auto PackagedWorld = PackageBuilder::LoadWorld(*Payload, PackageRoot);
		const Vector2 ViewportSize(720, 540);
		if (Headless)
			Renderer = std::make_unique<HeadlessRenderer>(ViewportSize);
		else
			Renderer = std::make_unique<SDLRenderer>(ViewportSize);
		std::shared_ptr<DataModel> World;
#if defined(GARGANTUAN_WITH_GNS)
		if (!ClientText.empty()) {
			Session = std::make_unique<network::GameSession>(
				std::make_shared<network::GameNetworkingSocketsTransport>(),
				network::GameSessionConfiguration{
					.Role = network::GameSessionRole::Client,
					.Endpoint = *ParseEndpoint(ClientText),
					.Limits = network::GameSessionConfiguration::DefaultLimits(),
					.AllowInsecureDevelopmentNetwork = AllowInsecureDevelopmentNetwork,
				}
			);
			if (!Session->Start().Succeeded()) throw std::runtime_error("client game-session endpoint failed to start");
			SDLHost BootstrapHost;
			for (std::uint64_t Tick = 1; Tick <= network::DefaultGameSessionHandshakeTimeoutTicks; ++Tick) {
				HostEvent Event;
				while (BootstrapHost.PollEvent(Event)) {
					if (std::holds_alternative<WindowCloseEvent>(Event))
						throw std::runtime_error("client startup was cancelled");
				}
				(void)Session->Poll();
				Session->Step(Tick);
				World = Session->GetClientDataModel();
				if (World) break;
				if (Session->GetStatus() == network::GameSessionStatus::Failed)
					throw std::runtime_error(Session->GetFailure());
				SDL_Delay(16);
			}
			if (!World) throw std::runtime_error("client game-session bootstrap timed out");
			auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
			if (!Assets) throw std::runtime_error("replicated AssetService is unavailable");
			Assets->LoadRuntimeAssetSnapshot(Payload->Assets);
			const auto HydratedCode = PackageBuilder::HydrateClientCode(PackagedWorld, World);
			LOG_INFO(App, "[Package:Runtime] Hydrated %zu trusted client code containers", HydratedCode);
			PackagedWorld->Destroy();
			PackagedWorld.reset();
		} else
#endif
		{
			World = std::move(PackagedWorld);
		}
		const auto Mode = !ServerText.empty()
							  ? RuntimeMode::NetworkServer
							  : (!ClientText.empty() ? RuntimeMode::NetworkClient : RuntimeMode::Offline);
		Runtime = std::make_unique<Engine>(
			World,
			Renderer.get(),
			[](std::string Code, std::string Message) {
				if (Code == "Information") return;
				std::cerr << "[Runtime:Diagnostic] [" << Code << "] " << Message << '\n';
			},
			EngineProviderConfiguration{.AudioEnabled = !Headless, .Mode = Mode}
		);
#if defined(GARGANTUAN_WITH_GNS)
		if (!ServerText.empty()) {
			Session = std::make_unique<network::GameSession>(
				std::make_shared<network::GameNetworkingSocketsTransport>(),
				network::GameSessionConfiguration{
					.Role = network::GameSessionRole::Server,
					.Endpoint = *ParseEndpoint(ServerText),
					.Limits = network::GameSessionConfiguration::DefaultLimits(),
					.AllowInsecureDevelopmentNetwork = AllowInsecureDevelopmentNetwork,
				},
				Runtime.get()
			);
			if (!Session->Start().Succeeded()) throw std::runtime_error("server game-session endpoint failed to start");
		} else if (!ClientText.empty() && !Session->AttachClientRuntime(*Runtime)) {
			throw std::runtime_error("client game-session runtime bridge failed to attach");
		}
#endif
		Runtime->ProcessService->Alive = true;
		const auto UserDataRoot = GetPackageUserDataRoot(Payload->Inspection.Identity);
		LOG_INFO(
			App,
			"[Package:Runtime] Started %s (%s); user data is isolated by ProjectId",
			Payload->Inspection.DisplayName.c_str(),
			Payload->Inspection.Identity.ToString().c_str()
		);
		(void)UserDataRoot;

		SDLHost Host;
		const auto RequestedFrames = Program.get<int>("--max-frames");
		const auto MaximumFrames = RequestedFrames > 0 ? RequestedFrames
													   : (Program.is_used("--startup-smoke") ? 12 : 0);
		const bool SessionSmoke = Program.is_used("--session-smoke");
		bool SessionMovementInjected = false;
		int Frames = 0;
		auto NetworkFrameDeadline = std::chrono::steady_clock::now();
		while (Runtime->ProcessService->Alive) {
			if (Session) (void)Session->Poll();
			HostEvent Event;
			while (Host.PollEvent(Event)) {
				auto Result = Runtime->ProcessEvent(Event);
				if (Result.Command) Host.Apply(*Result.Command);
				if (!Runtime->ProcessService->Alive) break;
			}
			if (!Runtime->ProcessService->Alive) break;
			if (SessionSmoke && !ClientText.empty() && !SessionMovementInjected) {
				if (auto LocalPlayer = Runtime->Players->GetLocalPlayer();
					LocalPlayer && (*LocalPlayer)->GetCharacter()) {
					(void)Runtime->ProcessEvent(
						KeyEvent{
							.Device = {1},
							.Physical = PhysicalKey::W,
							.Logical = LogicalKey::W,
							.State = ButtonState::Pressed,
						}
					);
					SessionMovementInjected = true;
				}
			}
			Runtime->Step();
			if (Session) {
				Session->Step(Runtime->GetSimulationTick());
				if (Session->GetStatus() == network::GameSessionStatus::Failed)
					throw std::runtime_error(Session->GetFailure());
				if (SessionSmoke) {
					const auto Metrics = Session->GetMetrics();
					const bool CameraReady = Headless || Runtime->Workspace->GetCurrentCamera()->GetCameraType() ==
															 Enums::CameraType::Scriptable;
					const bool Satisfied =
						Session->GetRole() == network::GameSessionRole::Server
							? Metrics.PlayersRemoved >= 1 && Runtime->Players->GetPlayers().empty()
							: Runtime->CharacterControl->GetAttributeValue("SessionSmokeComplete").has_value() &&
								  Metrics.ActionsPresented >= 1 && Metrics.ActionPresentationStops >= 1 && CameraReady;
					if (Satisfied) Runtime->ProcessService->MarkExit(0);
				}
			}
			if (MaximumFrames > 0 && ++Frames >= MaximumFrames) {
				int ExitCode = SessionSmoke ? 8 : 0;
				if (SessionSmoke && !ClientText.empty()) {
					if (!Runtime->Players->GetLocalPlayer())
						ExitCode = 10;
					else if (!Runtime->Players->GetLocalPlayer().value()->GetCharacter())
						ExitCode = 11;
					else if (auto Value = Runtime->CharacterControl->GetAttributeValue("SessionSmokeStep"))
						if (const auto *Step = std::get_if<int>(&*Value))
							ExitCode = 20 + *Step;
						else if (const auto *Step = std::get_if<double>(&*Value))
							ExitCode = 20 + static_cast<int>(*Step);
						else
							ExitCode = 13;
					else
						ExitCode = 12;
					if (ExitCode != 0)
						if (const auto LocalPlayer = Runtime->Players->GetLocalPlayer();
							LocalPlayer && (*LocalPlayer)->GetCharacter()) {
							const auto Position = (*LocalPlayer)->GetCharacter().value()->GetPosition();
							std::cerr << "[Package:Session] client proof stopped at Character position " << Position.x
									  << ", " << Position.y << ", " << Position.z << " with CameraType "
									  << static_cast<int>(Runtime->Workspace->GetCurrentCamera()->GetCameraType())
									  << '\n';
						}
				}
				Runtime->ProcessService->MarkExit(ExitCode);
			}
			if (!ServerText.empty() || !ClientText.empty()) {
				NetworkFrameDeadline += std::chrono::microseconds(16'667);
				const auto Now = std::chrono::steady_clock::now();
				if (NetworkFrameDeadline > Now)
					std::this_thread::sleep_until(NetworkFrameDeadline);
				else if (Now - NetworkFrameDeadline > std::chrono::milliseconds(250))
					NetworkFrameDeadline = Now;
			}
		}
		const auto ExitCode = Runtime->ProcessService->ExitCode;
		if (Session) {
			const auto Metrics = Session->GetMetrics();
			LOG_INFO(
				App,
				"[Network:Session] role=%s accepted=%llu ready=%llu playersCreated=%llu playersRemoved=%llu "
				"controlBindings=%llu controlRevocations=%llu actionsPresented=%llu actionStops=%llu",
				Session->GetRole() == network::GameSessionRole::Server ? "server" : "client",
				static_cast<unsigned long long>(Metrics.AcceptedPeers),
				static_cast<unsigned long long>(Metrics.ReadyPeers),
				static_cast<unsigned long long>(Metrics.PlayersCreated),
				static_cast<unsigned long long>(Metrics.PlayersRemoved),
				static_cast<unsigned long long>(Metrics.CharacterControlBindings),
				static_cast<unsigned long long>(Metrics.CharacterControlRevocations),
				static_cast<unsigned long long>(Metrics.ActionsPresented),
				static_cast<unsigned long long>(Metrics.ActionPresentationStops)
			);
			Session->Stop();
		}
		Session.reset();
		Runtime->Destroy();
		Runtime.reset();
		Renderer.reset();
		return ExitCode;
	} catch (const std::exception &Error) {
		std::cerr << "GargantuanPlayer stopped because packaged runtime startup failed: " << Error.what() << "\n";
		if (Session) Session->Stop();
		Session.reset();
		if (Runtime) Runtime->Destroy();
		Runtime.reset();
		Renderer.reset();
		return 7;
	}
}

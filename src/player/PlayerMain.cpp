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
	Program.add_argument("--max-frames").scan<'i', int>().default_value(0).help("bounded test-only frame count");
	Program.add_argument("--server-bind").default_value(std::string()).help("host:port for a network game server");
	Program.add_argument("--connect").default_value(std::string()).help("host:port for a network game client");
	try {
		Program.parse_args(argc, argv);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer arguments are invalid.\n";
		return 2;
	}

	const auto ServerText = Program.get<std::string>("--server-bind");
	const auto ClientText = Program.get<std::string>("--connect");
	if ((!ServerText.empty() && !ClientText.empty()) || (!ServerText.empty() && !ParseEndpoint(ServerText)) ||
		(!ClientText.empty() && !ParseEndpoint(ClientText))) {
		std::cerr << "GargantuanPlayer network endpoint arguments are invalid.\n";
		return 2;
	}
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
			World, Renderer.get(), nullptr, EngineProviderConfiguration{.AudioEnabled = !Headless, .Mode = Mode}
		);
#if defined(GARGANTUAN_WITH_GNS)
		if (!ServerText.empty()) {
			Session = std::make_unique<network::GameSession>(
				std::make_shared<network::GameNetworkingSocketsTransport>(),
				network::GameSessionConfiguration{
					.Role = network::GameSessionRole::Server,
					.Endpoint = *ParseEndpoint(ServerText),
					.Limits = network::GameSessionConfiguration::DefaultLimits(),
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
		int Frames = 0;
		auto DedicatedServerDeadline = std::chrono::steady_clock::now();
		while (Runtime->ProcessService->Alive) {
			if (Session) (void)Session->Poll();
			HostEvent Event;
			while (Host.PollEvent(Event)) {
				auto Result = Runtime->ProcessEvent(Event);
				if (Result.Command) Host.Apply(*Result.Command);
				if (!Runtime->ProcessService->Alive) break;
			}
			if (!Runtime->ProcessService->Alive) break;
			Runtime->Step();
			if (Session) {
				Session->Step(Runtime->GetSimulationTick());
				if (Session->GetStatus() == network::GameSessionStatus::Failed)
					throw std::runtime_error(Session->GetFailure());
			}
			if (MaximumFrames > 0 && ++Frames >= MaximumFrames) Runtime->ProcessService->MarkExit(0);
			if (Headless && !ServerText.empty()) {
				DedicatedServerDeadline += std::chrono::microseconds(16'667);
				const auto Now = std::chrono::steady_clock::now();
				if (DedicatedServerDeadline > Now)
					std::this_thread::sleep_until(DedicatedServerDeadline);
				else if (Now - DedicatedServerDeadline > std::chrono::milliseconds(250))
					DedicatedServerDeadline = Now;
			}
		}
		const auto ExitCode = Runtime->ProcessService->ExitCode;
		if (Session) Session->Stop();
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

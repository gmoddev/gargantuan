#include "host/server/ServerHost.hpp"

#include "host/common/PackagedHost.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/render/Renderer.hpp"

#include <argparse/argparse.hpp>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif

namespace gargantuan::host {
	namespace {
		volatile std::sig_atomic_t StopRequested = 0;

		void RequestStop(int) {
			StopRequested = 1;
		}

		class SignalLifetime final {
		  public:
			SignalLifetime() {
				StopRequested = 0;
				PreviousInterrupt = std::signal(SIGINT, RequestStop);
				PreviousTerminate = std::signal(SIGTERM, RequestStop);
			}

			~SignalLifetime() {
				std::signal(SIGINT, PreviousInterrupt);
				std::signal(SIGTERM, PreviousTerminate);
			}

		  private:
			using Handler = void (*)(int);
			Handler PreviousInterrupt = SIG_DFL;
			Handler PreviousTerminate = SIG_DFL;
		};

		bool IsPlayerOnlyArgument(std::string_view Argument) {
			return Argument.starts_with("--connect") || Argument.starts_with("--window") ||
				Argument.starts_with("--renderer") || Argument == "--headless";
		}
	}

	int RunDedicatedServer(
		int ArgumentCount,
		char *Arguments[],
		ServerHostConfiguration HostConfiguration
	) {
		argparse::ArgumentParser Program("GargantuanServer");
		Program.add_description("Gargantuan authoritative headless packaged server");
		Program.add_argument("--bind").default_value(std::string()).help("host:port for the authoritative game server");
		Program.add_argument("--startup-smoke").flag().help("exit after a bounded authoritative startup smoke");
		Program.add_argument("--session-smoke").flag().help("require a bounded packaged game-session acceptance proof");
		Program.add_argument("--max-ticks").scan<'i', int>().default_value(0).help("bounded test-only server tick count");
		Program.add_argument("--allow-insecure-development-network")
			.flag()
			.help("allow DevelopmentLocal networking beyond loopback; authentication is not provided");

		for (int Index = 1; Index < ArgumentCount; ++Index)
			if (IsPlayerOnlyArgument(Arguments[Index])) {
				std::cerr << "GargantuanServer does not accept graphical or client options; use GargantuanPlayer.\n";
				return 2;
			}
		try {
			Program.parse_args(ArgumentCount, Arguments);
		} catch (const std::exception &) {
			std::cerr << "GargantuanServer arguments are invalid.\n";
			return 2;
		}

		const auto BindText = Program.get<std::string>("--bind");
		const auto BindEndpoint = BindText.empty() ? std::nullopt : ParseEndpoint(BindText);
		const bool StartupSmoke = Program.is_used("--startup-smoke");
		const bool SessionSmoke = Program.is_used("--session-smoke");
		const bool AllowInsecureDevelopmentNetwork = Program.is_used("--allow-insecure-development-network");
		if ((!BindText.empty() && !BindEndpoint) || (!BindEndpoint && !StartupSmoke) || (SessionSmoke && !BindEndpoint) ||
			(BindEndpoint && !network::IsLoopbackTransportEndpoint(*BindEndpoint) &&
			 !AllowInsecureDevelopmentNetwork)) {
			std::cerr << "GargantuanServer bind arguments are invalid.\n";
			return 2;
		}
		if (AllowInsecureDevelopmentNetwork && BindEndpoint)
			std::cerr << "[Network:Security] DevelopmentLocal networking is exposed without peer authentication.\n";
#if !defined(GARGANTUAN_WITH_GNS)
		if (BindEndpoint) {
			std::cerr << "GargantuanServer was built without production game transport support.\n";
			return 2;
		}
#endif

		const auto RuntimeRoot = Paths::GetExecutableDirectory().lexically_normal();
		const auto PackageRoot = RuntimeRoot;
		int BootstrapExitCode = 0;
		auto Payload = BootstrapPackagedRuntime(PackageRoot, RuntimeRoot, "GargantuanServer", BootstrapExitCode);
		if (!Payload) return BootstrapExitCode;

		std::unique_ptr<Engine> Runtime;
		std::unique_ptr<network::GameSession> Session;
		try {
			auto World = PackageBuilder::LoadWorld(*Payload, PackageRoot);
			HeadlessRenderer Renderer(Vector2(320, 180));
			std::optional<ContentAvailabilityConfiguration> Content;
			if (HostConfiguration.ContentProvider) {
				if (!Payload->ContentManifestReference || !Payload->ContentManifestDigest.IsValid())
					throw std::runtime_error("trusted provider requires a package content manifest");
				Content = ContentAvailabilityConfiguration{
					.Provider = std::move(HostConfiguration.ContentProvider),
					.Package = {Payload->Inspection.Identity, Payload->Inspection.Revision},
					.ManifestDigest = Payload->ContentManifestDigest,
					.Mode = HostConfiguration.ContentMode,
				};
			} else {
				Content = PackageBuilder::GetLocalContentConfiguration(
					*Payload, PackageRoot, HostConfiguration.ContentMode
				);
			}
			Runtime = std::make_unique<Engine>(
				World,
				&Renderer,
				[](std::string Code, std::string Message) {
					if (Code == "Information") return;
					std::cerr << "[Runtime:Server] [" << Code << "] " << Message << '\n';
				},
				EngineProviderConfiguration{
					.Content = std::move(Content),
					.AudioEnabled = false,
					.Mode = RuntimeMode::NetworkServer,
				}
			);
#if defined(GARGANTUAN_WITH_GNS)
			if (BindEndpoint) {
				Session = std::make_unique<network::GameSession>(
					std::make_shared<network::GameNetworkingSocketsTransport>(),
					network::GameSessionConfiguration{
						.Role = network::GameSessionRole::Server,
						.Endpoint = *BindEndpoint,
						.Limits = network::GameSessionConfiguration::DefaultLimits(),
						.AllowInsecureDevelopmentNetwork = AllowInsecureDevelopmentNetwork,
					},
					Runtime.get()
				);
				if (!Session->Start().Succeeded())
					throw std::runtime_error("server game-session endpoint failed to start");
			}
#endif
			Runtime->ProcessService->Alive = true;
			LOG_INFO(
				App,
				"[Runtime:Server] Started %s (%s) in explicit NetworkServer mode",
				Payload->Inspection.DisplayName.c_str(),
				Payload->Inspection.Identity.ToString().c_str()
			);

			SignalLifetime Signals;
			const auto RequestedTicks = Program.get<int>("--max-ticks");
			const auto MaximumTicks = RequestedTicks > 0 ? RequestedTicks : (StartupSmoke ? 12 : 0);
			int Ticks = 0;
			auto TickDeadline = std::chrono::steady_clock::now();
			while (Runtime->ProcessService->Alive && StopRequested == 0) {
				if (Session) (void)Session->Poll();
				Runtime->Step();
				if (Session) {
					Session->Step(Runtime->GetSimulationTick());
					if (Session->GetStatus() == network::GameSessionStatus::Failed)
						throw std::runtime_error(Session->GetFailure());
					if (SessionSmoke) {
						const auto Metrics = Session->GetMetrics();
						if (Metrics.PlayersRemoved >= 1 && Runtime->Players->GetPlayers().empty())
							Runtime->ProcessService->MarkExit(0);
					}
				}
				if (MaximumTicks > 0 && ++Ticks >= MaximumTicks)
					Runtime->ProcessService->MarkExit(SessionSmoke ? 8 : 0);
				if (BindEndpoint) {
					TickDeadline += std::chrono::microseconds(16'667);
					const auto Now = std::chrono::steady_clock::now();
					if (TickDeadline > Now)
						std::this_thread::sleep_until(TickDeadline);
					else if (Now - TickDeadline > std::chrono::milliseconds(250))
						TickDeadline = Now;
				}
			}

			const auto ExitCode = Runtime->ProcessService->ExitCode;
			if (Session) Session->Stop();
			Session.reset();
			Runtime->Destroy();
			Runtime.reset();
			return ExitCode;
		} catch (const std::exception &Error) {
			std::cerr << "GargantuanServer stopped because packaged runtime startup failed: " << Error.what() << '\n';
			if (Session) Session->Stop();
			Session.reset();
			if (Runtime) Runtime->Destroy();
			Runtime.reset();
			return 7;
		}
	}
}

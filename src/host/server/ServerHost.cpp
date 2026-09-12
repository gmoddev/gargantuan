#include "host/server/ServerHost.hpp"

#include "host/common/PackagedHost.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
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
#include <unordered_set>
#include <variant>

#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif
#if defined(GARGANTUAN_WITH_NODE_CONTENT)
#include "host/server/NodeContentProvider.hpp"
#endif

namespace gargantuan::host {
	namespace {
#if defined(GARGANTUAN_WITH_GNS)
		// Only constructed by the trusted bounded session-smoke mode. This observes
		// ordinary sends unchanged; it has no queue, scheduling or authority policy.
		class SessionSmokeTransport final : public network::IGameTransport {
			std::shared_ptr<network::IGameTransport> Delegate;
			std::uint32_t ApplicationTraces = 0;
			std::uint32_t StructuralTraces = 0;
		  public:
			explicit SessionSmokeTransport(std::shared_ptr<network::IGameTransport> Value) : Delegate(std::move(Value)) {}
			network::TransportOperationResult Start(const network::TransportStartConfiguration &Value) override { return Delegate->Start(Value); }
			network::TransportOperationResult Stop(network::DisconnectInfo Value) override { return Delegate->Stop(std::move(Value)); }
			network::TransportOperationResult Disconnect(network::ConnectionId Connection, network::DisconnectInfo Value) override { return Delegate->Disconnect(Connection, std::move(Value)); }
			std::size_t PollEvents(std::span<network::TransportEvent> Output) override { return Delegate->PollEvents(Output); }
			std::optional<std::size_t> GetAvailableDatagramBytes(network::ConnectionId Connection) const override { return Delegate->GetAvailableDatagramBytes(Connection); }
			std::optional<network::NetworkStatistics> GetStatistics(network::ConnectionId Connection) const override { return Delegate->GetStatistics(Connection); }
			network::TransportOperationResult Send(const network::NetworkMessageIntent &Message) override {
				const bool Structural = Message.Traffic() == network::TrafficClass::StructuralReplication;
				const bool Application = Message.Traffic() == network::TrafficClass::ReliableApplication;
				auto &Count = Structural ? StructuralTraces : ApplicationTraces;
				if ((!Structural && !Application) || Count >= 256) return Delegate->Send(Message);
				++Count;
				const auto Before = Delegate->GetStatistics(Message.Destination());
				const auto Result = Delegate->Send(Message);
				std::cout << "[Content:TransportSend] monotonic_us="
					<< std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
					<< " structural=" << Structural << " bytes=" << Message.Payload().size()
					<< " queued_reliable_before=" << (Before && Before->QueuedReliableBytes ? *Before->QueuedReliableBytes : 0)
					<< " accepted=" << Result.Succeeded() << '\n';
				return Result;
			}
		};
#endif
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

		bool IsContentArgument(std::string_view Argument) { return Argument.starts_with("--content-"); }

		std::optional<ContentResidencyMode> ParseContentResidencyMode(std::string_view Value) {
			if (Value == "fully-resident") return ContentResidencyMode::FullyResident;
			if (Value == "on-demand") return ContentResidencyMode::OnDemand;
			return std::nullopt;
		}

		std::string_view ContentResidencyName(ContentResidencyMode Mode) {
			return Mode == ContentResidencyMode::FullyResident ? "fully-resident" : "on-demand";
		}

		std::string GetSemanticDigest(std::shared_ptr<Instance> Root) {
			auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
			if (Encoded.empty()) throw std::runtime_error("streamed content could not be serialized for semantic validation");
			return AssetContentId::Hash(std::span(
				reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()
			)).ToString();
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
		Program.add_argument("--content-provider")
			.default_value(std::string("local"))
			.help("trusted content origin: local (default) or node");
		Program.add_argument("--content-residency")
			.default_value(std::string("fully-resident"))
			.help("content residency policy: fully-resident or on-demand");
		Program.add_argument("--content-node-endpoint")
			.default_value(std::string())
			.help("TLS ContentStreaming endpoint used only in Node mode");
		Program.add_argument("--content-node-root-ca")
			.default_value(std::string())
			.help("trusted root certificate file used only in Node mode");
		Program.add_argument("--content-node-token-env")
			.default_value(std::string())
			.help("environment-variable name containing the Node workload token");
		Program.add_argument("--content-lifecycle-smoke")
			.default_value(std::string())
			.help("test-only trusted content key for official load/evict/reload validation");
		Program.add_argument("--content-churn-cycles").scan<'i', int>().default_value(0)
			.help("test-only bounded repeated lifecycle smoke cycles (1-10000)");
		Program.add_argument("--content-stop-in-flight-smoke")
			.default_value(std::string())
			.help("test-only trusted content key for official shutdown cancellation validation");
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

		bool HasContentArgument = false;
		for (int Index = 1; Index < ArgumentCount; ++Index)
			HasContentArgument = HasContentArgument || IsContentArgument(Arguments[Index]);
		if (HasContentArgument && !std::holds_alternative<LocalServerContentConfiguration>(HostConfiguration.Content)) {
			std::cerr << "GargantuanServer content arguments conflict with injected trusted host configuration.\n";
			return 2;
		}
		const auto Residency = ParseContentResidencyMode(Program.get<std::string>("--content-residency"));
		const auto ProviderName = Program.get<std::string>("--content-provider");
		const auto NodeEndpoint = Program.get<std::string>("--content-node-endpoint");
		const auto NodeRootCertificate = Program.get<std::string>("--content-node-root-ca");
		const auto NodeTokenEnvironment = Program.get<std::string>("--content-node-token-env");
		const auto ContentLifecycleKey = Program.get<std::string>("--content-lifecycle-smoke");
		const auto ContentChurnCycles = Program.get<int>("--content-churn-cycles");
		const auto StopInFlightKey = Program.get<std::string>("--content-stop-in-flight-smoke");
		const bool HasNodeOption = !NodeEndpoint.empty() || !NodeRootCertificate.empty() || !NodeTokenEnvironment.empty();
		if (ProviderName == "node" && NodeEndpoint.empty()) {
			std::cerr << "GargantuanServer Node content provider requires --content-node-endpoint.\n";
			return 2;
		}
		if (ProviderName == "node" && NodeRootCertificate.empty()) {
			std::cerr << "GargantuanServer Node content provider requires --content-node-root-ca.\n";
			return 2;
		}
		if (ProviderName == "node" && NodeTokenEnvironment.empty()) {
			std::cerr << "GargantuanServer Node content provider requires --content-node-token-env.\n";
			return 2;
		}
		if (ProviderName == "node" && !ParseEndpoint(NodeEndpoint)) {
			std::cerr << "GargantuanServer Node content endpoint is invalid.\n";
			return 2;
		}
		if (ProviderName == "local" && HasNodeOption) {
			std::cerr << "GargantuanServer Node content options require --content-provider node.\n";
			return 2;
		}
		if (!Residency || (ProviderName != "local" && ProviderName != "node") ||
			ContentChurnCycles < 0 || ContentChurnCycles > 10000 ||
			(ContentChurnCycles != 0 && (ContentLifecycleKey.empty() || *Residency != ContentResidencyMode::OnDemand)) ||
			(!ContentLifecycleKey.empty() && !StopInFlightKey.empty()) ||
			(!ContentLifecycleKey.empty() && !IsValidPackageContentKey(ContentLifecycleKey)) ||
			(!StopInFlightKey.empty() && !IsValidPackageContentKey(StopInFlightKey))) {
			std::cerr << "GargantuanServer content provider arguments are invalid.\n";
			return 2;
		}
		if (HasContentArgument) {
			if (ProviderName == "local") {
				HostConfiguration.Content = LocalServerContentConfiguration{.Mode = *Residency};
			} else {
#if defined(GARGANTUAN_WITH_NODE_CONTENT)
				HostConfiguration.Content = NodeServerContentConfiguration{
					.Endpoint = NodeEndpoint,
					.RootCertificateFile = NodeRootCertificate,
					.WorkloadTokenEnvironment = NodeTokenEnvironment,
					.Mode = *Residency,
				};
#else
				std::cerr << "GargantuanServer was built without Node content provider support.\n";
				return 2;
#endif
			}
		}

		const auto BindText = Program.get<std::string>("--bind");
		const auto BindEndpoint = BindText.empty() ? std::nullopt : ParseEndpoint(BindText);
		const bool StartupSmoke = Program.is_used("--startup-smoke");
		const bool SessionSmoke = Program.is_used("--session-smoke");
		// Keep bounded acceptance diagnostics available even if the harness must
		// terminate a failed long-running smoke process.
		if (SessionSmoke || ContentChurnCycles != 0) {
			std::cout << std::unitbuf;
			SDL_SetLogOutputFunction([](void *, int, SDL_LogPriority, const char *Message) {
				std::cerr << Message << '\n';
			}, nullptr);
			SDL_SetLogPriority(LogCategory::App, SDL_LOG_PRIORITY_INFO);
			SDL_SetLogPriority(LogCategory::Lua, SDL_LOG_PRIORITY_INFO);
		}
		const bool AllowInsecureDevelopmentNetwork = Program.is_used("--allow-insecure-development-network");
		if ((!BindText.empty() && !BindEndpoint) ||
			(!BindEndpoint && !StartupSmoke && StopInFlightKey.empty()) || (SessionSmoke && !BindEndpoint) ||
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

		// These owners also outlive the catch handler. Engine::Destroy borrows
		// Renderer, and GameSession::Stop borrows Engine during exception cleanup.
		std::unique_ptr<HeadlessRenderer> Renderer;
		std::unique_ptr<Engine> Runtime;
		std::unique_ptr<network::GameSession> Session;
		try {
			const auto HostStartupStarted = std::chrono::steady_clock::now();
			const auto HostStartupUnixMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			auto World = PackageBuilder::LoadWorld(*Payload, PackageRoot);
			Renderer = std::make_unique<HeadlessRenderer>(Vector2(320, 180));
			std::optional<ContentAvailabilityConfiguration> Content;
			const bool IsNodeProvider = std::holds_alternative<NodeServerContentConfiguration>(HostConfiguration.Content);
			ContentResidencyMode ContentMode = ContentResidencyMode::FullyResident;
			std::string_view EffectiveProviderName = "local";
			std::string NodeEndpointDiagnostic;
			std::chrono::milliseconds NodeBootstrapDeadline{30'000};
			if (const auto *Injected = std::get_if<InjectedServerContentConfiguration>(&HostConfiguration.Content)) {
				if (!Injected->Provider)
					throw std::runtime_error("trusted content provider configuration is empty");
				if (!Payload->ContentManifestReference || !Payload->ContentManifestDigest.IsValid())
					throw std::runtime_error("trusted provider requires a package content manifest");
				Content = ContentAvailabilityConfiguration{
					.Provider = Injected->Provider,
					.Package = {Payload->Inspection.Identity, Payload->Inspection.Revision},
					.ManifestDigest = Payload->ContentManifestDigest,
					.Mode = Injected->Mode,
				};
				ContentMode = Injected->Mode;
				EffectiveProviderName = Injected->Provider->Name();
			} else if (const auto *Local = std::get_if<LocalServerContentConfiguration>(&HostConfiguration.Content)) {
				Content = PackageBuilder::GetLocalContentConfiguration(
					*Payload, PackageRoot, Local->Mode
				);
				ContentMode = Local->Mode;
			} else if (const auto *Node = std::get_if<NodeServerContentConfiguration>(&HostConfiguration.Content)) {
#if defined(GARGANTUAN_WITH_NODE_CONTENT)
				if (!Payload->ContentManifestReference || !Payload->ContentManifestDigest.IsValid())
					throw std::runtime_error("Node content provider requires a package content manifest");
				auto Provider = std::make_shared<NodeContentProvider>(NodeContentProviderConfiguration{
					.Endpoint = Node->Endpoint,
					.RootCertificateFile = Node->RootCertificateFile,
					.WorkloadTokenEnvironment = Node->WorkloadTokenEnvironment,
				});
				Content = ContentAvailabilityConfiguration{
					.Provider = std::move(Provider),
					.Package = {Payload->Inspection.Identity, Payload->Inspection.Revision},
					.ManifestDigest = Payload->ContentManifestDigest,
					.Mode = Node->Mode,
				};
				ContentMode = Node->Mode;
				EffectiveProviderName = "node";
				NodeEndpointDiagnostic = Node->Endpoint;
				NodeBootstrapDeadline = Node->BootstrapDeadline;
#else
				(void)Node;
				throw std::runtime_error("Node content provider support is unavailable");
#endif
			}
			LOG_INFO(
				App,
				"[Content:Server] Provider=%s Residency=%s Project=%s PackageVersion=%llu TLS=%s Endpoint=%s",
				std::string(EffectiveProviderName).c_str(),
				std::string(ContentResidencyName(ContentMode)).c_str(),
				Payload->Inspection.Identity.ToString().c_str(),
				static_cast<unsigned long long>(Payload->Inspection.Revision),
				IsNodeProvider ? "enabled" : "not-applicable",
				IsNodeProvider ? NodeEndpointDiagnostic.c_str() : "not-applicable"
			);
			std::cout << "[Content:Server] Provider=" << EffectiveProviderName
					  << " Residency=" << ContentResidencyName(ContentMode)
					  << " Project=" << Payload->Inspection.Identity.ToString()
					  << " PackageVersion=" << Payload->Inspection.Revision
					  << " TLS=" << (IsNodeProvider ? "enabled" : "not-applicable")
					  << " Endpoint=" << (IsNodeProvider ? NodeEndpointDiagnostic : "not-applicable") << '\n';
			const auto ContentBootstrapStarted = std::chrono::steady_clock::now();
			Runtime = std::make_unique<Engine>(
				World,
				Renderer.get(),
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
			if (IsNodeProvider) {
				auto *Availability = Runtime->GetContentAvailability();
				const auto Deadline = ContentBootstrapStarted + NodeBootstrapDeadline;
				while (!Availability || !Availability->IsBootstrapComplete()) {
					if (!Availability || std::chrono::steady_clock::now() >= Deadline ||
						(Availability->GetActiveRequestCount() == 0 && !Availability->IsBootstrapComplete()))
						throw std::runtime_error("Node content bootstrap failed");
					Availability->Step();
					std::this_thread::yield();
				}
				const auto BootstrapMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - ContentBootstrapStarted
				).count();
				LOG_INFO(
					App,
					"[Content:Server] Node bootstrap ready ManifestBytes=%llu WallMilliseconds=%lld",
					static_cast<unsigned long long>(Availability->GetMetrics().ManifestBytes),
					static_cast<long long>(BootstrapMilliseconds)
				);
				std::cout << "[Content:Server] Node bootstrap ready ManifestBytes="
						  << Availability->GetMetrics().ManifestBytes
						  << " WallMilliseconds=" << BootstrapMilliseconds << '\n';
			}
#if defined(GARGANTUAN_WITH_GNS)
			if (BindEndpoint) {
				std::shared_ptr<network::IGameTransport> Transport = std::make_shared<network::GameNetworkingSocketsTransport>();
				if (SessionSmoke) Transport = std::make_shared<SessionSmokeTransport>(std::move(Transport));
				Session = std::make_unique<network::GameSession>(
					std::move(Transport),
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
			const auto StartupWallMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - HostStartupStarted
			).count();
			LOG_INFO(
				App,
				"[Runtime:Server] StartupWallMilliseconds=%lld",
				static_cast<long long>(StartupWallMilliseconds)
			);
			std::cout << "[Runtime:Server] StartupWallMilliseconds=" << StartupWallMilliseconds << '\n';
			if (ContentChurnCycles != 0)
				std::cout << "[Content:Churn] ClockAnchorUnixMilliseconds=" << HostStartupUnixMilliseconds << '\n';

			SignalLifetime Signals;
			auto HostElapsedMilliseconds = [&] {
				return std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - HostStartupStarted
				).count();
			};
			const auto RequestedTicks = Program.get<int>("--max-ticks");
			const auto MaximumTicks = RequestedTicks > 0 ? RequestedTicks
				: (StartupSmoke ? 12 : (!StopInFlightKey.empty() ? 180 : 0));
			int Ticks = 0;
			enum class ContentSmokeStage : std::uint8_t {
				WaitingForPeer,
				WaitingForFirstResident,
				HoldingFirstResident,
				WaitingForEviction,
				HoldingEviction,
				WaitingForReload,
				WaitingForFinalEviction,
				HoldingFinalDrain,
				Complete,
			};
			auto ContentStage = ContentSmokeStage::WaitingForPeer;
			std::unordered_set<ObjectId> BootstrapWorkspaceObjects;
			const bool ContentStartsResident = Runtime->Content && !ContentLifecycleKey.empty() &&
				Runtime->Content->GetState(ContentLifecycleKey) == ContentResidencyState::Resident;
			if (!ContentStartsResident)
				for (const auto &Child : Runtime->Workspace->GetChildren())
					BootstrapWorkspaceObjects.insert(Child->GetObjectId());
			std::shared_ptr<Instance> FirstContentRoot;
			ObjectId FirstContentObjectId;
			int ContentStageTick = 0;
			int CompletedContentCycles = 0;
			bool StopRequestSubmitted = false;
			std::optional<std::chrono::steady_clock::time_point> StopRequestObserved;
			auto FindStreamedRoot = [&]() -> std::shared_ptr<Instance> {
				for (const auto &Child : Runtime->Workspace->GetChildren())
					if (!BootstrapWorkspaceObjects.contains(Child->GetObjectId())) return Child;
				return {};
			};
			if ((!ContentLifecycleKey.empty() || !StopInFlightKey.empty()) && !Runtime->Content)
				throw std::runtime_error("trusted content smoke requires package content availability");
			auto TickDeadline = std::chrono::steady_clock::now();
			auto PreviousTickStarted = TickDeadline;
			auto PreviousSessionMetrics = network::GameSessionMetrics{};
			std::uint32_t ProfileTicks = 0;
			if (SessionSmoke)
				std::cout << "[Runtime:ServerFrame] unix_us,tick,interval_ns,poll_ns,engine_ns,session_ns,encode_ns,relevance_ns,materialize_ns,selected,committed,pending,wire_bytes\n";
			while (Runtime->ProcessService->Alive && StopRequested == 0) {
				const auto TickStarted = std::chrono::steady_clock::now();
				if (Session) (void)Session->Poll();
				const auto EngineStarted = std::chrono::steady_clock::now();
				Runtime->Step();
				const auto SessionStarted = std::chrono::steady_clock::now();
				if (Session) {
					Session->Step(Runtime->GetSimulationTick());
					if (Session->GetStatus() == network::GameSessionStatus::Failed)
						throw std::runtime_error(Session->GetFailure());
					if (SessionSmoke) {
						const auto Metrics = Session->GetMetrics();
						const bool ContentProofComplete = ContentLifecycleKey.empty() ||
							(ContentStage == ContentSmokeStage::Complete &&
							 Runtime->CharacterControl->GetAttributeValue("BeforeContentEventObserved").has_value() &&
							 Runtime->CharacterControl->GetAttributeValue("ContentResidentObserved").has_value() &&
							 Runtime->CharacterControl->GetAttributeValue("ContentEvictedObserved").has_value() &&
							 Runtime->CharacterControl->GetAttributeValue("ContentReloadedObserved").has_value() &&
							 Runtime->CharacterControl->GetAttributeValue("RemoteFunctionObserved").has_value());
						if (Metrics.PlayersRemoved >= 1 && Runtime->Players->GetPlayers().empty() && ContentProofComplete)
							Runtime->ProcessService->MarkExit(0);
					}
				}
				if (SessionSmoke && Session && ProfileTicks < 4096) {
					const auto Now = std::chrono::steady_clock::now();
					const auto Metrics = Session->GetMetrics();
					auto Ns = [](auto Duration) { return std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count(); };
					std::cout << "[Runtime:ServerFrame] "
						<< std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
						<< ',' << ++ProfileTicks << ',' << Ns(TickStarted - PreviousTickStarted) << ',' << Ns(EngineStarted - TickStarted)
						<< ',' << Ns(SessionStarted - EngineStarted) << ',' << Ns(Now - SessionStarted)
						<< ',' << Metrics.BaselineEncodeCpuNanoseconds - PreviousSessionMetrics.BaselineEncodeCpuNanoseconds
						<< ',' << Metrics.RelevanceCpuNanoseconds - PreviousSessionMetrics.RelevanceCpuNanoseconds
						<< ',' << Metrics.MaterializationCpuNanoseconds - PreviousSessionMetrics.MaterializationCpuNanoseconds
						<< ',' << Metrics.StructuralTransitionsSelected - PreviousSessionMetrics.StructuralTransitionsSelected
						<< ',' << Metrics.StructuralTransitionsCommitted - PreviousSessionMetrics.StructuralTransitionsCommitted
						<< ',' << Metrics.StructuralPendingEnters + Metrics.StructuralPendingLeaves
						<< ',' << Metrics.StructuralBytesEncoded - PreviousSessionMetrics.StructuralBytesEncoded << '\n';
					PreviousSessionMetrics = Metrics;
				}
				PreviousTickStarted = TickStarted;
				if (!ContentLifecycleKey.empty()) {
					const int ContentHoldTicks = ContentChurnCycles == 0 || CompletedContentCycles <= 1 ? 45 : 6;
					switch (ContentStage) {
					case ContentSmokeStage::WaitingForPeer:
						if (!Session || Session->GetMetrics().ReadyPeers >= 1) {
							const auto State = Runtime->Content->GetState(ContentLifecycleKey);
							if (!State) {
								if (Runtime->Content->IsManifestAvailable())
									throw std::runtime_error(
										"trusted content smoke key is absent from the active package manifest"
									);
								break;
							}
							if (*State == ContentResidencyState::Resident) {
								FirstContentRoot = FindStreamedRoot();
								if (!FirstContentRoot) throw std::runtime_error("Resident content has no authoritative root");
								FirstContentObjectId = FirstContentRoot->GetObjectId();
								const auto SemanticDigest = GetSemanticDigest(FirstContentRoot);
								LOG_INFO(
									App, "[Content:Server] CONTENT_LIFECYCLE_FIRST_RESIDENT Key=%s SemanticDigest=%s",
									ContentLifecycleKey.c_str(), SemanticDigest.c_str()
								);
								std::cout << "[Content:Server] CONTENT_LIFECYCLE_FIRST_RESIDENT Key="
										  << ContentLifecycleKey << " SemanticDigest=" << SemanticDigest
										  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
								if (ContentMode == ContentResidencyMode::FullyResident) {
									std::cout << "[Content:Server] CONTENT_FULLY_RESIDENT_OK Key="
											  << ContentLifecycleKey << " SemanticDigest=" << SemanticDigest << '\n';
									ContentStage = ContentSmokeStage::Complete;
								} else
									ContentStage = ContentSmokeStage::HoldingFirstResident;
								ContentStageTick = Ticks;
							} else {
								// A connected peer can add its Character beneath Workspace before the
								// trusted demand. Reset the comparison set at the demand boundary; the
								// non-Resident state above is the authoritative pre-demand proof.
								BootstrapWorkspaceObjects.clear();
								for (const auto &Child : Runtime->Workspace->GetChildren())
									BootstrapWorkspaceObjects.insert(Child->GetObjectId());
								if (!Runtime->Content->RequestContent(ContentLifecycleKey))
									throw std::runtime_error("trusted content demand was rejected");
								std::cout << "[Content:Server] CONTENT_DEMAND_ISSUED Key=" << ContentLifecycleKey
										  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
								ContentStage = ContentSmokeStage::WaitingForFirstResident;
							}
						}
						break;
					case ContentSmokeStage::WaitingForFirstResident:
						if (Runtime->Content->GetState(ContentLifecycleKey) == ContentResidencyState::Resident) {
							FirstContentRoot = FindStreamedRoot();
							if (!FirstContentRoot) throw std::runtime_error("Resident content has no authoritative root");
							FirstContentObjectId = FirstContentRoot->GetObjectId();
							const auto SemanticDigest = GetSemanticDigest(FirstContentRoot);
							LOG_INFO(
								App, "[Content:Server] CONTENT_LIFECYCLE_FIRST_RESIDENT Key=%s SemanticDigest=%s",
								ContentLifecycleKey.c_str(), SemanticDigest.c_str()
							);
							std::cout << "[Content:Server] CONTENT_LIFECYCLE_FIRST_RESIDENT Key="
									  << ContentLifecycleKey << " SemanticDigest=" << SemanticDigest
									  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
							if (ContentMode == ContentResidencyMode::FullyResident) {
								std::cout << "[Content:Server] CONTENT_FULLY_RESIDENT_OK Key="
										  << ContentLifecycleKey << " SemanticDigest=" << SemanticDigest << '\n';
								ContentStage = ContentSmokeStage::Complete;
							} else
								ContentStage = ContentSmokeStage::HoldingFirstResident;
							ContentStageTick = Ticks;
						}
						break;
					case ContentSmokeStage::HoldingFirstResident:
						// The RSS soak proves one complete network lifecycle first, then
						// churns the authoritative world after that Player disconnects.
						// Continuous peer pressure is measured separately by the scale test.
						if (ContentChurnCycles != 0 && CompletedContentCycles == 1 && Session &&
							Session->GetMetrics().PlayersRemoved == 0) break;
						if (Ticks - ContentStageTick >= ContentHoldTicks) {
							if (!Runtime->Content->ReleaseContent(ContentLifecycleKey))
								throw std::runtime_error("trusted content eviction demand was rejected");
							std::cout << "[Content:Server] CONTENT_EVICTION_ISSUED Key=" << ContentLifecycleKey
									  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
							ContentStage = ContentSmokeStage::WaitingForEviction;
						}
						break;
					case ContentSmokeStage::WaitingForEviction:
						if (const auto State = Runtime->Content->GetState(ContentLifecycleKey);
							State == ContentResidencyState::Available || State == ContentResidencyState::Unavailable) {
							if (!FirstContentRoot->GetDestroyed() || FindStreamedRoot())
								throw std::runtime_error("evicted authoritative content remained attached");
							LOG_INFO(App, "[Content:Server] CONTENT_LIFECYCLE_EVICTED Key=%s", ContentLifecycleKey.c_str());
							std::cout << "[Content:Server] CONTENT_LIFECYCLE_EVICTED Key=" << ContentLifecycleKey << '\n';
							std::cout << "[Content:Server] CONTENT_AUTHORITATIVE_REMOVAL Key=" << ContentLifecycleKey
									  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
							ContentStage = ContentSmokeStage::HoldingEviction;
							ContentStageTick = Ticks;
						}
						break;
					case ContentSmokeStage::HoldingEviction:
						if (Ticks - ContentStageTick >= ContentHoldTicks) {
							if (!Runtime->Content->RequestContent(ContentLifecycleKey))
								throw std::runtime_error("trusted content reload demand was rejected");
							std::cout << "[Content:Server] CONTENT_RELOAD_ISSUED Key=" << ContentLifecycleKey
									  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
							ContentStage = ContentSmokeStage::WaitingForReload;
						}
						break;
					case ContentSmokeStage::WaitingForReload:
						if (Runtime->Content->GetState(ContentLifecycleKey) == ContentResidencyState::Resident) {
							auto ReloadedRoot = FindStreamedRoot();
							if (!ReloadedRoot || ReloadedRoot->GetObjectId() == FirstContentObjectId)
								throw std::runtime_error("content reload reused a stale authoritative lifetime");
							const auto SemanticDigest = GetSemanticDigest(ReloadedRoot);
							LOG_INFO(
								App, "[Content:Server] CONTENT_LIFECYCLE_RELOAD_OK Key=%s SemanticDigest=%s",
								ContentLifecycleKey.c_str(), SemanticDigest.c_str()
							);
							std::cout << "[Content:Server] CONTENT_LIFECYCLE_RELOAD_OK Key="
									  << ContentLifecycleKey << " SemanticDigest=" << SemanticDigest
									  << " ElapsedMilliseconds=" << HostElapsedMilliseconds() << '\n';
							++CompletedContentCycles;
							if (ContentChurnCycles != 0 && CompletedContentCycles < ContentChurnCycles) {
								FirstContentRoot = std::move(ReloadedRoot);
								FirstContentObjectId = FirstContentRoot->GetObjectId();
								ContentStage = ContentSmokeStage::HoldingFirstResident;
								ContentStageTick = Ticks;
							} else if (ContentChurnCycles != 0) {
								if (!Runtime->Content->ReleaseContent(ContentLifecycleKey))
									throw std::runtime_error("trusted final drain was rejected");
								ContentStage = ContentSmokeStage::WaitingForFinalEviction;
							} else ContentStage = ContentSmokeStage::Complete;
							if (ContentChurnCycles != 0) {
								const auto Memory = Runtime->Content->GetMetrics();
								std::cout << "[Content:Churn] cycle=" << CompletedContentCycles
									<< " elapsed_ms=" << HostElapsedMilliseconds() << " cache_bytes=" << Memory.CachedPayloadBytes
									<< " decoded_bytes=" << Memory.DecodedDocumentBytes << " decoded_high_water=" << Memory.DecodedDocumentBytesHighWater
									<< " completion_high_water=" << Memory.CompletedPayloadBytesHighWater
									<< " detached_objects_high_water=" << Memory.DetachedObjectsHighWater
									<< " resident_objects=" << Memory.ResidentPackageObjects << '\n';
							}
						}
						break;
					case ContentSmokeStage::WaitingForFinalEviction:
						if (Runtime->Content->GetState(ContentLifecycleKey) == ContentResidencyState::Unavailable) {
							if (FindStreamedRoot()) throw std::runtime_error("final content drain retained a runtime root");
							ContentStageTick = Ticks;
							ContentStage = ContentSmokeStage::HoldingFinalDrain;
							std::cout << "[Content:Churn] FINAL_DRAIN elapsed_ms=" << HostElapsedMilliseconds() << '\n';
						}
						break;
					case ContentSmokeStage::HoldingFinalDrain:
						if (Ticks - ContentStageTick >= 120) {
							const auto Memory = Runtime->Content->GetMetrics();
							if (Memory.ResidentUnits != 0 || Memory.DecodedDocumentBytes != 0 ||
								Memory.CompletedPayloadBytes != 0 || Runtime->Content->GetActiveRequestCount() != 0)
								throw std::runtime_error("final content drain retained live work");
							std::cout << "[Content:Churn] CONTENT_CHURN_OK cycles=" << CompletedContentCycles << '\n';
							ContentStage = ContentSmokeStage::Complete;
						}
						break;
					case ContentSmokeStage::Complete:
						if (ContentChurnCycles != 0 && !Session) Runtime->ProcessService->MarkExit(0);
						break;
					}
				}
				if (!StopInFlightKey.empty()) {
					if (!StopRequestSubmitted) {
						if (!Runtime->Content->GetState(StopInFlightKey)) {
							if (Runtime->Content->IsManifestAvailable())
								throw std::runtime_error(
									"trusted content smoke key is absent from the active package manifest"
								);
						} else {
							if (!Runtime->Content->RequestContent(StopInFlightKey))
								throw std::runtime_error("trusted in-flight shutdown demand was rejected");
							StopRequestSubmitted = true;
						}
					}
					if (!StopRequestObserved && Runtime->Content->GetActiveRequestCount() > 0) {
					StopRequestObserved = std::chrono::steady_clock::now();
					LOG_INFO(App, "[Content:Server] CONTENT_REQUEST_IN_FLIGHT Key=%s", StopInFlightKey.c_str());
					std::cout << "[Content:Server] CONTENT_REQUEST_IN_FLIGHT Key=" << StopInFlightKey << '\n';
					}
					if (StopRequestObserved &&
						std::chrono::steady_clock::now() - *StopRequestObserved >= std::chrono::milliseconds(250))
						Runtime->ProcessService->MarkExit(0);
				}
				if (MaximumTicks > 0 && ++Ticks >= MaximumTicks) {
					const bool ContentIncomplete = !ContentLifecycleKey.empty() && ContentStage != ContentSmokeStage::Complete;
					Runtime->ProcessService->MarkExit(SessionSmoke ? 8 : (ContentIncomplete ? 9 : 0));
				}
				if (BindEndpoint) {
					TickDeadline += std::chrono::microseconds(16'667);
					const auto Now = std::chrono::steady_clock::now();
					if (TickDeadline > Now)
						std::this_thread::sleep_until(TickDeadline);
					else if (Now - TickDeadline > std::chrono::milliseconds(250))
						TickDeadline = Now;
				} else if (!ContentLifecycleKey.empty() || !StopInFlightKey.empty())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			const auto ExitCode = Runtime->ProcessService->ExitCode;
			if (Runtime->Content) {
				const auto Metrics = Runtime->Content->GetMetrics();
				LOG_INFO(
					App,
					"[Content:Server] requests=%llu acquisitions=%llu admissions=%llu evictions=%llu failures=%llu "
					"cancellations=%llu stale=%llu provider_us=%llu verification_us=%llu preparation_us=%llu commit_us=%llu",
					static_cast<unsigned long long>(Metrics.Requests),
					static_cast<unsigned long long>(Metrics.Acquisitions),
					static_cast<unsigned long long>(Metrics.Admissions),
					static_cast<unsigned long long>(Metrics.Evictions),
					static_cast<unsigned long long>(Metrics.Failures),
					static_cast<unsigned long long>(Metrics.Cancellations),
					static_cast<unsigned long long>(Metrics.StaleCompletionsRejected),
					static_cast<unsigned long long>(Metrics.Timing.Provider.TotalMicroseconds),
					static_cast<unsigned long long>(Metrics.Timing.Verification.TotalMicroseconds),
					static_cast<unsigned long long>(Metrics.Timing.Preparation.TotalMicroseconds),
					static_cast<unsigned long long>(Metrics.Timing.Commit.TotalMicroseconds)
				);
				std::cout << "[Content:Server] requests=" << Metrics.Requests
						  << " acquisitions=" << Metrics.Acquisitions
						  << " admissions=" << Metrics.Admissions
						  << " evictions=" << Metrics.Evictions
						  << " failures=" << Metrics.Failures
						  << " cancellations=" << Metrics.Cancellations
						  << " stale=" << Metrics.StaleCompletionsRejected
						  << " provider_us=" << Metrics.Timing.Provider.TotalMicroseconds
						  << " verification_us=" << Metrics.Timing.Verification.TotalMicroseconds
						  << " preparation_us=" << Metrics.Timing.Preparation.TotalMicroseconds
						  << " commit_us=" << Metrics.Timing.Commit.TotalMicroseconds
						  << " accepted_to_queued_us=" << Metrics.Timing.AcceptedToQueued.TotalMicroseconds
						  << " queued_to_provider_us=" << Metrics.Timing.QueuedToProviderStart.TotalMicroseconds
						  << " verification_to_preparation_us="
						  << Metrics.Timing.VerificationToPreparation.TotalMicroseconds
						  << " preparation_to_commit_us=" << Metrics.Timing.PreparationToCommit.TotalMicroseconds
						  << " end_to_end_us=" << Metrics.Timing.EndToEnd.TotalMicroseconds
						  << " step_us=" << Metrics.Timing.Step.TotalMicroseconds << '\n';
			}
			const auto ShutdownStarted = std::chrono::steady_clock::now();
			LOG_INFO(App, "[Runtime:Server] Stopping peer acceptance and game session");
			if (Session) Session->Stop();
			Session.reset();
			LOG_INFO(App, "[Runtime:Server] Stopping authoritative runtime and content provider");
			Runtime->Destroy();
			Runtime.reset();
			std::cout << "[Runtime:Server] ShutdownMicroseconds="
					  << std::chrono::duration_cast<std::chrono::microseconds>(
							 std::chrono::steady_clock::now() - ShutdownStarted
						 ).count()
					  << '\n';
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

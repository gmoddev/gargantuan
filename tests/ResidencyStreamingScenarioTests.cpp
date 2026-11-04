#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	struct StreamingFixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-3m-streaming-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		PackageContentManifest Manifest;
		AssetContentId Digest;
		std::vector<std::filesystem::path> Files;

		StreamingFixture() {
			Check(std::filesystem::create_directories(Root / "content"), "could not create streaming fixture directory");
			Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 31};
			for (int Index = 0; Index < 5; ++Index) {
				const auto Key = "cell/0" + std::to_string(Index);
				const float X = static_cast<float>(Index) * 512.0f;
				auto Object = std::make_shared<Part>();
				Object->SetName("StreamingUnit_" + std::to_string(Index));
				Object->SetAnchored(true);
				Object->SetCanCollide(false);
				Object->SetSize({1, 1, 1});
				Object->SetCFrame(CFrame(X, 0, 0));
				Object->SetArchivable(true);
				const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Object);
				const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
				PackageContentEntry Entry;
				Entry.Key = Key;
				Entry.BlobReference = "content/cell_0" + std::to_string(Index) + ".json";
				Entry.Digest = AssetContentId::Hash(Bytes);
				Entry.CompressedBytes = Entry.UncompressedBytes = Bytes.size();
				Entry.ObjectCount = 1;
				Entry.Dependencies = {"shared"};
				Entry.CoarseBounds = PackageCoarseBounds{{X - 0.5f, -0.5f, -0.5f}, {X + 0.5f, 0.5f, 0.5f}};
				Write(Entry.BlobReference, Encoded);
				Manifest.Entries.push_back(std::move(Entry));
				Object->Destroy();
			}
			auto Shared = std::make_shared<Folder>();
			Shared->SetName("StreamingShared");
			Shared->SetArchivable(true);
			const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Shared);
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
			PackageContentEntry Entry;
			Entry.Key = "shared";
			Entry.BlobReference = "content/shared.json";
			Entry.Digest = AssetContentId::Hash(Bytes);
			Entry.CompressedBytes = Entry.UncompressedBytes = Bytes.size();
			Entry.ObjectCount = 1;
			Write(Entry.BlobReference, Encoded);
			Manifest.Entries.push_back(std::move(Entry));
			Shared->Destroy();
			const auto ManifestText = EncodePackageContentManifest(Manifest);
			Digest = AssetContentId::Hash(std::span(
				reinterpret_cast<const std::uint8_t *>(ManifestText.data()), ManifestText.size()));
			Write("content/manifest.json", ManifestText);
		}

		~StreamingFixture() {
			std::error_code Ignored;
			for (const auto &File : Files) std::filesystem::remove(Root / File, Ignored);
			std::filesystem::remove(Root / "content", Ignored);
			std::filesystem::remove(Root, Ignored);
		}

		void Write(const std::filesystem::path &Relative, const std::string &Bytes) {
			std::ofstream Output(Root / Relative, std::ios::binary | std::ios::trunc);
			Check(Output.good(), "could not open streaming fixture payload");
			Output.write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
			Check(Output.good(), "could not write streaming fixture payload");
			if (std::ranges::find(Files, Relative) == Files.end()) Files.push_back(Relative);
		}

		std::string Read(const std::filesystem::path &Relative) const {
			std::ifstream Input(Root / Relative, std::ios::binary);
			Check(Input.good(), "could not read streaming fixture payload");
			return std::string(std::istreambuf_iterator<char>{Input}, std::istreambuf_iterator<char>{});
		}

		ContentAvailabilityConfiguration Configuration(std::shared_ptr<IContentAvailabilityProvider> Provider = {}) const {
			ContentAvailabilityConfiguration Result;
			Result.Provider = Provider ? std::move(Provider) :
				std::make_shared<LocalPackageContentProvider>(Root, "content/manifest.json", Digest);
			Result.Package = Manifest.Package;
			Result.ManifestDigest = Digest;
			Result.Mode = ContentResidencyMode::OnDemand;
			Result.Limits.WorkerCount = 1;
			Result.Limits.MaximumInFlight = 1;
			Result.Limits.MaximumPendingRequests = 1;
			Result.Limits.MaximumAdmissionUnitsPerTick = 1;
			return Result;
		}
	};

	struct GatedLocalProvider final : IContentAvailabilityProvider {
		std::shared_ptr<LocalPackageContentProvider> Local;
		std::atomic_bool BlockCellTwo{false};
		std::atomic_uint64_t BlockedCalls{0};
		explicit GatedLocalProvider(const StreamingFixture &Fixture)
			: Local(std::make_shared<LocalPackageContentProvider>(Fixture.Root, "content/manifest.json", Fixture.Digest)) {}
		std::string_view Name() const override { return "gated-local-package"; }
		ContentProviderLifecycleResult Start(const ContentRequestContext &Context) override { return Local->Start(Context); }
		void Stop() override { BlockCellTwo = false; Local->Stop(); }
		ContentManifestProviderResult GetManifest(const ContentRequestContext &Context,
			const PackageContentNamespace &Package) override { return Local->GetManifest(Context, Package); }
		ContentPayloadProviderResult GetContent(const ContentRequestContext &Context,
			const PackageContentIdentity &Identity) override {
			if (Identity.Key == "cell/02" && BlockCellTwo) {
				++BlockedCalls;
				while (BlockCellTwo && !Context.IsCancelled() && !Context.IsExpired()) std::this_thread::yield();
			}
			return Local->GetContent(Context, Identity);
		}
	};

	template <class Step, class Predicate> void Until(Step Advance, Predicate Complete, const char *Message) {
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < Deadline) {
			Advance();
			if (Complete()) return;
			std::this_thread::yield();
		}
		throw std::runtime_error(Message);
	}

	void LocalStreamingLifecycle() {
		StreamingFixture Fixture;
		auto Provider = std::make_shared<GatedLocalProvider>(Fixture);
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, Space, Fixture.Configuration(Provider));
		Until([&] { Service.Step(); }, [&] { return Service.IsManifestAvailable(); }, "local manifest unavailable");
		ResidencyDemandPolicyLimits Limits;
		Limits.GraceTicks = 3;
		Limits.MaximumDemandDeltasPerTick = 1;
		ResidencyDemandPolicy Policy(*Service.GetManifest(), Limits);
		std::uint64_t Tick = 0;
		auto Advance = [&] { Policy.Step(&Service, ++Tick); Service.Step(); };
		auto Resident = [&](std::string_view Key) { return Service.GetState(Key) == ContentResidencyState::Resident; };
		Check(Policy.UpdateFocus(1, {0, 0, 0}), "initial focus rejected");
		Until(Advance, [&] { return Resident("cell/00") && Resident("shared"); }, "first cell and dependency did not admit");
		Check(Space->FindFirstChild("StreamingUnit_0", false) && Space->FindFirstChild("StreamingShared", false),
			"admitted local content did not materialize");
		Check(Service.GetMetrics().Acquisitions == 2, "dependency closure acquired duplicate payloads");

		// Overlap uses one 3M owner. Releasing one focus cannot retire the other one's content.
		Check(Policy.UpdateFocus(2, {0, 0, 0}), "overlapping focus rejected");
		for (int Index = 0; Index < 4; ++Index) Advance();
		Check(Policy.GetMetrics().Requests == 1 && Service.GetMetrics().Acquisitions == 2,
			"overlapping focuses duplicated acquisition");
		Check(Policy.RemoveFocus(1), "first overlapping focus removal rejected");
		for (int Index = 0; Index < 5; ++Index) Advance();
		Check(Resident("cell/00"), "one focus removal retired overlapping demand");
		Check(Policy.UpdateFocus(2, {512, 0, 0}, {4096, 0, 0}), "high-speed focus rejected");
		Until(Advance, [&] { return Resident("cell/01"); }, "second cell did not admit after movement");
		Check(!Resident("cell/03"), "lookahead exceeded its bounded corridor");
		Check(Policy.UpdateFocus(2, {0, 0, 0}, {-4096, 0, 0}), "reverse focus rejected");
		Until(Advance, [&] { return Resident("cell/00"); }, "reversal did not retain or reacquire first cell");

		Provider->BlockCellTwo = true;
		Check(Policy.UpdateFocus(2, {1024, 0, 0}), "blocked provider focus rejected");
		Until(Advance, [&] { return Provider->BlockedCalls > 0; }, "controlled provider delay was not reached");
		Check(Policy.UpdateFocus(2, {2048, 0, 0}), "teleport focus rejected");
		for (int Index = 0; Index < 6; ++Index) Advance();
		Provider->BlockCellTwo = false;
		Until(Advance, [&] { return Resident("cell/04"); }, "teleport destination did not admit under request pressure");
		Check(!Space->FindFirstChild("StreamingUnit_2", false), "stale blocked provider completion admitted after teleport");
		Check(Service.GetMetrics().Cancellations + Service.GetMetrics().StaleCompletionsRejected > 0,
			"teleport did not retire the blocked request");

		const auto CorruptPath = std::filesystem::path("content/cell_03.json");
		const auto Original = Fixture.Read(CorruptPath);
		Check(std::filesystem::remove(Fixture.Root / CorruptPath), "could not remove missing-content fixture payload");
		Check(Policy.UpdateFocus(2, {1536, 0, 0}), "missing cell focus rejected");
		Until(Advance, [&] { return Service.GetState("cell/03") == ContentResidencyState::Failed; },
			"missing payload did not fail acquisition");
		Check(!Space->FindFirstChild("StreamingUnit_3", false), "missing payload crossed authoritative admission");
		const auto FailuresBeforeCorruption = Service.GetMetrics().Failures;
		Fixture.Write(CorruptPath, std::string(Original.size(), 'x'));
		Check(Service.RetryContent("cell/03"), "trusted corrupt-payload retry was rejected");
		Until(Advance, [&] { return Service.GetMetrics().Failures > FailuresBeforeCorruption &&
			Service.GetState("cell/03") == ContentResidencyState::Failed; },
			"corrupt payload did not fail verification");
		Check(!Space->FindFirstChild("StreamingUnit_3", false), "corrupt payload crossed authoritative admission");
		Fixture.Write(CorruptPath, Original);
		Check(Service.RetryContent("cell/03"), "trusted retry was rejected");
		Until(Advance, [&] { return Resident("cell/03"); }, "repaired payload did not admit on retry");
		Check(Policy.GetMetrics().Requests >= 4, "movement did not publish multiple cell demands");

		Check(Policy.RemoveFocus(2), "final focus removal rejected");
		for (int Index = 0; Index < 2; ++Index) Advance();
		Check(Resident("cell/03"), "grace retired last focus before deadline");
		Until(Advance, [&] { return Service.GetMetrics().ResidentUnits == 0 && Service.GetActiveRequestCount() == 0; },
			"focus retirement did not release content and dependency closure");
		Check(Service.GetMetrics().DecodedDocumentBytes == 0, "retired content retained decoded documents");
		std::cout << "[Content:StreamingScenario] local acquisitions=" << Service.GetMetrics().Acquisitions
			<< " admissions=" << Service.GetMetrics().Admissions
			<< " cancellations=" << Service.GetMetrics().Cancellations
			<< " failures=" << Service.GetMetrics().Failures
			<< " retries=2\n";
		const auto OldManifest = Service.GetManifestSnapshot();
		Check(static_cast<bool>(OldManifest), "old manifest snapshot unavailable");
		Policy.Stop(&Service);
		Service.Stop();
		World->Destroy();

		StreamingFixture Revision;
		++Revision.Manifest.Package.PackageVersion;
		const auto RevisionText = EncodePackageContentManifest(Revision.Manifest);
		Revision.Digest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(RevisionText.data()), RevisionText.size()));
		Revision.Write("content/manifest.json", RevisionText);
		auto RevisionWorld = std::make_shared<DataModel>();
		auto RevisionSpace = std::dynamic_pointer_cast<Workspace>(RevisionWorld->GetService("Workspace"));
		ContentAvailabilityService RevisionService(RevisionWorld, RevisionSpace, Revision.Configuration());
		Until([&] { RevisionService.Step(); }, [&] { return RevisionService.IsManifestAvailable(); },
			"replacement manifest unavailable");
		ResidencyDemandPolicy StalePolicy(OldManifest);
		Check(StalePolicy.SetRequirement("cell/00", ResidencyDemandClass::Critical), "old revision key rejected");
		StalePolicy.Step(&RevisionService, 1);
		Check(StalePolicy.GetMetrics().ServiceBindingFailures == 1 && RevisionService.GetActiveRequestCount() == 0,
			"old revision policy published into replacement service");
		StalePolicy.Stop(nullptr);
		RevisionService.Stop();
		RevisionWorld->Destroy();
	}

	void NetworkStreamingLifecycle() {
		StreamingFixture Fixture;
		auto World = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersValue->SetCharacterAutoLoads(false);
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, {}, {.Content = Fixture.Configuration(), .Mode = RuntimeMode::NetworkServer,
			.EnableResidencyDemandPolicy = true});
		Runtime.ProcessService->Alive = true;
		auto Network = SimulatedNetwork::Create({});
		GameSessionConfiguration ServerConfig;
		ServerConfig.Role = GameSessionRole::Server;
		ServerConfig.Endpoint = {"3m-streaming", 39002};
		ServerConfig.Limits = GameSessionConfiguration::DefaultLimits();
		ServerConfig.AllowInsecureDevelopmentNetwork = true;
		auto ClientConfig = ServerConfig;
		ClientConfig.Role = GameSessionRole::Client;
		GameSession Server(Network->CreateTransport(), ServerConfig, &Runtime);
		GameSession Client(Network->CreateTransport(), ClientConfig);
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "streaming sessions failed to start");
		auto Advance = [&] {
			Check(Network->Advance(16ms), "simulated transport clock failed");
			Network->Pump();
			(void)Server.Poll();
			(void)Client.Poll();
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
				ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient});
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "client runtime attachment failed");
			}
			if (ClientRuntime) ClientRuntime->Step();
			Runtime.Step();
			Server.Step(Runtime.GetSimulationTick());
			Client.Step(Runtime.GetSimulationTick());
		};
		Until(Advance, [&] { return Client.GetStatus() == GameSessionStatus::Ready &&
			Runtime.GetResidencyDemandPolicy() && ClientRuntime; }, "network streaming session did not become ready");
		Check(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0,
			"connected client without authoritative Character gained policy focus");
		using Clock = std::chrono::steady_clock;
		auto WaitForUnit = [&](std::string_view Marker, std::string_view Key, const char *Name,
			Clock::time_point Trigger, const char *Message) {
			std::optional<Clock::time_point> CasResident;
			std::optional<Clock::time_point> ClientMaterialized;
			std::size_t Observations = 0;
			Until(Advance, [&] {
				++Observations;
				const auto Observed = Clock::now();
				const bool IsResident = Runtime.Content->GetState(Key) == ContentResidencyState::Resident;
				const bool IsMaterialized = ClientRuntime->Workspace->FindFirstChild(Name, false) != nullptr;
				if (!CasResident && IsResident)
					CasResident = Observed;
				if (!ClientMaterialized && IsMaterialized)
					ClientMaterialized = Observed;
				return IsResident && IsMaterialized;
			}, Message);
			std::cout << "[Content:StreamingScenario] movement=" << Marker
				<< " casResidentUs=" << std::chrono::duration_cast<std::chrono::microseconds>(*CasResident - Trigger).count()
				<< " clientMaterializedUs=" << std::chrono::duration_cast<std::chrono::microseconds>(
					*ClientMaterialized - Trigger).count()
				<< " observations=" << Observations << " marker=simulated-network-observation\n";
		};
		const auto PlayerValue = Runtime.Players->GetPlayers().front();
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		auto RootPart = std::make_shared<Part>();
		RootPart->SetAnchored(true);
		RootPart->SetCanCollide(false);
		RootPart->SetParent(CharacterValue);
		CharacterValue->SetRootPart(RootPart);
		CharacterValue->ApplyRuntimeTransform(CFrame(0, 0, 0));
		CharacterValue->SetParent(Runtime.Workspace);
		const auto InitialTrigger = Clock::now();
		PlayerValue->SetCharacter(CharacterValue);
		WaitForUnit("initial", "cell/00", "StreamingUnit_0", InitialTrigger,
			"first admitted cell did not reach accepted client materialization");
		Check(Server.GetMetrics().ReadyPeers == 1, "content admission disrupted ready peer");
		const auto TraverseTrigger = Clock::now();
		CharacterValue->ApplyRuntimeTransform(CFrame(1024, 0, 0));
		WaitForUnit("traverse", "cell/02", "StreamingUnit_2", TraverseTrigger,
			"second cell did not reach accepted client materialization");
		const auto TeleportTrigger = Clock::now();
		CharacterValue->ApplyRuntimeTransform(CFrame(2048, 0, 0));
		WaitForUnit("teleport", "cell/04", "StreamingUnit_4", TeleportTrigger,
			"teleport cell did not reach accepted client materialization");
		Check(Server.GetMetrics().SpatialMembershipCount >= 2,
			"admitted Parts did not enter normal spatial projection");
		Client.Stop();
		for (int Index = 0; Index < 40; ++Index) Advance();
		Check(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0 && Server.GetMetrics().ReadyPeers == 0,
			"disconnect retained generation-scoped network focus or ready peer");
		ClientRuntime->Destroy();
		ClientRuntime.reset();
		ClientRenderer.reset();
		for (int Cycle = 0; Cycle < 3; ++Cycle) {
			GameSession Replacement(Network->CreateTransport(), ClientConfig);
			std::unique_ptr<HeadlessRenderer> ReplacementRenderer;
			std::unique_ptr<Engine> ReplacementRuntime;
			Check(Replacement.Start().Succeeded(), "replacement client session failed to start");
			auto AdvanceReplacement = [&] {
				Check(Network->Advance(16ms), "replacement transport clock failed");
				Network->Pump();
				(void)Server.Poll();
				(void)Replacement.Poll();
				if (!ReplacementRuntime && Replacement.GetClientDataModel()) {
					ReplacementRenderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
					ReplacementRuntime = std::make_unique<Engine>(Replacement.GetClientDataModel(),
						ReplacementRenderer.get(), nullptr, EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient});
					ReplacementRuntime->ProcessService->Alive = true;
					Check(Replacement.AttachClientRuntime(*ReplacementRuntime), "replacement runtime attachment failed");
				}
				if (ReplacementRuntime) ReplacementRuntime->Step();
				Runtime.Step();
				Server.Step(Runtime.GetSimulationTick());
				Replacement.Step(Runtime.GetSimulationTick());
			};
			Until(AdvanceReplacement, [&] { return Replacement.GetStatus() == GameSessionStatus::Ready &&
				ReplacementRuntime && !Runtime.Players->GetPlayers().empty(); }, "replacement session did not become ready");
			const auto ReplacementPlayer = Runtime.Players->GetPlayers().back();
			auto ReplacementCharacter = std::make_shared<KinematicCharacter>();
			auto ReplacementRoot = std::make_shared<Part>();
			ReplacementRoot->SetAnchored(true);
			ReplacementRoot->SetCanCollide(false);
			ReplacementRoot->SetParent(ReplacementCharacter);
			ReplacementCharacter->SetRootPart(ReplacementRoot);
			const bool FarCell = Cycle % 2 == 0;
			ReplacementCharacter->ApplyRuntimeTransform(CFrame(FarCell ? 2048 : 0, 0, 0));
			ReplacementCharacter->SetParent(Runtime.Workspace);
			ReplacementPlayer->SetCharacter(ReplacementCharacter);
			Until(AdvanceReplacement, [&] { return Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 1 &&
				ReplacementRuntime->Workspace->FindFirstChild(FarCell ? "StreamingUnit_4" : "StreamingUnit_0", false); },
				"replacement session did not receive current admitted cell");
			Check(Server.GetMetrics().ReadyPeers == 1, "replacement materialization disrupted ready peer");
			std::cout << "[Content:StreamingScenario] reconnectCycle=" << Cycle + 1
				<< " cell=" << (FarCell ? 4 : 0)
				<< " focuses=" << Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses
				<< " readyPeers=" << Server.GetMetrics().ReadyPeers << '\n';
			Replacement.Stop();
			for (int Index = 0; Index < 40; ++Index) AdvanceReplacement();
			Check(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0 && Server.GetMetrics().ReadyPeers == 0,
				"replacement disconnect retained focus or ready peer before next session");
			std::cout << "[Content:StreamingScenario] reconnectCycle=" << Cycle + 1
				<< " teardownFocuses=" << Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses
				<< " teardownReadyPeers=" << Server.GetMetrics().ReadyPeers << '\n';
			if (ReplacementRuntime) ReplacementRuntime->Destroy();
		}
		std::cout << "[Content:StreamingScenario] reconnectCycles=3"
			<< " spatialMembership=" << Server.GetMetrics().SpatialMembershipCount
			<< " admissions=" << Runtime.Content->GetMetrics().Admissions << '\n';
		Server.Stop();
		Runtime.Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		LocalStreamingLifecycle();
		NetworkStreamingLifecycle();
		std::cout << "[Content:StreamingScenario] passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:StreamingScenario] " << Error.what() << '\n';
		return 1;
	} catch (...) {
		std::cerr << "[Content:StreamingScenario] unknown failure\n";
		return 1;
	}
}

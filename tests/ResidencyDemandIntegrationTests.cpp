#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	struct LocalFixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-3m-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		PackageContentManifest Manifest;
		AssetContentId Digest;

		LocalFixture() {
			std::filesystem::create_directories(Root / "content");
			Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 29};
			for (const auto *Key : {"a", "ahead", "b", "shared"}) {
				std::shared_ptr<Instance> Object = std::make_shared<Folder>();
				Object->SetName(std::string("Unit_") + Key);
				Object->SetArchivable(true);
				const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Object);
				const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
				PackageContentEntry Entry;
				Entry.Key = Key;
				Entry.BlobReference = std::string("content/") + Key + ".json";
				Entry.Digest = AssetContentId::Hash(Bytes);
				Entry.CompressedBytes = Entry.UncompressedBytes = Bytes.size();
				Entry.ObjectCount = 1;
				if (Entry.Key != "shared") {
					Entry.Dependencies = {"shared"};
					const float X = Entry.Key == "a" ? 0.0f : Entry.Key == "ahead" ? 360.0f : 2048.0f;
					Entry.CoarseBounds = PackageCoarseBounds{{X, 0, 0}, {X + 1, 1, 1}};
				}
				std::ofstream Output(Root / Entry.BlobReference, std::ios::binary);
				Output << Encoded;
				Manifest.Entries.push_back(std::move(Entry));
				Object->Destroy();
			}
			const auto Encoded = EncodePackageContentManifest(Manifest);
			Digest = AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()));
			std::ofstream Output(Root / "content/manifest.json", std::ios::binary);
			Output << Encoded;
		}
		~LocalFixture() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
		ContentAvailabilityConfiguration Configuration() const {
			return {.Provider = std::make_shared<LocalPackageContentProvider>(Root, "content/manifest.json", Digest),
				.Package = Manifest.Package, .ManifestDigest = Digest, .Mode = ContentResidencyMode::OnDemand};
		}
	};

	template <class Predicate> void Pump(ContentAvailabilityService &Service, Predicate Complete) {
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < Deadline) {
			Service.Step();
			if (Complete()) return;
			std::this_thread::yield();
		}
		throw std::runtime_error("content service did not complete bounded integration fixture");
	}

	void LocalLifecycle() {
		LocalFixture Fixture;
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, Space, Fixture.Configuration());
		Pump(Service, [&] { return Service.IsManifestAvailable(); });
		ResidencyDemandPolicyLimits Limits;
		Limits.GraceTicks = 3;
		Limits.MaximumDemandDeltasPerTick = 1;
		ResidencyDemandPolicy Policy(*Service.GetManifest(), Limits);
		for (std::uint64_t Id = 1; Id <= 500; ++Id)
			Check(Policy.UpdateFocus(Id, {0, 0, 0}), "trusted duplicate focus rejected");
		for (std::uint64_t Tick = 1; Tick <= 40; ++Tick) Policy.Step(&Service, Tick);
		Check(Policy.GetMetrics().Requests == 1 && Service.GetMetrics().Requests == 1,
			"duplicate spatial demand did not coalesce to one owned request");
		Check(!Space->FindFirstChild("Unit_a", false), "policy created an authoritative object before 3L admission");
		Pump(Service, [&] { return Service.GetState("a") == ContentResidencyState::Resident; });
		Check(Service.GetState("shared") == ContentResidencyState::Resident &&
			Service.GetMetrics().Acquisitions == 2 && Service.GetMetrics().Admissions == 2,
			"existing 3L dependency closure/acquisition was not preserved");
		const auto OldRoot = Space->FindFirstChild("Unit_a", false);
		const auto OldId = OldRoot->GetObjectId();
		Check(Service.RequestContent("a"), "independent native demand rejected");
		for (std::uint64_t Id = 1; Id <= 500; ++Id) Check(Policy.RemoveFocus(Id), "focus removal rejected");
		Policy.Step(&Service, 41);
		Policy.Step(&Service, 42);
		Check(Policy.GetMetrics().Releases == 0, "grace released demand before its deadline");
		Policy.Step(&Service, 43);
		Check(Policy.GetMetrics().Releases == 1, "policy did not release exactly its owned demand");
		for (int Attempt = 0; Attempt < 4; ++Attempt) Service.Step();
		Check(Service.GetState("a") == ContentResidencyState::Resident,
			"policy release removed another native owner's demand");
		Check(Service.ReleaseContent("a"), "independent demand release rejected");
		Pump(Service, [&] { return Service.GetState("a") == ContentResidencyState::Unavailable &&
			Service.GetState("shared") == ContentResidencyState::Unavailable; });
		Check(!ObjectRegistry::Get().Lookup(OldId), "retired content ObjectId still resolves");

		Check(Policy.UpdateFocus(501, {2048, 0, 0}), "teleport focus rejected");
		Policy.Step(&Service, 44);
		Pump(Service, [&] { return Service.GetState("b") == ContentResidencyState::Resident; });
		Check(!Space->FindFirstChild("Unit_a", false), "teleport reacquired stale old content");
		Check(Policy.UpdateFocus(501, {0, 0, 0}), "cancel/reacquire focus rejected");
		Policy.Step(&Service, 45);
		Pump(Service, [&] { return Service.GetState("a") == ContentResidencyState::Resident; });
		Check(Space->FindFirstChild("Unit_a", false)->GetObjectId() != OldId,
			"reacquisition reused a retired runtime identity");
		Policy.Stop(&Service);
		Pump(Service, [&] { return Service.GetMetrics().ResidentUnits == 0; });
		Check(Service.GetMetrics().DecodedDocumentBytes == 0 && Service.GetActiveRequestCount() == 0,
			"policy stop retained active demand or decoded resources");
		const auto Snapshot = Service.GetManifestSnapshot();
		Service.Stop();
		Check(Snapshot && Snapshot->Entries.size() == 4 && !Service.GetManifestSnapshot(),
			"immutable preparation metadata did not survive direct service stop safely");
		ResidencyDemandPolicy StoppedPreparation(*Snapshot);
		Check(StoppedPreparation.SetRequirement("a", ResidencyDemandClass::Critical), "stopped snapshot fixture rejected key");
		StoppedPreparation.Step(&Service, 1);
		Check(StoppedPreparation.GetMetrics().Requests == 0, "stopped service accepted old snapshot preparation");
		StoppedPreparation.Stop(nullptr);
		World->Destroy();
	}

	void ProductionFocusFeed() {
		LocalFixture Fixture;
		auto World = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersValue->SetCharacterAutoLoads(false);
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		auto Probe = std::make_shared<RemoteEvent>();
		Probe->SetName("ResidencyProbe");
		Probe->SetParent(World);
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, {}, {.Content = Fixture.Configuration(), .Mode = RuntimeMode::NetworkServer,
			.EnableResidencyDemandPolicy = true});
		Runtime.ProcessService->Alive = true;
		auto Network = SimulatedNetwork::Create({});
		GameSessionConfiguration ServerConfig;
		ServerConfig.Role = GameSessionRole::Server;
		ServerConfig.Endpoint = {"3m-focus", 39001};
		ServerConfig.Limits = GameSessionConfiguration::DefaultLimits();
		ServerConfig.AllowInsecureDevelopmentNetwork = true;
		auto ClientConfig = ServerConfig;
		ClientConfig.Role = GameSessionRole::Client;
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		GameSession Server(Network->CreateTransport(), ServerConfig, &Runtime);
		GameSession Client(Network->CreateTransport(), ClientConfig);
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "production focus sessions failed to start");
		auto Step = [&] {
			Check(Network->Advance(16ms), "simulated transport clock failed");
			Network->Pump();
			(void)Server.Poll(); (void)Client.Poll();
			if (!ClientRuntime && Client.GetClientDataModel()) {
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
				ClientRuntime = std::make_unique<Engine>(Client.GetClientDataModel(), ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient});
				ClientRuntime->ProcessService->Alive = true;
				Check(Client.AttachClientRuntime(*ClientRuntime), "production focus client runtime failed to attach");
			}
			if (ClientRuntime) ClientRuntime->Step();
			Runtime.Step();
			Server.Step(Runtime.GetSimulationTick()); Client.Step(Runtime.GetSimulationTick());
			std::this_thread::yield();
		};
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while ((Client.GetStatus() != GameSessionStatus::Ready || !Runtime.GetResidencyDemandPolicy()) &&
			std::chrono::steady_clock::now() < Deadline) Step();
		Check(Client.GetStatus() == GameSessionStatus::Ready && Runtime.GetResidencyDemandPolicy(),
			"default server on-demand policy was not composed");
		Check(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0,
			"an accepted peer without authoritative Character gained spatial demand");
		const auto PlayerValue = Runtime.Players->GetPlayers().front();
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		CharacterValue->ApplyRuntimeTransform(CFrame(0, 0, 0));
		CharacterValue->SetParent(Runtime.Workspace);
		PlayerValue->SetCharacter(CharacterValue);
		while (Runtime.Content->GetState("a") != ContentResidencyState::Resident &&
			std::chrono::steady_clock::now() < Deadline) Step();
		Check(Runtime.Content->GetState("a") == ContentResidencyState::Resident &&
			Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 1,
			"committed server Character did not drive production residency demand");
		Check(Server.GetMetrics().ReadyPeers == 1, "content admission disrupted the accepted peer");
		Check(Runtime.Content->GetState("ahead") != ContentResidencyState::Resident,
			"static Character acquired outside-entry content without a predictive reason");
		while (Runtime.Content->GetState("ahead") != ContentResidencyState::Resident &&
			std::chrono::steady_clock::now() < Deadline) {
			CharacterValue->ApplyRuntimeControllerFacts({256, 0, 0}, {0, 1, 0}, true);
			Step();
		}
		Check(Runtime.Content->GetState("ahead") == ContentResidencyState::Resident,
			"authoritative controller velocity did not drive the production predictive corridor");
		const auto ClientProbe = std::dynamic_pointer_cast<RemoteEvent>(
			ClientRuntime->DataModel->FindFirstChild("ResidencyProbe", false));
		Check(ClientProbe && ClientProbe->GetRemoteManager() && Probe->GetRemoteManager(),
			"normal accepted materialization did not expose the service probe Remote");
		std::size_t Replies = 0;
		bool ReplyRejected = false;
		Check(ClientProbe->GetRemoteManager()->SetEventHandler(ClientProbe->GetNetworkObjectId(),
			[&](const RemoteInvocation &) { ++Replies; }), "client probe handler registration failed");
		Check(Probe->GetRemoteManager()->SetEventHandler(Probe->GetNetworkObjectId(),
			[&](const RemoteInvocation &Invocation) {
				if (!Probe->GetRemoteManager()->SendEvent(Invocation.Peer.Connection,
					Probe->GetNetworkObjectId(), Invocation.Arguments).Accepted()) ReplyRejected = true;
			}), "server probe handler registration failed");
		const auto CharacterBefore = Server.GetMetrics().CharacterPublicationStatesAccepted;
		std::size_t Sent = 0;
		std::chrono::nanoseconds MaximumFrame{};
		for (std::uint64_t Tick = 0; Tick < 192; ++Tick) {
			const auto Started = std::chrono::steady_clock::now();
			for (std::uint64_t Focus = 1; Focus <= 499; ++Focus)
				Check(Runtime.GetResidencyDemandPolicy()->UpdateFocus(0xffff'ffff'ffff'ffffULL - Focus,
					{Tick % 2 ? 1.0f : 0.0f, 0, 0}), "server gameplay focus load rejected");
			if (Tick % 8 == 0) {
				Check(ClientProbe->GetRemoteManager()->SendEvent(*ClientProbe->GetDefaultServerConnection(),
					ClientProbe->GetNetworkObjectId(), {WireValue(static_cast<int>(++Sent))}).Accepted(),
					"ordinary Remote request was starved by policy work");
			}
			Step();
			MaximumFrame = std::max(MaximumFrame, std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Started));
		}
		for (int Tick = 0; Tick < 8; ++Tick) Step();
		Check(!ReplyRejected && Replies == Sent && Server.GetMetrics().ReadyPeers == 1 &&
			Server.GetMetrics().CharacterPublicationStatesAccepted > CharacterBefore,
			"Character or reliable Remote service failed to progress under 500 moving policy focuses");
		std::cout << "[Content:ResidencyService] focuses=500 remoteSent=" << Sent << " remoteReplies=" << Replies
			<< " characterAccepted=" << Server.GetMetrics().CharacterPublicationStatesAccepted - CharacterBefore
			<< " maxFrameUs=" << std::chrono::duration<double, std::micro>(MaximumFrame).count() << '\n';
		for (std::uint64_t Focus = 1; Focus <= 499; ++Focus)
			Check(Runtime.GetResidencyDemandPolicy()->RemoveFocus(0xffff'ffff'ffff'ffffULL - Focus), "gameplay focus removal failed");
		Client.Stop();
		for (int Attempt = 0; Attempt < 40; ++Attempt) Step();
		Check(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0,
			"disconnect left a stale generation-scoped focus");
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		Runtime.Destroy();
	}

	void FailureAndStalePending() {
		LocalFixture Fixture;
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, Space, Fixture.Configuration());
		Pump(Service, [&] { return Service.IsManifestAvailable(); });
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumFocusEvaluationsPerTick = 1;
		Limits.MaximumDemandDeltasPerTick = 1;
		Limits.GraceTicks = 0;
		ResidencyDemandPolicy Policy(*Service.GetManifest(), Limits);
		for (std::uint64_t Id = 1; Id <= 32; ++Id) Check(Policy.UpdateFocus(Id, {0, 0, 0}), "stale fixture focus rejected");
		for (std::uint64_t Tick = 1; Tick <= 32; ++Tick) Policy.Step(nullptr, Tick);
		for (std::uint64_t Id = 1; Id <= 32; ++Id) Check(Policy.UpdateFocus(Id, {2048, 0, 0}), "stale fixture teleport rejected");
		for (std::uint64_t Tick = 33; Tick <= 65; ++Tick) {
			Policy.Step(&Service, Tick);
			Check(Service.GetState("a") == ContentResidencyState::Unavailable,
				"a stale pending request escaped while other focuses awaited revalidation");
		}
		// The production provider now fails digest validation; repair and explicit
		// trusted 3L retry must recover without a second policy ownership counter.
		const auto Path = Fixture.Root / "content/b.json";
		std::ifstream Input(Path, std::ios::binary);
		const std::string Original(std::istreambuf_iterator<char>{Input}, {});
		Input.close();
		{ std::ofstream Output(Path, std::ios::binary); Output << std::string(Original.size(), 'x'); }
		Pump(Service, [&] { return Service.GetState("b") == ContentResidencyState::Failed; });
		Check(!Space->FindFirstChild("Unit_b", false), "corrupt content crossed authoritative admission");
		{ std::ofstream Output(Path, std::ios::binary); Output << Original; }
		Check(Service.RetryContent("b"), "trusted retry of failed policy content was rejected");
		Pump(Service, [&] { return Service.GetState("b") == ContentResidencyState::Resident; });
		Check(Policy.GetMetrics().Requests == 1, "provider recovery duplicated policy-owned demand");
		Policy.Stop(&Service);
		Pump(Service, [&] { return Service.GetMetrics().ResidentUnits == 0; });
		Service.Stop();
		World->Destroy();
	}

	void PublicationAndManifestIdentity() {
		LocalFixture Fixture;
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, Space, Fixture.Configuration());
		Pump(Service, [&] { return Service.IsManifestAvailable(); });
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumDemandDeltasPerTick = 1;
		ResidencyDemandPolicy Policy(*Service.GetManifest(), Limits);
		Check(Policy.SetRequirement("a", ResidencyDemandClass::Presentation, 1) &&
			Policy.SetRequirement("b", ResidencyDemandClass::Critical, 10) &&
			Policy.SetRequirement("ahead", ResidencyDemandClass::Critical, 5), "priority fixture requirement rejected");
		Policy.Step(&Service, 1);
		Check(Service.GetState("ahead") == ContentResidencyState::Requested &&
			Service.GetState("b") == ContentResidencyState::Unavailable &&
			Service.GetState("a") == ContentResidencyState::Unavailable,
			"semantic class and deadline did not determine actual bounded request publication");
		Policy.Step(&Service, 2);
		Check(Service.GetState("b") == ContentResidencyState::Requested &&
			Service.GetState("a") == ContentResidencyState::Unavailable, "critical request lost publication priority");

		LocalFixture Replacement;
		Replacement.Manifest.Entries.front().Dependencies.clear();
		const auto Changed = EncodePackageContentManifest(Replacement.Manifest);
		Replacement.Digest = AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Changed.data()), Changed.size()));
		{ std::ofstream Output(Replacement.Root / "content/manifest.json", std::ios::binary); Output << Changed; }
		auto NewWorld = std::make_shared<DataModel>();
		auto NewSpace = std::dynamic_pointer_cast<Workspace>(NewWorld->GetService("Workspace"));
		ContentAvailabilityService NewService(NewWorld, NewSpace, Replacement.Configuration());
		Pump(NewService, [&] { return NewService.IsManifestAvailable(); });
		Policy.Step(&NewService, 3);
		Check(Policy.GetMetrics().ServiceBindingFailures == 1 && NewService.GetActiveRequestCount() == 0,
			"policy published into a replacement service lifetime");
		ResidencyDemandPolicy Unbound(*Service.GetManifest(), Limits);
		Check(Unbound.SetRequirement("a", ResidencyDemandClass::Critical), "manifest replacement fixture rejected key");
		Unbound.Step(&NewService, 1);
		Check(Unbound.GetMetrics().ServiceBindingFailures == 1 && NewService.GetActiveRequestCount() == 0,
			"changed dependency declaration bypassed exact manifest binding in the same package namespace");
		Unbound.Stop(nullptr);
		Policy.Stop(&Service);
		NewService.Stop(); Service.Stop();
		NewWorld->Destroy(); World->Destroy();
	}

	void PreparationStop() {
		LocalFixture Fixture;
		for (int Attempt = 0; Attempt < 8; ++Attempt) {
			auto World = std::make_shared<DataModel>();
			auto PlayersValue = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
			PlayersValue->SetDefaultControllerEnabled(false);
			PlayersValue->SetDefaultCameraEnabled(false);
			HeadlessRenderer Renderer(Vector2(32, 32));
			Engine Runtime(World, &Renderer, {}, {.Content = Fixture.Configuration(), .Mode = RuntimeMode::NetworkServer,
				.EnableResidencyDemandPolicy = true});
			Runtime.ProcessService->Alive = true;
			Pump(*Runtime.Content, [&] { return Runtime.Content->IsManifestAvailable(); });
			Runtime.Step();
			Runtime.Content->Stop();
			for (int Tick = 0; Tick < 32; ++Tick) {
				Runtime.Step();
				Check(!Runtime.GetResidencyDemandPolicy(), "stale asynchronous preparation published after direct content stop");
				std::this_thread::yield();
			}
			Runtime.Destroy();
		}
	}

	void PriorityBacklog() {
		LocalFixture Fixture;
		auto Prototype = Fixture.Manifest.Entries.front();
		Prototype.Dependencies.clear();
		Prototype.CoarseBounds.reset();
		Fixture.Manifest.Entries.clear();
		for (int Index = 0; Index < 32; ++Index) {
			auto Entry = Prototype;
			Entry.Key = "rank/" + std::string(Index < 10 ? "0" : "") + std::to_string(Index);
			Fixture.Manifest.Entries.push_back(std::move(Entry));
		}
		const auto Encoded = EncodePackageContentManifest(Fixture.Manifest);
		Fixture.Digest = AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()));
		{ std::ofstream Output(Fixture.Root / "content/manifest.json", std::ios::binary); Output << Encoded; }
		auto World = std::make_shared<DataModel>();
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, Space, Fixture.Configuration());
		Pump(Service, [&] { return Service.IsManifestAvailable(); });
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumDemandDeltasPerTick = 1;
		Limits.GraceTicks = 0;
		ResidencyDemandPolicy Policy(*Service.GetManifest(), Limits);
		for (int Index = 23; Index >= 0; --Index)
			Check(Policy.SetRequirement(Fixture.Manifest.Entries[Index].Key, ResidencyDemandClass::Critical, Index + 1),
				"backlog priority requirement rejected");
		for (std::uint64_t Tick = 1; Tick <= 24; ++Tick) {
			Policy.Step(&Service, Tick);
			Check(Service.GetState(Fixture.Manifest.Entries[Tick - 1].Key) == ContentResidencyState::Requested,
				"bounded request window skipped an earlier eligible critical deadline");
			if (Tick < 24)
				Check(Service.GetState(Fixture.Manifest.Entries[Tick].Key) == ContentResidencyState::Unavailable,
					"publication exceeded its single-delta budget");
		}
		for (int Index = 0; Index < 24; ++Index)
			Check(Policy.ClearRequirement(Fixture.Manifest.Entries[Index].Key), "backlog release rejected");
		Check(Policy.SetRequirement("rank/31", ResidencyDemandClass::Critical, 0), "new critical requirement rejected");
		Policy.Step(&Service, 25);
		Policy.Step(&Service, 26);
		Check(Service.GetState("rank/31") == ContentResidencyState::Requested && Policy.GetMetrics().Releases >= 1,
			"release backlog starved a new critical request or lost release progress");
		Policy.Stop(&Service);
		Service.Stop(); World->Destroy();
	}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		LocalLifecycle();
		FailureAndStalePending();
		PublicationAndManifestIdentity();
		PreparationStop();
		PriorityBacklog();
		ProductionFocusFeed();
		std::cout << "[Content:ResidencyIntegration] passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ResidencyIntegration] " << Error.what() << '\n';
		return 1;
	}
}

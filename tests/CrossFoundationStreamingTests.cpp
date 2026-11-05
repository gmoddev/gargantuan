#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess {
		static PhysicsCollisionResidency Collision(const WorldRoot &World) {
			return World.Physics.GetCollisionResidency();
		}
		static PhysicsBodyId Body(const WorldRoot &World, const BasePart &Part) {
			const auto Found = World.PartBodies.find(Part.GetObjectId());
			return Found == World.PartBodies.end() ? PhysicsBodyId{} : Found->second;
		}
		static bool ValidBody(const WorldRoot &World, PhysicsBodyId Id) { return World.Physics.IsBodyValid(Id); }
		static WorldRaycastResult Raycast(WorldRoot &World, glm::vec3 Origin, glm::vec3 Direction) {
			return World.ResolveRaycast(Origin, Direction);
		}
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	double Microseconds(Clock::time_point Started) {
		return std::chrono::duration<double, std::micro>(Clock::now() - Started).count();
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Value) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Value.data(), static_cast<std::streamsize>(Value.size()));
		Require(static_cast<bool>(Output), "fixture source write failed");
	}

	std::string ReadText(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		Require(Input.good(), "fixture package input could not be read");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	AssetContentId Digest(std::string_view Bytes) {
		return AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Bytes.data()), Bytes.size()));
	}

	void CheckHistoricalUnderbound(const std::filesystem::path &Root, std::string_view Key) {
		using Json = nlohmann::ordered_json;
		const auto ManifestPath = Root / "content/content.manifest.json";
		const auto PackagePath = Root / "game.package.json";
		const auto OriginalManifest = ReadText(ManifestPath);
		const auto OriginalPackage = ReadText(PackagePath);
		auto Manifest = ParsePackageContentManifest(OriginalManifest);
		Require(Manifest.has_value(), "historical underbound fixture lacks a valid manifest");
		auto Entry = std::ranges::find(Manifest->Entries, Key, &PackageContentEntry::Key);
		Require(Entry != Manifest->Entries.end(), "historical underbound fixture lost its offset unit");
		// Reproduce the pre-integration Size-box approximation for roots at 0/20.
		Entry->CoarseBounds = PackageCoarseBounds{{-0.5f, -0.5f, -0.5f}, {20.5f, 0.5f, 0.5f}};
		const auto Underbound = EncodePackageContentManifest(*Manifest);
		Require(ParsePackageContentManifest(Underbound).has_value(),
			"historical underbound fixture failed ordinary manifest shape validation");
		auto Package = Json::parse(OriginalPackage);
		Require(Package["PackageFormatVersion"] == GamePackageFormatVersion,
			"historical regression no longer represents the current package format");
		bool Rehashed = false;
		for (auto &File : Package["Content"])
			if (File["Path"] == "content/content.manifest.json") {
				File["Size"] = Underbound.size();
				File["Sha256"] = Digest(Underbound).ToString();
				Rehashed = true;
			}
		Require(Rehashed, "historical underbound fixture lacks its content-table record");
		Package["ContentTableSha256"] = Digest(Package["Content"].dump()).ToString();
		WriteText(ManifestPath, Underbound);
		WriteText(PackagePath, Package.dump());
		const auto Diagnostics = PackageBuilder::Validate(Root);
		std::vector<PackageDiagnostic> LoadDiagnostics;
		const auto Loaded = PackageBuilder::Load(Root, LoadDiagnostics);
		WriteText(ManifestPath, OriginalManifest);
		WriteText(PackagePath, OriginalPackage);
		Require(std::ranges::any_of(Diagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error &&
				Diagnostic.Code == "InvalidSpatialMetadata";
		}), "rehashing the historical underbound bypassed full package geometry validation");
		Require(!Loaded && std::ranges::any_of(LoadDiagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error &&
				Diagnostic.Code == "InvalidSpatialMetadata";
		}), "official package startup did not deny the rehashed underbound by spatial proof");
	}

	bool HasPackageError(const std::vector<PackageDiagnostic> &Diagnostics) {
		return std::ranges::any_of(Diagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		});
	}

	struct FixtureDirectory final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("g-cf-stream-" + std::to_string(Clock::now().time_since_epoch().count()));
		std::filesystem::path ProjectRoot = Root / "Project";
		std::filesystem::path PackageRoot = Root / "Release";
		FixtureDirectory() {
			std::filesystem::create_directory(Root);
			std::filesystem::copy(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), ProjectRoot,
				std::filesystem::copy_options::recursive);
		}
		~FixtureDirectory() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	};

	std::shared_ptr<MeshPart> AddMeshPart(const std::shared_ptr<Folder> &Unit, std::string Name,
		std::string_view Reference, glm::vec3 Position) {
		auto Part = std::make_shared<MeshPart>();
		Part->SetName(std::move(Name));
		Part->SetArchivable(true);
		Part->SetAnchored(true);
		Part->SetCanCollide(true);
		Part->SetSize({1.0f, 1.0f, 1.0f});
		Part->SetCFrame(CFrame(Position.x, Position.y, Position.z));
		Part->SetMesh(std::string(Reference));
		Part->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Part->SetParent(Unit);
		return Part;
	}

	std::shared_ptr<Folder> AddUnit(const std::shared_ptr<Workspace> &Space, std::string Name) {
		auto Unit = std::make_shared<Folder>();
		Unit->SetName(std::move(Name));
		Unit->SetArchivable(true);
		Unit->SetParent(Space);
		return Unit;
	}

	std::string FindUnitKey(const GamePayload &Payload, std::string_view Name) {
		for (const auto &Unit : Payload.ContentUnits)
			if (Unit.Payload.find(Name) != std::string::npos) return Unit.Entry.Key;
		return {};
	}

	const PackageContentEntry &FindEntry(const PackageContentManifest &Manifest, std::string_view Key) {
		const auto Found = std::ranges::find_if(Manifest.Entries,
			[Key](const PackageContentEntry &Entry) { return Entry.Key == Key; });
		Require(Found != Manifest.Entries.end(), "packaged content key missing from manifest");
		return *Found;
	}

	template <typename Predicate>
	void Pump(ContentAvailabilityService &Service, ResidencyDemandPolicy *Policy, std::uint64_t &Tick,
		Predicate Complete, std::string_view Failure) {
		const auto Deadline = Clock::now() + 10s;
		while (Clock::now() < Deadline) {
			if (Policy) Policy->Step(&Service, ++Tick);
			Service.Step();
			if (Complete()) return;
			std::this_thread::sleep_for(1ms);
		}
		throw std::runtime_error(std::string(Failure));
	}

	std::size_t CountCreates(const RenderPublication &Publication, ObjectId First, ObjectId Second = {}) {
		return std::ranges::count_if(Publication.Creates, [&](const RenderObjectCreate &Create) {
			return Create.Item.Object == First || (Second.IsValid() && Create.Item.Object == Second);
		});
	}

	std::size_t CountRemoves(const RenderPublication &Publication, ObjectId First, ObjectId Second = {}) {
		return std::ranges::count_if(Publication.Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == First || (Second.IsValid() && Remove.Object == Second);
		});
	}

	void RunGameSessionPhase(const RuntimePackagePayload &Loaded, const std::filesystem::path &PackageRoot,
		std::string_view OffsetKey, std::string_view SolidKey, std::string_view OffsetMeshReference) {
		using namespace gargantuan::network;
		auto ServerWorld = PackageBuilder::LoadWorld(Loaded, PackageRoot);
		auto PlayersValue = std::dynamic_pointer_cast<Players>(ServerWorld->GetService("Players"));
		Require(PlayersValue != nullptr, "packaged server has no Players service");
		PlayersValue->SetCharacterAutoLoads(false);
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		auto ServerContent = PackageBuilder::GetLocalContentConfiguration(Loaded, PackageRoot,
			ContentResidencyMode::OnDemand);
		Require(ServerContent.has_value(), "packaged server has no local on-demand configuration");
		HeadlessRenderer ServerRenderer(Vector2(32, 32));
		Engine Runtime(ServerWorld, &ServerRenderer, {}, {.Content = std::move(ServerContent),
			.Mode = RuntimeMode::NetworkServer, .EnableResidencyDemandPolicy = true});
		Runtime.ProcessService->Alive = true;
		auto Network = SimulatedNetwork::Create({});
		GameSessionConfiguration ServerConfig;
		ServerConfig.Role = GameSessionRole::Server;
		ServerConfig.Endpoint = {"cross-foundation", 39003};
		ServerConfig.Limits = GameSessionConfiguration::DefaultLimits();
		ServerConfig.AllowInsecureDevelopmentNetwork = true;
		auto ClientConfig = ServerConfig;
		ClientConfig.Role = GameSessionRole::Client;
		GameSession Server(Network->CreateTransport(), ServerConfig, &Runtime);
		GameSession Client(Network->CreateTransport(), ClientConfig);
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		Require(Server.Start().Succeeded() && Client.Start().Succeeded(), "simulated GameSession failed to start");
		std::size_t NetworkObservations = 0;
		auto Advance = [&] {
			Require(Network->Advance(16ms), "simulated transport advance failed");
			Network->Pump();
			(void)Server.Poll();
			(void)Client.Poll();
			if (!ClientRuntime && Client.GetClientDataModel()) {
				// PlayerHost loads the signed package asset catalog into this replica
				// before attaching its client Engine. Mirror that native composition.
				auto Replica = Client.GetClientDataModel();
				auto ClientAssets = std::dynamic_pointer_cast<AssetService>(Replica->GetService("AssetService"));
				Require(ClientAssets != nullptr, "replicated client has no AssetService");
				ClientAssets->LoadRuntimeAssetSnapshot(Loaded.Assets);
				Require(ClientAssets->ResolveCollisionForMesh(OffsetMeshReference).has_value(),
					"replicated client did not load packaged collision dependency");
				ClientRenderer = std::make_unique<HeadlessRenderer>(Vector2(32, 32));
				ClientRuntime = std::make_unique<Engine>(Replica, ClientRenderer.get(), nullptr,
					EngineProviderConfiguration{.Mode = RuntimeMode::NetworkClient});
				ClientRuntime->ProcessService->Alive = true;
				Require(Client.AttachClientRuntime(*ClientRuntime), "client runtime attachment failed");
			}
			if (ClientRuntime) ClientRuntime->Step();
			Runtime.Step();
			Server.Step(Runtime.GetSimulationTick());
			Client.Step(Runtime.GetSimulationTick());
			++NetworkObservations;
		};
		auto Until = [&](auto Complete, std::string_view Failure) {
			const auto Deadline = Clock::now() + 15s;
			while (Clock::now() < Deadline) {
				Advance();
				if (Complete()) return;
				std::this_thread::yield();
			}
			throw std::runtime_error(std::string(Failure));
		};
		Until([&] { return Client.GetStatus() == GameSessionStatus::Ready && ClientRuntime &&
			Runtime.GetResidencyDemandPolicy() && !Runtime.Players->GetPlayers().empty(); },
			"packaged GameSession did not become ready");
		Require(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0,
			"ready client without authoritative Character created a content focus");
		Require(Runtime.Content->GetState(OffsetKey) != ContentResidencyState::Resident &&
			Runtime.Content->GetState(SolidKey) != ContentResidencyState::Resident &&
			!ClientRuntime->Workspace->FindFirstDescendant("OffsetA") &&
			!ClientRuntime->Workspace->FindFirstDescendant("OffsetB"),
			"packaged MeshParts were already resident or materialized before trusted geometry focus");
		auto PlayerValue = Runtime.Players->GetPlayers().front();
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		auto RootPart = std::make_shared<Part>();
		RootPart->SetAnchored(true);
		RootPart->SetCanCollide(false);
		RootPart->SetParent(CharacterValue);
		CharacterValue->SetRootPart(RootPart);
		CharacterValue->ApplyRuntimeTransform(CFrame(301, 0, 0));
		CharacterValue->SetParent(Runtime.Workspace);
		const auto GeometryFocusStarted = Clock::now();
		PlayerValue->SetCharacter(CharacterValue);
		Until([&] { return Runtime.Content->GetState(OffsetKey) == ContentResidencyState::Resident &&
			Runtime.Content->GetState(SolidKey) == ContentResidencyState::Resident; },
			"geometry-adjacent authoritative Character did not demand package content");
		const auto GeometryFocusToResidentUs = Microseconds(GeometryFocusStarted);
		Until([&] { return ClientRuntime->Workspace->FindFirstDescendant("OffsetA") &&
			ClientRuntime->Workspace->FindFirstDescendant("OffsetB"); },
			"geometry-adjacent Character did not receive both accepted offset MeshParts");
		const auto GeometryFocusToMaterializedUs = Microseconds(GeometryFocusStarted);
		auto GeometryA = std::dynamic_pointer_cast<MeshPart>(ClientRuntime->Workspace->FindFirstDescendant("OffsetA"));
		auto GeometryB = std::dynamic_pointer_cast<MeshPart>(ClientRuntime->Workspace->FindFirstDescendant("OffsetB"));
		Require(GeometryA && GeometryB && GeometryA->GetMesh() == OffsetMeshReference &&
			GeometryB->GetMesh() == OffsetMeshReference && Server.GetMetrics().SpatialMembershipCount >= 2,
			"geometry-aware relevance lost shared MeshPart asset identity or spatial membership");
		const auto GeometryAId = GeometryA->GetObjectId();
		const auto GeometryBId = GeometryB->GetObjectId();
		GeometryA.reset(); GeometryB.reset();
		const auto RootFocusStarted = Clock::now();
		const auto EvaluationsBeforeRootMove = Server.GetMetrics().RelevancePeerEvaluations;
		// x128 also lies within both existing spatial radii; accepted instances
		// must remain current as the trusted Character crosses toward their roots.
		CharacterValue->ApplyRuntimeTransform(CFrame(128, 0, 0));
		Until([&] { return Server.GetMetrics().RelevancePeerEvaluations > EvaluationsBeforeRootMove &&
			ClientRuntime->Workspace->FindFirstDescendant("OffsetA") &&
			ClientRuntime->Workspace->FindFirstDescendant("OffsetB"); },
			"root-near traversal lost accepted client MeshParts");
		const auto RootFocusToRetainedUs = Microseconds(RootFocusStarted);
		auto ClientA = std::dynamic_pointer_cast<MeshPart>(ClientRuntime->Workspace->FindFirstDescendant("OffsetA"));
		auto ClientB = std::dynamic_pointer_cast<MeshPart>(ClientRuntime->Workspace->FindFirstDescendant("OffsetB"));
		Require(ClientA && ClientB && ClientA->GetMesh() == OffsetMeshReference &&
			ClientB->GetMesh() == OffsetMeshReference && ClientA->GetObjectId() == GeometryAId &&
			ClientB->GetObjectId() == GeometryBId,
			"root-near traversal replaced or changed current shared MeshParts");
		const auto OldClientAId = ClientA->GetObjectId();
		const auto OldClientBId = ClientB->GetObjectId();
		ClientA.reset(); ClientB.reset();
		const auto TraverseStarted = Clock::now();
		CharacterValue->ApplyRuntimeTransform(CFrame(900, 0, 0));
		Until([&] { return ClientRuntime->Workspace->FindFirstDescendant("CutPart") != nullptr; },
			"traverse did not materialize generated solid MeshPart");
		const auto TraverseToMaterializedUs = Microseconds(TraverseStarted);
		const auto TeleportStarted = Clock::now();
		CharacterValue->ApplyRuntimeTransform(CFrame(5000, 0, 0));
		Until([&] { return Runtime.Content->GetState(OffsetKey) == ContentResidencyState::Unavailable &&
			Runtime.Content->GetState(SolidKey) == ContentResidencyState::Unavailable &&
			!ClientRuntime->Workspace->FindFirstDescendant("OffsetA") &&
			!ClientRuntime->Workspace->FindFirstDescendant("OffsetB") &&
			!ClientRuntime->Workspace->FindFirstDescendant("CutPart"); },
			"teleport did not retire server content and accepted client MeshParts");
		const auto TeleportToRemovalUs = Microseconds(TeleportStarted);
		const auto ReentryStarted = Clock::now();
		CharacterValue->ApplyRuntimeTransform(CFrame(128, 0, 0));
		Until([&] { return Runtime.Content->GetState(OffsetKey) == ContentResidencyState::Resident &&
			ClientRuntime->Workspace->FindFirstDescendant("OffsetA") &&
			ClientRuntime->Workspace->FindFirstDescendant("OffsetB"); },
			"reentry did not restore accepted current MeshParts");
		const auto ReentryToMaterializedUs = Microseconds(ReentryStarted);
		auto ReenteredA = ClientRuntime->Workspace->FindFirstDescendant("OffsetA");
		auto ReenteredB = ClientRuntime->Workspace->FindFirstDescendant("OffsetB");
		Require(ReenteredA && ReenteredB && ReenteredA->GetObjectId() != OldClientAId &&
			ReenteredB->GetObjectId() != OldClientBId,
			"reentry reused stale client object identities");
		std::cout << "[Content:CrossFoundationNetwork] transport=simulated provider=local-package"
			<< " geometryFocusResidentUs=" << GeometryFocusToResidentUs
			<< " geometryFocusMaterializedUs=" << GeometryFocusToMaterializedUs
			<< " rootFocusEvaluatedRetainedUs=" << RootFocusToRetainedUs
			<< " traverseMaterializedUs=" << TraverseToMaterializedUs
			<< " teleportRemovedUs=" << TeleportToRemovalUs
			<< " reentryMaterializedUs=" << ReentryToMaterializedUs
			<< " networkObservations=" << NetworkObservations
			<< " readyPeers=" << Server.GetPeerIdentities().size()
			<< " spatialMembership=" << Server.GetMetrics().SpatialMembershipCount
			<< " clientMaterializationMeasured=true physicalNetworkMeasured=false gpuMeasured=false" << '\n';
		Client.Stop();
		for (int Drain = 0; Drain < 40; ++Drain) Advance();
		Require(Runtime.GetResidencyDemandPolicy()->GetMetrics().Focuses == 0 &&
			Server.GetPeerIdentities().empty() && Runtime.Players->GetPlayers().empty(),
			"client disconnect retained focus, peer, or Player");
		if (ClientRuntime) ClientRuntime->Destroy();
		Server.Stop();
		Runtime.Destroy();
	}

	void Run() {
		FixtureDirectory Fixture;
		BootstrapProjectRuntimeSchema(Fixture.ProjectRoot);
		WriteText(Fixture.ProjectRoot / "assets" / "offset.obj",
			"v 300 -1 0\nv 302 -1 0\nv 301 1 0\nvt 0 0\nvt 1 0\nvt 0.5 1\nf 1/1 2/2 3/3\n");
		WriteText(Fixture.ProjectRoot / "assets" / "cut.solid",
			R"({"Version":1,"Root":"cut","Nodes":[{"Id":"body","Kind":"Box","Size":[4,4,4]},{"Id":"cutter","Kind":"Box","Size":[2,6,2]},{"Id":"cut","Kind":"Subtraction","Inputs":["body","cutter"]}]})");
		DiskFilesystem Filesystem(Fixture.ProjectRoot);
		SourceMount Mount(Filesystem);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto Authoring = std::make_shared<DataModel>();
		Authoring->SetName("CrossFoundationStreaming");
		Authoring->Root = Fixture.ProjectRoot;
		Authoring->Filesystem = &Filesystem;
		Authoring->InitializeLoadedProjectRevision();
		auto AuthoringSpace = std::dynamic_pointer_cast<Workspace>(Authoring->GetService("Workspace"));
		auto AuthoringAssets = std::dynamic_pointer_cast<AssetService>(Authoring->GetService("AssetService"));
		Require(AuthoringSpace && AuthoringAssets, "authoring services missing");

		const auto ImportStarted = Clock::now();
		const auto Imported = AuthoringAssets->ImportProjectAsset(Mount, "assets/offset.obj", AssetKind::Mesh, "Offset");
		const auto ObjImportUs = Microseconds(ImportStarted);
		const auto CompileStarted = Clock::now();
		const auto Compiled = AuthoringAssets->ImportProjectAsset(Mount, "assets/cut.solid", AssetKind::Mesh, "Cut");
		const auto SolidImportUs = Microseconds(CompileStarted);
		Require(Imported.Ok && Imported.Record && Imported.Records.size() == 2 &&
			Compiled.Ok && Compiled.Record && Compiled.Records.size() == 2,
			"real OBJ and Boolean source did not publish paired Mesh/Collision assets");
		const auto ObjCollision = AuthoringAssets->ResolveCollisionForMesh(Imported.Record->Reference.Value);
		const auto SolidCollision = AuthoringAssets->ResolveCollisionForMesh(Compiled.Record->Reference.Value);
		Require(ObjCollision && SolidCollision && ObjCollision->Id != SolidCollision->Id,
			"distinct imported and generated collision dependencies missing");

		const auto OffsetUnit = AddUnit(AuthoringSpace, "OffsetUnit");
		AddMeshPart(OffsetUnit, "OffsetA", Imported.Record->Reference.Value, {0, 0, 0});
		AddMeshPart(OffsetUnit, "OffsetB", Imported.Record->Reference.Value, {20, 0, 0});
		const auto SolidUnit = AddUnit(AuthoringSpace, "SolidUnit");
		AddMeshPart(SolidUnit, "CutPart", Compiled.Record->Reference.Value, {900, 0, 0});
		Authoring->MarkPersistenceSubtreeArchivable();
		const auto Revision = Authoring->GetAuthoritativeRevision();
		const auto SaveStarted = Clock::now();
		ProjectValue.PersistGameAtomically(ProjectValue.CaptureGame(Authoring, Revision));
		const auto ProjectSaveUs = Microseconds(SaveStarted);
		DiskFilesystem ReopenFilesystem(Fixture.ProjectRoot);
		auto ReopenedProject = Project::fromExisting(&ReopenFilesystem);
		const auto ReopenStarted = Clock::now();
		auto Reopened = ReopenedProject.DeserializeGame();
		const auto ProjectReopenUs = Microseconds(ReopenStarted);
		auto ReopenedAssets = std::dynamic_pointer_cast<AssetService>(Reopened->GetService("AssetService"));
		auto ReopenedSpace = std::dynamic_pointer_cast<Workspace>(Reopened->GetService("Workspace"));
		auto SavedA = ReopenedSpace ? std::dynamic_pointer_cast<MeshPart>(
			ReopenedSpace->FindFirstDescendant("OffsetA")) : nullptr;
		auto SavedB = ReopenedSpace ? std::dynamic_pointer_cast<MeshPart>(
			ReopenedSpace->FindFirstDescendant("OffsetB")) : nullptr;
		auto SavedCut = ReopenedSpace ? std::dynamic_pointer_cast<MeshPart>(
			ReopenedSpace->FindFirstDescendant("CutPart")) : nullptr;
		Require(ReopenedAssets && SavedA && SavedB && SavedCut &&
			SavedA->GetMesh() == Imported.Record->Reference.Value &&
			SavedB->GetMesh() == Imported.Record->Reference.Value &&
			SavedCut->GetMesh() == Compiled.Record->Reference.Value &&
			ReopenedAssets->ResolveCollisionForMesh(Imported.Record->Reference.Value).has_value() &&
			ReopenedAssets->ResolveCollisionForMesh(Compiled.Record->Reference.Value).has_value(),
			"saved project did not reopen shared MeshParts and canonical collision artifacts");
		const auto CaptureStarted = Clock::now();
		auto Payload = PackageBuilder::Capture(ReopenedProject, Reopened, Revision, Revision);
		const auto CaptureUs = Microseconds(CaptureStarted);
		const auto OffsetKey = FindUnitKey(Payload, "OffsetUnit");
		const auto SolidKey = FindUnitKey(Payload, "SolidUnit");
		Require(Payload.ContentUnits.size() == 2 && !OffsetKey.empty() && !SolidKey.empty() && OffsetKey != SolidKey,
			"capture did not produce two independent streamed content units");
		for (auto &Unit : Payload.ContentUnits)
			if (Unit.Entry.Key == OffsetKey) Unit.Entry.Dependencies = {SolidKey};
		const auto OffsetCaptured = std::ranges::find_if(Payload.ContentUnits,
			[&](const GamePayload::ContentUnit &Unit) { return Unit.Entry.Key == OffsetKey; });
		Require(OffsetCaptured != Payload.ContentUnits.end() && OffsetCaptured->Entry.CoarseBounds &&
			OffsetCaptured->Entry.CoarseBounds->Minimum[0] <= 300.0f &&
			OffsetCaptured->Entry.CoarseBounds->Maximum[0] >= 322.0f,
			"capture underbounded the offset imported mesh geometry");
		const auto AssetBefore = AuthoringAssets->ReadCollisionStats();

		// The package builder rejects an absent hard dependency before publishing a candidate.
		auto Invalid = Payload;
		for (auto &Unit : Invalid.ContentUnits)
			if (Unit.Entry.Key == OffsetKey) Unit.Entry.Dependencies = {"missing"};
		const auto Denied = PackageBuilder::Build({
			.Payload = std::move(Invalid),
			.RuntimeDistributionRoot = std::filesystem::path(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
			.OutputDirectory = Fixture.Root / "Rejected",
			.Configuration = PackageConfiguration::Release,
		});
		Require(!Denied.Ok && HasPackageError(Denied.Diagnostics), "invalid package dependency was accepted");

		const auto BuildStarted = Clock::now();
		const auto Built = PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = std::filesystem::path(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
			.OutputDirectory = Fixture.PackageRoot,
			.Configuration = PackageConfiguration::Release,
		});
		const auto PackageBuildUs = Microseconds(BuildStarted);
		Require(Built.Ok && !HasPackageError(Built.Diagnostics), "real Release package build failed");
		const auto ValidateStarted = Clock::now();
		Require(!HasPackageError(PackageBuilder::Validate(Fixture.PackageRoot)), "Release package validation failed");
		const auto PackageValidateUs = Microseconds(ValidateStarted);
		CheckHistoricalUnderbound(Fixture.PackageRoot, OffsetKey);
		std::vector<PackageDiagnostic> LoadDiagnostics;
		const auto LoadStarted = Clock::now();
		auto Loaded = PackageBuilder::Load(Fixture.PackageRoot, LoadDiagnostics);
		const auto PackageLoadUs = Microseconds(LoadStarted);
		Require(Loaded && !HasPackageError(LoadDiagnostics), "validated package could not load");
		SavedA.reset(); SavedB.reset(); SavedCut.reset();
		ReopenedSpace.reset(); ReopenedAssets.reset();
		Reopened->Destroy(); Reopened.reset();
		AuthoringAssets.reset();
		Authoring->Destroy();
		Authoring.reset();
		std::filesystem::remove_all(Fixture.ProjectRoot); // Packaged runtime has no authored source tree.
		BootstrapPackagedRuntimeSchema(Loaded->PreRunSource);
		const auto WorldStarted = Clock::now();
		auto World = PackageBuilder::LoadWorld(*Loaded, Fixture.PackageRoot);
		const auto WorldLoadUs = Microseconds(WorldStarted);
		auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		Require(Space && Assets && Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value) &&
			Assets->ResolveCollisionForMesh(Compiled.Record->Reference.Value),
			"packaged runtime did not load exact Mesh/Collision asset closure without source");
		const auto RuntimeAssetsBefore = Assets->ReadCollisionStats();
		Require(RuntimeAssetsBefore.ResidentRecords >= 2 && RuntimeAssetsBefore.RetainedArtifactBytes > 0 &&
			RuntimeAssetsBefore.CanonicalBytes >= AssetBefore.CanonicalBytes,
			"packaged asset collision resources were not separately accounted");
		auto Configuration = PackageBuilder::GetLocalContentConfiguration(*Loaded, Fixture.PackageRoot,
			ContentResidencyMode::OnDemand);
		Require(Configuration.has_value(), "package did not expose real local on-demand provider");
		ContentAvailabilityService Service(World, Space, std::move(*Configuration));
		std::uint64_t Tick = 0;
		Pump(Service, nullptr, Tick, [&] { return Service.IsManifestAvailable(); }, "local manifest readiness timed out");
		const auto Manifest = Service.GetManifestSnapshot();
		Require(Manifest && Manifest->Entries.size() == 2 && FindEntry(*Manifest, OffsetKey).Dependencies ==
			std::vector<std::string>{SolidKey}, "packaged dependency metadata changed");
		Require(!Space->FindFirstChild("OffsetUnit", false) && !Space->FindFirstChild("SolidUnit", false),
			"on-demand units materialized before policy demand");
		auto WrongRevision = std::make_shared<PackageContentManifest>(*Manifest);
		++WrongRevision->Package.PackageVersion;
		ResidencyDemandPolicy WrongPolicy(*WrongRevision);
		Require(WrongPolicy.UpdateFocus(99, {301, 0, 0}), "wrong-revision test focus rejected");
		WrongPolicy.Step(&Service, 1);
		Require(WrongPolicy.GetMetrics().ServiceBindingFailures == 1 && Service.GetMetrics().Requests == 0,
			"policy accepted a stale package revision");

		ResidencyDemandPolicyLimits Limits;
		Limits.GraceTicks = 3;
		const auto PolicyBuildStarted = Clock::now();
		auto Policy = std::make_unique<ResidencyDemandPolicy>(Manifest, Limits);
		const auto PolicyBuildUs = Microseconds(PolicyBuildStarted);
		Require(Policy->UpdateFocus(1, {301, 0, 0}), "offset geometry focus rejected");
		const auto FirstPolicyStepStarted = Clock::now();
		Policy->Step(&Service, ++Tick);
		const auto FirstPolicyStepUs = Microseconds(FirstPolicyStepStarted);
		Require(Policy->GetMetrics().ManifestBindingEntriesCompared == 0,
			"shared snapshot first binding performed a deep manifest comparison");
		const auto AdmissionStarted = Clock::now();
		Pump(Service, Policy.get(), Tick, [&] {
			return Service.GetState(OffsetKey) == ContentResidencyState::Resident &&
				Service.GetState(SolidKey) == ContentResidencyState::Resident;
		}, "offset/dependency content admission timed out");
		const auto AdmissionUs = Microseconds(AdmissionStarted);
		Require(Policy->GetMetrics().Requests == 1 && Service.GetMetrics().Requests == 1,
			"one focus did not produce one direct demand with transitive dependency");
		auto LiveOffset = std::dynamic_pointer_cast<Folder>(Space->FindFirstChild("OffsetUnit", false));
		auto LiveSolid = std::dynamic_pointer_cast<Folder>(Space->FindFirstChild("SolidUnit", false));
		Require(LiveOffset && LiveSolid, "3L resident units were not authoritative Workspace children");
		auto OffsetA = std::dynamic_pointer_cast<MeshPart>(LiveOffset->FindFirstChild("OffsetA", false));
		auto OffsetB = std::dynamic_pointer_cast<MeshPart>(LiveOffset->FindFirstChild("OffsetB", false));
		auto CutPart = std::dynamic_pointer_cast<MeshPart>(LiveSolid->FindFirstChild("CutPart", false));
		Require(OffsetA && OffsetB && CutPart, "packaged MeshParts did not deserialize");
		const auto OldOffsetId = LiveOffset->GetObjectId();
		const auto OldAId = OffsetA->GetObjectId();
		const auto OldBId = OffsetB->GetObjectId();
		const auto OldCutId = CutPart->GetObjectId();
		const auto OldSolidId = LiveSolid->GetObjectId();
		const auto ProjectionStarted = Clock::now();
		Space->StepPhysics(0.0f, std::nullopt);
		const auto PostAdmissionPhysicsStepUs = Microseconds(ProjectionStarted);
		const auto Native = WorldRootTestAccess::Collision(*Space);
		Require(Native.ActiveResources == 2 && Native.ActiveConsumers == 3 &&
			Native.ActiveMeshShapes >= 3 && Native.NativeMeshBytes > 0 && Native.FailedAdmissions == 0,
			"resident MeshParts did not project shared and generated native collision");
		const auto OldBodyA = WorldRootTestAccess::Body(*Space, *OffsetA);
		const auto OldBodyB = WorldRootTestAccess::Body(*Space, *OffsetB);
		Require(OldBodyA.IsValid() && OldBodyB.IsValid() && OldBodyA != OldBodyB &&
			WorldRootTestAccess::ValidBody(*Space, OldBodyA), "shared mesh consumers lack distinct body identities");
		const auto Hit = WorldRootTestAccess::Raycast(*Space, {301, 0, 2}, {0, 0, -4});
		Require(Hit.Succeeded() && Hit.Instance == OffsetA, "offset imported triangle is not queryable at its actual geometry");
		const auto SolidHit = WorldRootTestAccess::Raycast(*Space, {901.5f, 5, 0}, {0, -6, 0});
		Require(SolidHit.Succeeded() && SolidHit.Instance == CutPart,
			"generated Boolean collision did not expose its retained top slab to queries");
		auto Probe = std::make_shared<Part>();
		Probe->SetName("CollisionProbe");
		Probe->SetShape(Enums::PartType::Ball);
		Probe->SetSize({1.0f, 1.0f, 1.0f});
		Probe->SetAnchored(false);
		Probe->SetCanCollide(true);
		Probe->SetCanTouch(true);
		Probe->SetCFrame(CFrame(901.5f, 5.0f, 0.0f));
		int Touches = 0;
		auto TouchConnection = CutPart->Touched->Connect([&](std::shared_ptr<BasePart> Other) {
			if (Other == Probe) ++Touches;
		});
		Probe->SetParent(Space); // It is not a package child and cannot pin unit eviction.
		const auto ContactStarted = Clock::now();
		for (int ContactTick = 0; ContactTick < 180; ++ContactTick)
			Space->StepPhysics(1.0f / 60.0f, std::nullopt);
		const auto ContactLoopUs = Microseconds(ContactStarted);
		Require(Touches > 0 && Probe->GetCFrame().Position.y >= 2.1f &&
			Probe->GetCFrame().Position.y <= 3.0f,
			"dynamic primitive did not contact and settle on generated Boolean collision");
		TouchConnection->Disconnect();
		Probe->Destroy();
		Probe.reset();

		RenderPublisher Publisher;
		RenderProjection Projection;
		auto MeshChanges = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
		const auto RenderStarted = Clock::now();
		auto Published = Publisher.Publish(*Space, RenderCameraInput{}, 320, 180);
		const auto RenderPublishUs = Microseconds(RenderStarted);
		Require(Published && CountCreates(*Published, OldAId, OldBId) == 2 &&
			CountCreates(*Published, OldCutId) == 1 &&
			Projection.Apply(*Published).Created >= 3,
			"resident MeshParts did not publish separate render objects");

		// Teleport away: grace must keep the old direct demand briefly, then 3L
		// destroys only its original package-owned hierarchy.
		Require(Policy->UpdateFocus(1, {5000, 0, 0}), "teleport focus rejected");
		for (int Delay = 0; Delay < 2; ++Delay) { Policy->Step(&Service, ++Tick); Service.Step(); }
		Require(Service.GetState(OffsetKey) == ContentResidencyState::Resident,
			"grace released offset content before its finite deadline");
		const auto RetirementStarted = Clock::now();
		Pump(Service, Policy.get(), Tick, [&] {
			return Service.GetState(OffsetKey) == ContentResidencyState::Unavailable &&
				Service.GetState(SolidKey) == ContentResidencyState::Unavailable;
		}, "teleport release/eviction timed out");
		const auto RetirementUs = Microseconds(RetirementStarted);
		const auto PhysicsRetirementStarted = Clock::now();
		Space->StepPhysics(0.0f, std::nullopt);
		const auto PostEvictionPhysicsStepUs = Microseconds(PhysicsRetirementStarted);
		const auto Retired = WorldRootTestAccess::Collision(*Space);
		Require(Retired.ActiveResources == 0 && Retired.ActiveConsumers == 0 &&
			Retired.ActiveMeshShapes == 0 && Retired.NativeMeshBytes == 0 &&
			Retired.CreatedResources == Retired.RetiredResources &&
			Retired.CreatedMeshShapes == Retired.RetiredMeshShapes &&
			!WorldRootTestAccess::ValidBody(*Space, OldBodyA),
			"last native mesh consumer or shape survived package eviction");
		Require(Assets->ReadCollisionStats().ResidentRecords == RuntimeAssetsBefore.ResidentRecords &&
			Assets->ReadCollisionStats().RetainedArtifactBytes == RuntimeAssetsBefore.RetainedArtifactBytes,
			"3L eviction unexpectedly destroyed separately owned asset artifacts");
		auto Unpublished = Publisher.Publish(*Space, RenderCameraInput{}, 320, 180);
		Require(Unpublished && CountRemoves(*Unpublished, OldAId, OldBId) == 2 &&
			CountRemoves(*Unpublished, OldCutId) == 1 &&
			Projection.Apply(*Unpublished).Removed >= 3,
			"evicted MeshParts did not unpublish renderer objects");
		LiveOffset.reset(); LiveSolid.reset(); OffsetA.reset(); OffsetB.reset(); CutPart.reset();
		Require(!Space->FindFirstChild("OffsetUnit", false) && !Space->FindFirstChild("SolidUnit", false),
			"evicted unit roots remain in Workspace");

		Require(Policy->UpdateFocus(1, {301, 0, 0}), "reacquire focus rejected");
		const auto ReacquireStarted = Clock::now();
		Pump(Service, Policy.get(), Tick, [&] {
			return Service.GetState(OffsetKey) == ContentResidencyState::Resident &&
				Service.GetState(SolidKey) == ContentResidencyState::Resident;
		}, "reacquisition timed out");
		const auto ReacquireUs = Microseconds(ReacquireStarted);
		const auto PhysicsReprojectionStarted = Clock::now();
		Space->StepPhysics(0.0f, std::nullopt);
		const auto PostReacquirePhysicsStepUs = Microseconds(PhysicsReprojectionStarted);
		auto NewOffset = std::dynamic_pointer_cast<Folder>(Space->FindFirstChild("OffsetUnit", false));
		auto NewSolid = std::dynamic_pointer_cast<Folder>(Space->FindFirstChild("SolidUnit", false));
		Require(NewOffset && NewSolid && NewOffset->GetObjectId() != OldOffsetId &&
			NewSolid->GetObjectId() != OldSolidId, "reacquisition reused stale package root identities");
		auto NewA = std::dynamic_pointer_cast<MeshPart>(NewOffset->FindFirstChild("OffsetA", false));
		Require(NewA && NewA->GetObjectId() != OldAId &&
			WorldRootTestAccess::Body(*Space, *NewA).IsValid() &&
			WorldRootTestAccess::Collision(*Space).ActiveResources == 2,
			"reacquisition did not rebuild native collision with fresh object identity");
		Require(Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value)->Id == ObjCollision->Id &&
			Assets->ResolveCollisionForMesh(Compiled.Record->Reference.Value)->Id == SolidCollision->Id,
			"reacquisition changed stable asset identities");
		Policy->Stop(&Service);
		Service.Stop();
		Require(Service.GetMetrics().RetainedRecordCount == 0 &&
			Service.GetMetrics().ResidentPackageObjects == 0,
			"service stop retained package ownership metadata");
		NewA.reset(); NewOffset.reset(); NewSolid.reset();
		Assets.reset();
		World->Destroy();
		World.reset();
		RunGameSessionPhase(*Loaded, Fixture.PackageRoot, OffsetKey, SolidKey,
			Imported.Record->Reference.Value);

		std::cout << "[Content:CrossFoundation] provider=local-package sourceFilesRemoved=true"
			<< " importedMeshCollisionPair=true generatedSolidCollisionPair=true"
			<< " contentUnits=2 dependencyEdges=1 sharedConsumers=2"
			<< " objImportUs=" << ObjImportUs << " solidCompileImportUs=" << SolidImportUs
			<< " projectSaveUs=" << ProjectSaveUs << " projectReopenUs=" << ProjectReopenUs
			<< " captureUs=" << CaptureUs
			<< " spatialInspectionUs=" << Payload.SpatialInspectionDuration.count()
			<< " packageBuildUs=" << PackageBuildUs
			<< " packageValidateUs=" << PackageValidateUs << " packageLoadUs=" << PackageLoadUs
			<< " worldLoadUs=" << WorldLoadUs << " policyBuildUs=" << PolicyBuildUs
			<< " firstPolicyStepIncludingBindUs=" << FirstPolicyStepUs << " admissionWaitUs=" << AdmissionUs
			<< " postAdmissionPhysicsStepUs=" << PostAdmissionPhysicsStepUs << " renderPublishUs=" << RenderPublishUs
			<< " contactLoopUs=" << ContactLoopUs << " contactTicks=180"
			<< " retirementWaitUs=" << RetirementUs << " reacquireWaitUs=" << ReacquireUs
			<< " postEvictionPhysicsStepUs=" << PostEvictionPhysicsStepUs
			<< " postReacquirePhysicsStepUs=" << PostReacquirePhysicsStepUs
			<< " admissionWaitIncludesNativeCreation=true retirementWaitIncludesNativeRetirement=true"
			<< " initialNativeResources=" << Native.ActiveResources
			<< " initialNativeConsumers=" << Native.ActiveConsumers
			<< " initialNativeShapes=" << Native.ActiveMeshShapes
			<< " initialNativeBytes=" << Native.NativeMeshBytes
			<< " retainedAssetArtifactBytes=" << RuntimeAssetsBefore.RetainedArtifactBytes
			<< " finalNativeResources=" << Retired.ActiveResources
			<< " clientMaterializationMeasured=true relevanceMeasured=true"
			<< " physicalNetworkMeasured=false gpuMeasured=false" << '\n';
	}
}

int main() {
	if (!SDL_Init(0)) {
		std::cerr << "[Content:CrossFoundation] SDL initialization failed\n";
		return 1;
	}
	try {
		Run();
		SDL_Quit();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:CrossFoundation] " << Error.what() << '\n';
		SDL_Quit();
		return 1;
	}
}

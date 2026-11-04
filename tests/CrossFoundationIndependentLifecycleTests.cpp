#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/network/GameSession.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace gargantuan {
	struct WorldRootTestAccess {
		static PhysicsCollisionResidency CollisionResidency(const WorldRoot &World) {
			return World.Physics.GetCollisionResidency();
		}
		static WorldRaycastResult Raycast(WorldRoot &World, glm::vec3 Origin, glm::vec3 Direction) {
			return World.ResolveRaycast(Origin, Direction);
		}
	};
}

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Value) {
		std::filesystem::create_directories(Path.parent_path());
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Value.data(), static_cast<std::streamsize>(Value.size()));
		Check(Output.good(), "fixture file write failed");
	}

	struct PackageFixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-cross-foundation-lifecycle-" +
				std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		PackageContentManifest Manifest;
		AssetContentId ManifestDigest;
		AssetRuntimeSnapshot Assets;
		std::string MeshReference;

		explicit PackageFixture(std::uint64_t PackageVersion, std::string_view MeshName = "CrossMeshA") {
			// The asset importer publishes one canonical render Mesh and its separate
			// Collision companion. The mesh is offset from its semantic root so the
			// package-space geometry and 3E root relevance remain distinct.
			WriteText(Root / "assets/offset.obj",
				"v 299 -1 0\nv 301 -1 0\nv 300 1 0\nf 1 2 3\n");
			DiskFilesystem Filesystem(Root);
			SourceMount Mount(Filesystem);
			auto AuthoringWorld = std::make_shared<DataModel>();
			AuthoringWorld->Root = Root;
			AuthoringWorld->Filesystem = &Filesystem;
			auto AuthoringAssets = std::dynamic_pointer_cast<AssetService>(AuthoringWorld->GetService("AssetService"));
			Check(static_cast<bool>(AuthoringAssets), "authoring AssetService unavailable");
			const auto Imported = AuthoringAssets->ImportProjectAsset(Mount, "assets/offset.obj", AssetKind::Mesh);
			Check(Imported.Ok && Imported.Record && Imported.Records.size() == 2,
				"compound Mesh and Collision import failed");
			MeshReference = Imported.Record->Reference.Value;
			const auto Mesh = AuthoringAssets->ResolveMesh(MeshReference);
			const auto Collision = AuthoringAssets->ResolveCollisionForMesh(MeshReference);
			Check(Mesh && Collision && Mesh->Bounds.Minimum.x == 299.0f &&
				Collision->Value.Bounds.Minimum.x == 299.0f,
				"canonical offset mesh and collision bounds were not retained");
			Assets = AuthoringAssets->CaptureRuntimeAssets();
			AuthoringWorld->Destroy();

			Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), PackageVersion};
			AddMesh("a", MeshName, CFrame(0, 0, 0), 299.0f, 301.0f);
			AddMesh("b", "CrossMeshB", CFrame(10, 0, 0), 309.0f, 311.0f);
			auto Sensor = std::make_shared<Part>();
			Sensor->SetName("CrossSensor");
			Sensor->SetArchivable(true);
			Sensor->SetAnchored(true);
			Sensor->SetCanCollide(false);
			Sensor->SetCanTouch(true);
			Sensor->SetSize({4, 4, 4});
			Sensor->SetCFrame(CFrame(100, 0, 0));
			AddUnit("sensor", Sensor, PackageCoarseBounds{{98, -2, -2}, {102, 2, 2}});
			Sensor->Destroy();
			const auto ManifestText = EncodePackageContentManifest(Manifest);
			ManifestDigest = AssetContentId::Hash(std::span(
				reinterpret_cast<const std::uint8_t *>(ManifestText.data()), ManifestText.size()));
			WriteText(Root / "content/manifest.json", ManifestText);
		}

		~PackageFixture() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		void AddUnit(std::string Key, const std::shared_ptr<Instance> &Object, PackageCoarseBounds Bounds) {
			auto Serializable = Object;
			const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Serializable);
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
			PackageContentEntry Entry;
			Entry.Key = Key;
			Entry.BlobReference = "content/" + Key + ".json";
			Entry.Digest = AssetContentId::Hash(Bytes);
			Entry.CompressedBytes = Entry.UncompressedBytes = Bytes.size();
			Entry.ObjectCount = 1;
			Entry.CoarseBounds = Bounds;
			WriteText(Root / Entry.BlobReference, Encoded);
			Manifest.Entries.push_back(std::move(Entry));
		}

		void AddMesh(std::string Key, std::string_view Name, CFrame Frame, float MinimumX, float MaximumX) {
			auto Object = std::make_shared<MeshPart>();
			Object->SetName(std::string(Name));
			Object->SetArchivable(true);
			Object->SetAnchored(true);
			Object->SetCanTouch(true);
			Object->SetSize({1, 1, 1});
			Object->SetCFrame(Frame);
			Object->SetMesh(MeshReference);
			Object->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
			AddUnit(std::move(Key), Object,
				PackageCoarseBounds{{MinimumX, -1, 0}, {MaximumX, 1, 0}});
			Object->Destroy();
		}

		ContentAvailabilityConfiguration Configuration() const {
			ContentAvailabilityConfiguration Result;
			Result.Provider = std::make_shared<LocalPackageContentProvider>(Root, "content/manifest.json", ManifestDigest);
			Result.Package = Manifest.Package;
			Result.ManifestDigest = ManifestDigest;
			Result.Mode = ContentResidencyMode::OnDemand;
			return Result;
		}
	};

	template <class Advance, class Predicate> void Until(Advance Step, Predicate Complete, const char *Message) {
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < Deadline) {
			Step();
			if (Complete()) return;
			std::this_thread::yield();
		}
		throw std::runtime_error(Message);
	}

	struct RuntimeFixture final {
		PackageFixture Package;
		std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer{Vector2(32, 32)};
		std::unique_ptr<Engine> Runtime;

		explicit RuntimeFixture(std::uint64_t PackageVersion, std::string_view MeshName = "CrossMeshA")
			: Package(PackageVersion, MeshName) {
			auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
			Check(static_cast<bool>(Assets), "runtime AssetService unavailable");
			Assets->LoadRuntimeAssetSnapshot(Package.Assets);
			Check(Assets->ResolveMeshResource(Package.MeshReference).has_value() &&
				Assets->ResolveCollisionForMesh(Package.MeshReference).has_value(),
				"runtime asset bytes did not resolve independently of content residency");
			Runtime = std::make_unique<Engine>(World, &Renderer, std::function<void(std::string, std::string)>{},
				EngineProviderConfiguration{.Content = Package.Configuration(), .Mode = RuntimeMode::NetworkServer});
			Runtime->ProcessService->Alive = true;
		}

		~RuntimeFixture() {
			if (Runtime) Runtime->Destroy();
			if (World) World->Destroy();
		}
	};

	void IndependentLifecycle() {
		RuntimeFixture Fixture(51);
		auto &Runtime = *Fixture.Runtime;
		auto &Renderer = Fixture.Renderer;
		auto Space = Runtime.Workspace;
		Check(!Space->FindFirstChild("CrossMeshA", false) &&
			WorldRootTestAccess::CollisionResidency(*Space).ActiveResources == 0,
			"asset bytes alone created a semantic or physics resident object");

		auto Visitor = std::make_shared<Part>();
		Visitor->SetName("CrossVisitor");
		Visitor->SetCFrame(CFrame(100, 0, 0));
		Visitor->SetParent(Space);
		Space->SetGravity(0.0f);
		std::size_t Touches = 0;
		Visitor->Touched->Connect([&](const std::shared_ptr<BasePart> &Other) {
			if (Other && Other->GetName() == "CrossSensor") ++Touches;
		});

		auto Network = SimulatedNetwork::Create({});
		GameSessionConfiguration ServerConfiguration;
		ServerConfiguration.Role = GameSessionRole::Server;
		ServerConfiguration.Endpoint = {"cross-foundation", 39051};
		ServerConfiguration.Limits = GameSessionConfiguration::DefaultLimits();
		ServerConfiguration.AllowInsecureDevelopmentNetwork = true;
		auto ClientConfiguration = ServerConfiguration;
		ClientConfiguration.Role = GameSessionRole::Client;
		GameSession Server(Network->CreateTransport(), ServerConfiguration, &Runtime);
		GameSession Client(Network->CreateTransport(), ClientConfiguration);
		Check(Server.Start().Succeeded() && Client.Start().Succeeded(), "local peer sessions failed to start");
		std::unique_ptr<HeadlessRenderer> ClientRenderer;
		std::unique_ptr<Engine> ClientRuntime;
		auto Step = [&] {
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
		Until(Step, [&] { return Client.GetStatus() == GameSessionStatus::Ready &&
			Runtime.Content->IsManifestAvailable(); }, "peer or local package manifest did not become ready");
		Check(!Client.GetClientDataModel()->FindFirstDescendant("CrossMeshA") &&
			Runtime.Content->GetMetrics().Admissions == 0,
			"peer received unadmitted package content");

		// 3E uses the semantic root for its final focus predicate. Keep the peer
		// at that root while the immutable mesh geometry remains near x=300.
		const auto PlayerValue = Runtime.Players->GetPlayers().front();
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		auto RootPart = std::make_shared<Part>();
		RootPart->SetAnchored(true);
		RootPart->SetCanCollide(false);
		RootPart->SetParent(CharacterValue);
		CharacterValue->SetRootPart(RootPart);
		CharacterValue->ApplyRuntimeTransform(CFrame(0, 0, 0));
		CharacterValue->SetParent(Space);
		PlayerValue->SetCharacter(CharacterValue);
		Check(Runtime.Content->RequestContent("a"), "first native demand was rejected");
		Until(Step, [&] { return Runtime.Content->GetState("a") == ContentResidencyState::Resident; },
			"verified package bytes did not cross semantic admission");
		const auto First = std::dynamic_pointer_cast<MeshPart>(Space->FindFirstChild("CrossMeshA", false));
		Check(First && Runtime.Content->GetMetrics().BytesVerified > 0 &&
			Runtime.Content->GetMetrics().Admissions == 1,
			"provider verification and authoritative admission were conflated");
		const auto FirstId = First->GetObjectId();
		const auto FirstPhysics = WorldRootTestAccess::CollisionResidency(*Space);
		const auto FirstHit = WorldRootTestAccess::Raycast(*Space, {300, 0, 2}, {0, 0, -4});
		Check(FirstPhysics.ActiveResources == 1 && FirstPhysics.ActiveConsumers == 1 &&
			FirstPhysics.ActiveMeshShapes > 0 && FirstHit.Succeeded() && FirstHit.HasHit() &&
			FirstHit.Instance == First,
			"admitted offset geometry did not create an owned shape and real query hit");
		Check(Renderer.GetProjection().GetItem(FirstId),
			"render publication did not follow authoritative MeshPart admission");
		Until(Step, [&] { return Client.GetClientDataModel()->FindFirstDescendant("CrossMeshA") != nullptr; },
			"accepted peer did not materialize the admitted semantic root");

		Check(Runtime.Content->RequestContent("b") && Runtime.Content->RequestContent("sensor"),
			"independent mesh and sensor demand was rejected");
		Until(Step, [&] { return Runtime.Content->GetState("b") == ContentResidencyState::Resident &&
			Runtime.Content->GetState("sensor") == ContentResidencyState::Resident; },
			"shared resource and sensor units did not admit");
		Space->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches >= 1, "admitted sensor did not produce a physics contact event");
		const auto SharedPhysics = WorldRootTestAccess::CollisionResidency(*Space);
		Check(SharedPhysics.ActiveResources == 1 && SharedPhysics.ActiveConsumers == 2 &&
			Runtime.Content->GetMetrics().Admissions == 3,
			"two semantic mesh units did not share one collision resource");

		Check(Runtime.Content->ReleaseContent("a"), "first demand release was rejected");
		Until(Step, [&] { return Runtime.Content->GetState("a") == ContentResidencyState::Unavailable; },
			"first content unit did not evict");
		Check(!ObjectRegistry::Get().Lookup(FirstId) &&
			WorldRootTestAccess::CollisionResidency(*Space).ActiveConsumers == 1 &&
			!Renderer.GetProjection().GetItem(FirstId),
			"eviction retained old semantic identity, render item, or physics consumer");
		Until(Step, [&] { return !Client.GetClientDataModel()->FindFirstDescendant("CrossMeshA"); },
			"peer did not unpublish the evicted root");
		Check(Runtime.Content->RequestContent("a"), "reacquisition demand was rejected");
		Until(Step, [&] { return Runtime.Content->GetState("a") == ContentResidencyState::Resident; },
			"reacquisition did not return to Resident");
		const auto Reacquired = Space->FindFirstChild("CrossMeshA", false);
		Check(Reacquired && Reacquired->GetObjectId() != FirstId && !ObjectRegistry::Get().Lookup(FirstId),
			"reacquisition reused a stale authoritative identity");
		const auto ReacquiredId = Reacquired->GetObjectId();
		Check(Runtime.Content->ReleaseContent("a") && Runtime.Content->ReleaseContent("b") &&
			Runtime.Content->ReleaseContent("sensor"), "final native releases failed");
		Until(Step, [&] { return Runtime.Content->GetMetrics().ResidentUnits == 0; },
			"final content demand did not drain");
		Check(WorldRootTestAccess::CollisionResidency(*Space).ActiveResources == 0 &&
			Runtime.Content->GetMetrics().DecodedDocumentBytes == 0,
			"release retained native collision resources or decoded content");
		Client.Stop();
		Server.Stop();
		if (ClientRuntime) ClientRuntime->Destroy();
		Runtime.Destroy();
		Check(Runtime.Content->GetDiagnosticCounters().Stopped && !ObjectRegistry::Get().Lookup(ReacquiredId),
			"shutdown retained content session or its authoritative identity");
		Fixture.World->Destroy();

		RuntimeFixture Replacement(52, "CrossMeshAReplacement");
		Until([&] { Replacement.Runtime->Step(); }, [&] {
			return Replacement.Runtime->Content->IsManifestAvailable();
		}, "replacement package manifest did not become available");
		Check(Replacement.Runtime->Content->RequestContent("a"), "replacement namespace demand rejected");
		Until([&] { Replacement.Runtime->Step(); }, [&] {
			return Replacement.Runtime->Content->GetState("a") == ContentResidencyState::Resident;
		}, "replacement package did not admit");
		const auto ReplacementObject = Replacement.Runtime->Workspace->FindFirstChild("CrossMeshAReplacement", false);
		Check(ReplacementObject && ReplacementObject->GetObjectId() != FirstId &&
			!ObjectRegistry::Get().Lookup(FirstId),
			"replacement package resolved a stale prior-session identity");
		std::cout << "[Content:CrossFoundationLifecycle] verifiedBytes="
			<< Runtime.Content->GetMetrics().BytesVerified << " admissions="
			<< Runtime.Content->GetMetrics().Admissions << " evictions="
			<< Runtime.Content->GetMetrics().Evictions << "\n";
	}
}

int main() {
	try {
		Check(SDL_Init(0), "SDL initialization failed");
		struct SdlLifetime final { ~SdlLifetime() { SDL_Quit(); } } Sdl;
		gargantuan::BootstrapNativeRuntimeSchema();
		IndependentLifecycle();
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Content:CrossFoundationLifecycle] FAIL: " << Failure.what() << '\n';
		return 1;
	}
}

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		Require(Output.good(), "fixture source write failed");
	}

	void SetPayload(GamePayload::ContentUnit &Unit, const Json &Document) {
		Unit.Payload = Document.dump();
		Unit.Entry.CompressedBytes = Unit.Entry.UncompressedBytes = Unit.Payload.size();
		Unit.Entry.Digest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Unit.Payload.data()), Unit.Payload.size()));
	}

	Json &MeshNode(Json &Document) {
		for (auto &Child : Document["Children"])
			if (Child.value("ClassName", "") == "MeshPart") return Child;
		throw std::runtime_error("fixture lost its imported MeshPart");
	}

	struct Fixture final {
		std::filesystem::path Root = std::filesystem::temp_directory_path() /
			("gargantuan-package-spatial-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::path ProjectRoot = Root / "Project";
		std::unique_ptr<DiskFilesystem> Filesystem;
		std::shared_ptr<DataModel> World;
		GamePayload Baseline;
		std::size_t Sequence = 0;

		Fixture() {
			std::filesystem::create_directory(Root);
			std::filesystem::copy(std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), ProjectRoot,
				std::filesystem::copy_options::recursive);
			std::filesystem::create_directories(ProjectRoot / "assets");
			BootstrapProjectRuntimeSchema(ProjectRoot);
			Filesystem = std::make_unique<DiskFilesystem>(ProjectRoot);
			SourceMount Mount(*Filesystem);
			const auto ProjectValue = Project::fromExisting(Filesystem.get());
			World = std::make_shared<DataModel>();
			World->SetName("PackageSpatialMetadata");
			World->Root = ProjectRoot;
			World->Filesystem = Filesystem.get();
			World->InitializeLoadedProjectRevision();
			auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
			Require(Space && Assets, "fixture lacks canonical services");
			WriteText(ProjectRoot / "assets/offset.obj",
				"v 10 -1 0\nv 12 -1 0\nv 11 1 0\nf 1 2 3\n");
			const auto Imported = Assets->ImportProjectAsset(Mount, "assets/offset.obj", AssetKind::Mesh);
			Require(Imported.Ok && Imported.Record, "canonical OBJ fixture import failed");
			auto Unit = std::make_shared<Folder>();
			Unit->SetName("MixedUnit");
			Unit->SetParent(Space);
			auto Mesh = std::make_shared<MeshPart>();
			Mesh->SetName("OffsetMesh");
			Mesh->SetMesh(Imported.Record->Reference.Value);
			Mesh->SetAnchored(true);
			Mesh->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
			Mesh->SetSize({1, 1, 1});
			Mesh->SetCFrame(CFrame());
			Mesh->SetParent(Unit);
			auto Primitive = std::make_shared<Part>();
			Primitive->SetName("OrdinaryMember");
			Primitive->SetAnchored(true);
			Primitive->SetSize({1, 1, 1});
			Primitive->SetCFrame(CFrame(-10, 0, 0));
			Primitive->SetParent(Unit);
			World->MarkPersistenceSubtreeArchivable();
			const auto Revision = World->GetAuthoritativeRevision();
			Baseline = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
			Require(Baseline.ContentUnits.size() == 1 && Baseline.ContentUnits[0].Entry.ObjectCount == 3 &&
				Baseline.ContentUnits[0].Entry.CoarseBounds &&
				Baseline.ContentUnits[0].Entry.CoarseBounds->Minimum[0] <= -10.5f &&
				Baseline.ContentUnits[0].Entry.CoarseBounds->Maximum[0] >= 12.0f,
				"actual mixed-unit producer omitted primitive or imported geometry");
		}

		~Fixture() {
			if (World) World->Destroy();
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		void CheckSnapshot(const GamePayload &Payload, bool Accepted, std::string_view Message) {
			std::vector<PackagePhase> Observed;
			const auto Output = Root / ("Output" + std::to_string(++Sequence));
			const auto Result = PackageBuilder::Build({
				.Payload = Payload,
				// Deliberately absent: a valid snapshot reaches Validate; an invalid
				// spatial proof must fail before this unrelated runtime lookup.
				.RuntimeDistributionRoot = Root / "UnreachableRuntime",
				.OutputDirectory = Output,
				.Progress = [&](PackagePhase Phase) { Observed.push_back(Phase); },
			});
			Require(!Result.Ok && !std::filesystem::exists(Output), "sentinel build published an output");
			Require(!Observed.empty() && Observed.front() == PackagePhase::Snapshot, "snapshot phase was not observed");
			Require(Accepted ? Observed.size() == 2 && Observed[1] == PackagePhase::Validate : Observed.size() == 1,
				Message);
		}
	};

	void CheckProducerDenials(Fixture &Test) {
		Test.CheckSnapshot(Test.Baseline, true, "qualified captured snapshot failed before runtime validation");
		auto Changed = Test.Baseline;
		Changed.ContentUnits[0].Entry.CoarseBounds = PackageCoarseBounds{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
		Test.CheckSnapshot(Changed, false, "underbounded mixed geometry reached package staging");
		Changed = Test.Baseline;
		Changed.ContentUnits[0].Entry.CoarseBounds.reset();
		Test.CheckSnapshot(Changed, false, "missing geometry bounds reached package staging");
		Changed = Test.Baseline;
		Changed.ContentUnits[0].Entry.PackageSpaceKey = "unqualified";
		Test.CheckSnapshot(Changed, false, "unsupported package space reached package staging");
		Changed = Test.Baseline;
		auto Document = Json::parse(Changed.ContentUnits[0].Payload);
		MeshNode(Document)["Properties"]["Mesh"]["String"] = "asset://ffffffffffffffffffffffffffffffff";
		SetPayload(Changed.ContentUnits[0], Document);
		Test.CheckSnapshot(Changed, false, "rehash of missing mesh identity bypassed geometry proof");
		Changed = Test.Baseline;
		Document = Json::parse(Changed.ContentUnits[0].Payload);
		MeshNode(Document)["Properties"]["CFrame"]["CFrame"][0] = nullptr;
		SetPayload(Changed.ContentUnits[0], Document);
		Test.CheckSnapshot(Changed, false, "malformed spatial member was silently omitted from mixed-unit proof");
		Changed = Test.Baseline;
		Document = Json::parse(Changed.ContentUnits[0].Payload);
		MeshNode(Document)["Properties"]["Anchored"]["Bool"] = false;
		SetPayload(Changed.ContentUnits[0], Document);
		Test.CheckSnapshot(Changed, false, "movable spatial member was accepted as immutable package bounds");
		Changed = Test.Baseline;
		Document = Json::parse(Changed.ContentUnits[0].Payload);
		MeshNode(Document)["Properties"]["CFrame"]["CFrame"][4] = 0.5f;
		SetPayload(Changed.ContentUnits[0], Document);
		Test.CheckSnapshot(Changed, false, "non-rigid spatial basis bypassed supported transform gate");
		Changed = Test.Baseline;
		Require(!Changed.Assets.Artifacts.empty() && Changed.Assets.Artifacts[0].Bytes &&
			!Changed.Assets.Artifacts[0].Bytes->empty(), "fixture artifact closure is absent");
		auto Corrupt = std::make_shared<std::vector<std::uint8_t>>(*Changed.Assets.Artifacts[0].Bytes);
		Corrupt->back() ^= 1;
		Changed.Assets.Artifacts[0].Bytes = std::move(Corrupt);
		Test.CheckSnapshot(Changed, false, "changed canonical artifact bytes bypassed exact producer proof");
	}

	void CheckNonspatialDependency(Fixture &Test) {
		auto Changed = Test.Baseline;
		GamePayload::ContentUnit Dependency;
		Dependency.Entry.Key = "zz/dependency";
		Dependency.Entry.BlobReference = "content/dependency.instance.json";
		Dependency.Entry.ObjectCount = 1;
		SetPayload(Dependency, Json{{"Version", PackageContentInstanceSchemaVersion}, {"Name", "Dependency"},
			{"ClassName", "Folder"}, {"Properties", Json::object()}, {"Children", Json::array()}});
		Changed.ContentUnits[0].Entry.Dependencies = {Dependency.Entry.Key};
		Changed.ContentUnits.push_back(std::move(Dependency));
		Test.CheckSnapshot(Changed, true, "valid nonspatial hard dependency lost 3L compatibility");
	}
}

int main() {
	if (!SDL_Init(0)) {
		std::cerr << "[Package:SpatialMetadata] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime final { ~SdlLifetime() { SDL_Quit(); } } Sdl;
	try {
		Fixture Test;
		CheckProducerDenials(Test);
		CheckNonspatialDependency(Test);
		std::cout << "[Package:SpatialMetadata] actual mixed geometry, exact proof and safe snapshot denials passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:SpatialMetadata] " << Error.what() << '\n';
		return 1;
	}
}

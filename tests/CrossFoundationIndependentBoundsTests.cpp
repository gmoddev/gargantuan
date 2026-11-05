#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	namespace Filesystem = std::filesystem;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::string ReadText(const Filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		Require(Input.good(), "package output file is unavailable");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	void WriteText(const Filesystem::path &Path, std::string_view Value) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Value.data(), static_cast<std::streamsize>(Value.size()));
		Require(Output.good(), "asset source fixture could not be written");
	}

	class TestWorkspace final {
	  public:
		Filesystem::path Root;
		Filesystem::path ProjectRoot;

		TestWorkspace() {
			const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
				"-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Root = Filesystem::temp_directory_path() / ("g-cf-bounds-" + Suffix);
			Filesystem::create_directory(Root);
			ProjectRoot = Root / "project";
			Filesystem::copy(Filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), ProjectRoot,
				Filesystem::copy_options::recursive);
			Filesystem::create_directories(ProjectRoot / "assets");
		}

		~TestWorkspace() {
			std::error_code Ignored;
			Filesystem::remove_all(Root, Ignored);
		}
	};

	std::shared_ptr<MeshPart> AddMeshPart(const std::shared_ptr<Instance> &Parent, std::string Name,
		const std::string &Reference, glm::vec3 Size, CFrame Frame, bool Anchored = true) {
		auto PartValue = std::make_shared<MeshPart>();
		PartValue->SetName(std::move(Name));
		PartValue->SetMesh(Reference);
		PartValue->SetSize(Size);
		PartValue->SetCFrame(Frame);
		PartValue->SetAnchored(Anchored);
		PartValue->SetParent(Parent);
		return PartValue;
	}

	const GamePayload::ContentUnit &FindUnit(const GamePayload &Payload, std::string_view Name) {
		const auto Found = std::ranges::find_if(Payload.ContentUnits, [&](const auto &Unit) {
			return Json::parse(Unit.Payload).value("Name", "") == Name;
		});
		Require(Found != Payload.ContentUnits.end(), "expected Workspace subtree was not packaged");
		return *Found;
	}

	bool BootstrapHas(const GamePayload &Payload, std::string_view Name) {
		const auto Project = Json::parse(Payload.ProjectJson);
		for (const auto &Child : Project["Children"])
			if (Child.value("ClassName", "") == "Workspace")
				for (const auto &Root : Child["Children"])
					if (Root.value("Name", "") == Name) return true;
		return false;
	}

	void RequireContains(const PackageCoarseBounds &Bounds, glm::dvec3 Point, std::string_view Message) {
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
			Require(static_cast<double>(Bounds.Minimum[Axis]) <= Point[Axis] &&
				static_cast<double>(Bounds.Maximum[Axis]) >= Point[Axis], Message);
	}

	std::vector<glm::dvec3> WorldVertices(const MeshPart &PartValue, const ImportedMesh &Mesh) {
		Require(Mesh.Vertices && !Mesh.Vertices->empty(), "import did not yield render vertices");
		const auto Frame = PartValue.GetCFrame();
		const auto Scale = PartValue.GetSize();
		std::vector<glm::dvec3> Points;
		Points.reserve(Mesh.Vertices->size());
		for (const auto &Vertex : *Mesh.Vertices) {
			glm::dvec3 Point(Frame.Position);
			for (std::size_t Row = 0; Row < 3; ++Row)
				for (std::size_t Column = 0; Column < 3; ++Column)
					Point[Row] += static_cast<double>(Frame.Rotation[Column][Row]) *
						static_cast<double>(Scale[Column]) * static_cast<double>(Vertex.Position[Column]);
			Points.push_back(Point);
		}
		return Points;
	}

	void RequirePartCornersInside(const PackageCoarseBounds &Bounds, const Part &PartValue) {
		const auto Frame = PartValue.GetCFrame();
		const auto Scale = PartValue.GetSize();
		for (std::size_t Corner = 0; Corner < 8; ++Corner) {
			glm::dvec3 Point(Frame.Position);
			for (std::size_t Row = 0; Row < 3; ++Row)
				for (std::size_t Column = 0; Column < 3; ++Column)
					Point[Row] += static_cast<double>(Frame.Rotation[Column][Row]) *
						static_cast<double>(Scale[Column]) *
						((Corner & (1u << Column)) ? 0.5 : -0.5);
			RequireContains(Bounds, Point, "aggregate bounds omit an ordinary Part corner");
		}
	}

	void CheckPackage(const TestWorkspace &Paths, const GamePayload &Payload,
		const GamePayload::ContentUnit &Expected, std::span<const glm::dvec3> GeometryPoints) {
		const auto Output = Paths.Root / "package";
		const auto Result = PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = Filesystem::absolute(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
			.OutputDirectory = Output,
		});
		Require(Result.Ok, "real package build rejected the independently captured bounds fixture");
		const auto Diagnostics = PackageBuilder::Validate(Output);
		Require(std::ranges::none_of(Diagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		}), "real package validation rejected the bounds fixture");
		std::vector<PackageDiagnostic> LoadDiagnostics;
		const auto Loaded = PackageBuilder::Load(Output, LoadDiagnostics);
		Require(Loaded.has_value(), "real package load rejected the bounds fixture");
		const auto Manifest = ParsePackageContentManifest(ReadText(Output / "content/content.manifest.json"));
		Require(Manifest.has_value() && Manifest->Entries.size() == Payload.ContentUnits.size(),
			"real package manifest lost content units");
		const auto Found = std::ranges::find(Manifest->Entries, Expected.Entry.Key,
			&PackageContentEntry::Key);
		Require(Found != Manifest->Entries.end() && Found->Digest == Expected.Entry.Digest &&
			Found->CoarseBounds.has_value(), "manifest lost the captured aggregate bounds");
		Require(Found->CoarseBounds->Minimum == Expected.Entry.CoarseBounds->Minimum &&
			Found->CoarseBounds->Maximum == Expected.Entry.CoarseBounds->Maximum,
			"package build changed the independently captured aggregate bounds");
		PackageContentCoarseIndex Index(*Manifest);
		for (const auto Point : GeometryPoints) {
			PackageCoarseBounds QueryBounds;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				QueryBounds.Minimum[Axis] = static_cast<float>(Point[Axis]);
				QueryBounds.Maximum[Axis] = static_cast<float>(Point[Axis]);
			}
			const auto Query = Index.Query("default", QueryBounds, Manifest->Entries.size());
			Require(std::ranges::find(Query, static_cast<std::size_t>(Found - Manifest->Entries.begin())) != Query.end(),
				"default-space package query missed transformed render geometry");
		}
	}
}

int main() {
	using namespace gargantuan;
	if (!SDL_Init(0)) {
		std::cerr << "[Package:IndependentBounds] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime final { ~SdlLifetime() { SDL_Quit(); } } Sdl;
	try {
		TestWorkspace Paths;
		BootstrapProjectRuntimeSchema(Paths.ProjectRoot);
		DiskFilesystem ProjectFilesystem(Paths.ProjectRoot);
		SourceMount Mount(ProjectFilesystem);
		const auto ProjectValue = Project::fromExisting(&ProjectFilesystem);
		auto World = std::make_shared<DataModel>();
		World->SetName("IndependentBounds");
		World->Root = Paths.ProjectRoot;
		World->Filesystem = &ProjectFilesystem;
		World->InitializeLoadedProjectRevision();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		Require(WorkspaceValue && Assets, "authoring world lacks Workspace or AssetService");

		WriteText(Paths.ProjectRoot / "assets" / "offset.obj",
			"v -3.75 -1.25 0.5\nv 2.75 0.25 0.75\nv 0.125 3.5 -2.25\nf 1 2 3\n");
		const auto Imported = Assets->ImportProjectAsset(Mount, "assets/offset.obj", AssetKind::Mesh);
		Require(Imported.Ok && Imported.Record, "non-normalized OBJ import failed");
		const auto ImportedMesh = Assets->ResolveMeshResource(Imported.Record->Reference.Value);
		Require(ImportedMesh && ImportedMesh->Value.Vertices, "OBJ render geometry is unavailable");
		Require(std::ranges::any_of(*ImportedMesh->Value.Vertices, [](const auto &Vertex) {
			return std::abs(Vertex.Position.x) > 0.5f || std::abs(Vertex.Position.y) > 0.5f ||
				std::abs(Vertex.Position.z) > 0.5f;
		}), "OBJ fixture did not escape the Size/2 assumption");

		WriteText(Paths.ProjectRoot / "assets" / "generated.solid",
			R"({"Version":1,"Root":"offset","Nodes":[{"Id":"box","Kind":"Box","Size":[2,2,2]},{"Id":"offset","Kind":"Transform","Inputs":["box"],"Matrix":[1,0,0,0,0,1,0,0,0,0,1,0,4,0,0,1]}]})");
		const auto Generated = Assets->ImportProjectAsset(Mount, "assets/generated.solid", AssetKind::Mesh);
		Require(Generated.Ok && Generated.Record, "generated solid import failed");
		const auto GeneratedMesh = Assets->ResolveMeshResource(Generated.Record->Reference.Value);
		Require(GeneratedMesh && GeneratedMesh->Value.Vertices, "generated solid render geometry is unavailable");

		auto Aggregate = std::make_shared<Folder>();
		Aggregate->SetName("Aggregate");
		Aggregate->SetParent(WorkspaceValue);
		auto ImportedPart = AddMeshPart(Aggregate, "ImportedPart", Imported.Record->Reference.Value,
			{1.25f, 2.0f, 0.75f}, CFrame(1000.125f, -42.375f, 18.625f) * CFrame::Angles(0.0f, 0.785398163f, 0.0f));
		ImportedPart->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		auto GeneratedPart = AddMeshPart(Aggregate, "GeneratedPart", Generated.Record->Reference.Value,
			{0.625f, 1.5f, 2.0f}, CFrame(1002.25f, -44.125f, 20.5f) * CFrame::Angles(0.15f, 0.0f, -0.35f));
		GeneratedPart->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		auto Ordinary = std::make_shared<Part>();
		Ordinary->SetName("OrdinaryPart");
		Ordinary->SetAnchored(true);
		Ordinary->SetSize({1.125f, 2.25f, 0.875f});
		Ordinary->SetCFrame(CFrame(1016.125f, -43.25f, 18.875f) * CFrame::Angles(0.0f, 0.25f, 0.0f));
		Ordinary->SetParent(Aggregate);

		auto MissingRoot = std::make_shared<Folder>();
		MissingRoot->SetName("MissingRoot");
		MissingRoot->SetParent(WorkspaceValue);
		AddMeshPart(MissingRoot, "MissingMesh", "asset://ffffffffffffffffffffffffffffffff",
			{1, 1, 1}, CFrame());
		auto ValidSibling = std::make_shared<Part>();
		ValidSibling->SetAnchored(true);
		ValidSibling->SetParent(MissingRoot);
		auto IncompatibleRoot = std::make_shared<Folder>();
		IncompatibleRoot->SetName("IncompatibleRoot");
		IncompatibleRoot->SetParent(WorkspaceValue);
		const auto Collision = Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value);
		Require(Collision.has_value(), "OBJ fixture lacks its collision companion");
		AddMeshPart(IncompatibleRoot, "CollisionIsNotMesh", AssetReference::FromAssetId(Collision->Id).Value,
			{1, 1, 1}, CFrame());
		auto Unanchored = std::make_shared<Part>();
		Unanchored->SetName("UnanchoredRoot");
		Unanchored->SetAnchored(false);
		Unanchored->SetParent(WorkspaceValue);

		World->MarkPersistenceSubtreeArchivable();
		const auto Revision = World->GetAuthoritativeRevision();
		const auto Payload = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		Require(Payload.ContentUnits.size() == 1, "unsafe or incompatible subtree entered spatial content");
		Require(BootstrapHas(Payload, "MissingRoot") && BootstrapHas(Payload, "IncompatibleRoot") &&
			BootstrapHas(Payload, "UnanchoredRoot"), "unsafe subtree did not retain its bootstrap fallback");
		Require(!BootstrapHas(Payload, "Aggregate"), "qualified aggregate remained in bootstrap");
		const auto &Unit = FindUnit(Payload, "Aggregate");
		Require(Unit.Entry.ObjectCount == 4 && Unit.Entry.CoarseBounds.has_value(),
			"aggregate did not retain one bounded four-object content unit");
		const auto ImportedPoints = WorldVertices(*ImportedPart, ImportedMesh->Value);
		const auto GeneratedPoints = WorldVertices(*GeneratedPart, GeneratedMesh->Value);
		for (const auto Point : ImportedPoints)
			RequireContains(*Unit.Entry.CoarseBounds, Point,
				"aggregate bounds omit an imported vertex after CFrame and Size");
		for (const auto Point : GeneratedPoints)
			RequireContains(*Unit.Entry.CoarseBounds, Point,
				"aggregate bounds omit a compiled solid vertex after CFrame and Size");
		RequirePartCornersInside(*Unit.Entry.CoarseBounds, *Ordinary);
		std::vector<glm::dvec3> GeometryPoints = ImportedPoints;
		GeometryPoints.insert(GeometryPoints.end(), GeneratedPoints.begin(), GeneratedPoints.end());
		CheckPackage(Paths, Payload, Unit, GeometryPoints);
		std::cout << "[Package:IndependentBounds] passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:IndependentBounds] " << Error.what() << '\n';
		return 1;
	}
}

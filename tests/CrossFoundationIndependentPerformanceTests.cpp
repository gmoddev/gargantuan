#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
	using namespace gargantuan;
	namespace Filesystem = std::filesystem;
	using Clock = std::chrono::steady_clock;
	constexpr std::size_t GridCells = 64;
	constexpr std::size_t TriangleCount = GridCells * GridCells * 2;
	constexpr std::size_t VertexCount = (GridCells + 1) * (GridCells + 1);
	constexpr std::size_t SharedPartCount = 128;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::int64_t Microseconds(Clock::time_point Started) {
		return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - Started).count();
	}

	template <typename PayloadType> std::string SpatialInspectionMicroseconds(const PayloadType &Payload) {
		if constexpr (requires { Payload.SpatialInspectionDuration; })
			return std::to_string(Payload.SpatialInspectionDuration.count());
		else return "unavailable"; // The 96a88d9 baseline has no per-inspection timer.
	}

	std::string_view PhaseName(PackagePhase Phase) {
		switch (Phase) {
		case PackagePhase::Snapshot: return "Snapshot";
		case PackagePhase::Validate: return "Validate";
		case PackagePhase::StageRuntime: return "StageRuntime";
		case PackagePhase::StageContent: return "StageContent";
		case PackagePhase::HashManifest: return "HashManifest";
		case PackagePhase::Finalize: return "Finalize";
		case PackagePhase::Complete: return "Complete";
		}
		return "Unknown";
	}

	class TestWorkspace final {
	  public:
		Filesystem::path Root;
		Filesystem::path ProjectRoot;

		TestWorkspace() {
			const auto Suffix = std::to_string(Clock::now().time_since_epoch().count()) + "-" +
				std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Root = Filesystem::temp_directory_path() / ("gargantuan-independent-performance-" + Suffix);
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

	void WriteGridObj(const Filesystem::path &Path) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Require(Output.good(), "could not create the bounded OBJ fixture");
		for (std::size_t Row = 0; Row <= GridCells; ++Row)
			for (std::size_t Column = 0; Column <= GridCells; ++Column)
				Output << "v " << 10 + Column << ' ' << Row << " 0\n";
		for (std::size_t Row = 0; Row < GridCells; ++Row)
			for (std::size_t Column = 0; Column < GridCells; ++Column) {
				const auto A = Row * (GridCells + 1) + Column + 1;
				const auto B = A + 1;
				const auto C = A + GridCells + 1;
				const auto D = C + 1;
				Output << "f " << A << ' ' << B << ' ' << C << '\n';
				Output << "f " << B << ' ' << D << ' ' << C << '\n';
			}
		Require(Output.good(), "could not finish the bounded OBJ fixture");
	}

	std::string ReadText(const Filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		Require(Input.good(), "packaged content manifest is unavailable");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	bool Contains(const PackageCoarseBounds &Bounds, const MeshPart &PartValue, const ImportedMesh &Mesh) {
		const auto Frame = PartValue.GetCFrame();
		const auto Scale = PartValue.GetSize();
		for (const auto &Vertex : *Mesh.Vertices)
			for (std::size_t Row = 0; Row < 3; ++Row) {
				double Position = static_cast<double>(Frame.Position[Row]);
				for (std::size_t Column = 0; Column < 3; ++Column)
					Position += static_cast<double>(Frame.Rotation[Column][Row]) *
						static_cast<double>(Scale[Column]) * static_cast<double>(Vertex.Position[Column]);
				if (Position < static_cast<double>(Bounds.Minimum[Row]) ||
					Position > static_cast<double>(Bounds.Maximum[Row])) return false;
			}
		return true;
	}

	bool HasError(const std::vector<PackageDiagnostic> &Diagnostics) {
		return std::ranges::any_of(Diagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		});
	}
}

int main(int ArgumentCount, char **Arguments) {
	using namespace gargantuan;
	const bool Baseline = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--baseline";
	if (ArgumentCount != 1 && !Baseline) {
		std::cerr << "[Package:IndependentPerformance] usage: [--baseline]\n";
		return 2;
	}
	if (!SDL_Init(0)) {
		std::cerr << "[Package:IndependentPerformance] SDL initialization failed\n";
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
		World->SetName("IndependentPerformance");
		World->Root = Paths.ProjectRoot;
		World->Filesystem = &ProjectFilesystem;
		World->InitializeLoadedProjectRevision();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		Require(WorkspaceValue && Assets, "authoring world lacks Workspace or AssetService");
		WriteGridObj(Paths.ProjectRoot / "assets" / "grid.obj");

		const auto ImportStarted = Clock::now();
		const auto Imported = Assets->ImportProjectAsset(Mount, "assets/grid.obj", AssetKind::Mesh);
		const auto ImportUs = Microseconds(ImportStarted);
		Require(Imported.Ok && Imported.Record && Imported.Records.size() == 2,
			"OBJ import did not publish one Mesh and one Collision");
		const auto Mesh = Assets->ResolveMeshResource(Imported.Record->Reference.Value);
		const auto Collision = Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value);
		Require(Mesh && Collision && Mesh->Value.Vertices && Mesh->Value.Indices &&
			Collision->Value.Positions && Collision->Value.Indices,
			"canonical render and collision geometry is unavailable");
		Require(Mesh->Value.Vertices->size() == VertexCount &&
			Mesh->Value.Indices->size() == TriangleCount * 3 &&
			Collision->Value.Positions->size() == VertexCount &&
			Collision->Value.Indices->size() == TriangleCount * 3,
			"bounded OBJ fixture geometry counts changed");
		const auto CollisionReference = AssetReference::FromAssetId(Collision->Id).Value;
		const auto CollisionRecord = Assets->GetAsset(CollisionReference);
		Require(CollisionRecord && CollisionRecord->Kind == AssetKind::Collision,
			"imported collision companion has no catalog identity");

		auto Group = std::make_shared<Folder>();
		Group->SetName("SharedGrid");
		Group->SetParent(WorkspaceValue);
		std::vector<std::shared_ptr<MeshPart>> Parts;
		Parts.reserve(SharedPartCount);
		for (std::size_t Index = 0; Index < SharedPartCount; ++Index) {
			auto PartValue = std::make_shared<MeshPart>();
			PartValue->SetName("Grid" + std::to_string(Index));
			PartValue->SetMesh(Imported.Record->Reference.Value);
			PartValue->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
			PartValue->SetAnchored(true);
			PartValue->SetSize({1.0f, 1.0f, 1.0f});
			PartValue->SetCFrame(CFrame(static_cast<float>(Index), 0.0f, 0.0f));
			PartValue->SetParent(Group);
			Parts.push_back(std::move(PartValue));
		}
		Require(std::ranges::all_of(Parts, [&](const auto &PartValue) {
			return PartValue->GetMesh() == Imported.Record->Reference.Value;
		}), "MeshParts did not retain one shared canonical Mesh reference");
		World->MarkPersistenceSubtreeArchivable();
		const auto Revision = World->GetAuthoritativeRevision();

		const auto CaptureStarted = Clock::now();
		const auto Payload = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		const auto CaptureUs = Microseconds(CaptureStarted);
		Require(Payload.ContentUnits.size() == 1 && Payload.ContentUnits.front().Entry.ObjectCount ==
			SharedPartCount + 1 && Payload.ContentUnits.front().Entry.CoarseBounds,
			"shared fixture did not capture as one bounded content unit");
		Require(Payload.Assets.Artifacts.size() == 2,
			"runtime asset snapshot did not retain exactly one Mesh and Collision artifact");
		std::size_t ArtifactBytes = 0;
		std::size_t MeshArtifactBytes = 0;
		std::size_t CollisionArtifactBytes = 0;
		for (const auto &Artifact : Payload.Assets.Artifacts) {
			Require(Artifact.Bytes && !Artifact.Bytes->empty(), "runtime artifact bytes are missing");
			ArtifactBytes += Artifact.Bytes->size();
			if (Artifact.RelativePath == "assets/artifacts/" + Imported.Record->ContentId.ToString() + ".gasset")
				MeshArtifactBytes = Artifact.Bytes->size();
			if (Artifact.RelativePath == "assets/artifacts/" + CollisionRecord->ContentId.ToString() + ".gasset")
				CollisionArtifactBytes = Artifact.Bytes->size();
		}
		Require(MeshArtifactBytes > 0 && CollisionArtifactBytes > 0,
			"runtime asset snapshot lacks the exact Mesh or Collision digest");
		const auto &Unit = Payload.ContentUnits.front();
		const bool CapturedBoundsCoverGeometry = std::ranges::all_of(Parts, [&](const auto &PartValue) {
			return Contains(*Unit.Entry.CoarseBounds, *PartValue, Mesh->Value);
		});
		if (!Baseline) Require(CapturedBoundsCoverGeometry,
			"candidate capture underbounded transformed canonical Mesh vertices");

		const auto Output = Paths.Root / "package";
		const auto BuildStarted = Clock::now();
		const auto Built = PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = Filesystem::absolute(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT),
			.OutputDirectory = Output,
		});
		const auto BuildUs = Microseconds(BuildStarted);
		if (!Built.Ok || HasError(Built.Diagnostics))
			for (const auto &Diagnostic : Built.Diagnostics)
				std::cerr << "[Package:IndependentPerformance] diagnostic=" << Diagnostic.Code
					<< " message=" << Diagnostic.Message << " item=" << Diagnostic.Item << '\n';
		Require(Built.Ok && !HasError(Built.Diagnostics), "real Release package build failed");
		const auto ValidateStarted = Clock::now();
		const auto Validation = PackageBuilder::Validate(Output);
		const auto ValidateUs = Microseconds(ValidateStarted);
		Require(!HasError(Validation), "real Release package validation failed");
		std::vector<PackageDiagnostic> LoadDiagnostics;
		const auto LoadStarted = Clock::now();
		const auto Loaded = PackageBuilder::Load(Output, LoadDiagnostics);
		const auto LoadUs = Microseconds(LoadStarted);
		Require(Loaded && !HasError(LoadDiagnostics), "real Release package load failed");
		const auto Manifest = ParsePackageContentManifest(ReadText(Output / "content/content.manifest.json"));
		Require(Manifest && Manifest->Entries.size() == 1 && Manifest->Entries.front().CoarseBounds &&
			Manifest->Entries.front().Digest == Unit.Entry.Digest,
			"packaged manifest lost its one shared content unit");
		const bool ManifestBoundsCoverGeometry = std::ranges::all_of(Parts, [&](const auto &PartValue) {
			return Contains(*Manifest->Entries.front().CoarseBounds, *PartValue, Mesh->Value);
		});
		if (!Baseline) Require(ManifestBoundsCoverGeometry,
			"candidate manifest underbounded transformed canonical Mesh vertices");
		std::cout << "[Package:IndependentPerformance] mode=" << (Baseline ? "baseline" : "candidate")
			<< " vertices=" << VertexCount << " triangles=" << TriangleCount
			<< " sharedParts=" << SharedPartCount << " uniqueMeshReferences=1"
			<< " meshDigest=" << Imported.Record->ContentId.ToString()
			<< " collisionDigest=" << CollisionRecord->ContentId.ToString()
			<< " objSourceBytes=" << Filesystem::file_size(Paths.ProjectRoot / "assets" / "grid.obj")
			<< " meshArtifactBytes=" << MeshArtifactBytes
			<< " collisionArtifactBytes=" << CollisionArtifactBytes
			<< " artifactBytes=" << ArtifactBytes << " contentUnitBytes=" << Unit.Payload.size()
			<< " captureBoundsCoverGeometry=" << CapturedBoundsCoverGeometry
			<< " manifestBoundsCoverGeometry=" << ManifestBoundsCoverGeometry
			<< " underboundDetected=" << (!CapturedBoundsCoverGeometry || !ManifestBoundsCoverGeometry)
			<< " importUs=" << ImportUs << " captureUs=" << CaptureUs
			<< " spatialInspectionUs=" << SpatialInspectionMicroseconds(Payload)
			<< " buildUs=" << BuildUs;
		for (const auto &Timing : Built.Timings)
			std::cout << " build" << PhaseName(Timing.Phase) << "Us=" << Timing.Duration.count();
		std::cout << " explicitValidateUs=" << ValidateUs << " loadUs=" << LoadUs
			<< " packageBytes=" << Built.Size.TotalBytes << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:IndependentPerformance] " << Error.what() << '\n';
		return 1;
	}
}

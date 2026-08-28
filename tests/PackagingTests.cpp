#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <thread>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	class TestWorkspace final {
	  public:
		TestWorkspace() {
			const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
								std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Root = std::filesystem::temp_directory_path() / ("gargantuan-packaging-" + Suffix);
			std::filesystem::create_directory(Root);
			ProjectRoot = Root / "FirstCompleteGame";
			std::filesystem::copy(
				std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT),
				ProjectRoot,
				std::filesystem::copy_options::recursive
			);
		}

		~TestWorkspace() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		std::filesystem::path Root;
		std::filesystem::path ProjectRoot;
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::string ReadText(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) throw std::runtime_error("Could not read packaging test fixture");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	void WriteText(const std::filesystem::path &Path, std::string_view Text) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		if (!Output) throw std::runtime_error("Could not write packaging test fixture");
	}

	bool HasError(const std::vector<PackageDiagnostic> &Diagnostics) {
		return std::ranges::any_of(Diagnostics, [](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
		});
	}

	bool HasDiagnostic(const std::vector<PackageDiagnostic> &Diagnostics, std::string_view Code) {
		return std::ranges::any_of(Diagnostics, [&](const PackageDiagnostic &Diagnostic) {
			return Diagnostic.Code == Code;
		});
	}

	PackageBuildResult Build(
		const GamePayload &Payload,
		const std::filesystem::path &Runtime,
		const std::filesystem::path &Output,
		PackageConfiguration Configuration = PackageConfiguration::Release,
		PackageFailurePoint Failure = PackageFailurePoint::None,
		PackageCancellationToken Cancellation = {}
	) {
		return PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = Runtime,
			.OutputDirectory = Output,
			.Configuration = Configuration,
			.Cancellation = std::move(Cancellation),
			.FailurePoint = Failure,
		});
	}

	void CopyPackage(const std::filesystem::path &Source, const std::filesystem::path &Destination) {
		std::filesystem::copy(Source, Destination, std::filesystem::copy_options::recursive);
	}

	void ExpectInvalid(const std::filesystem::path &Root, std::string_view Message) {
		Require(HasError(PackageBuilder::Validate(Root)), Message);
	}

	void RequireNoPrivateBackendArtifacts(const std::filesystem::path &Root) {
		for (const auto &Entry : std::filesystem::recursive_directory_iterator(Root)) {
			auto Relative = Entry.path().lexically_relative(Root).generic_string();
			std::ranges::transform(Relative, Relative.begin(), [](unsigned char Character) {
				return static_cast<char>(std::tolower(Character));
			});
			Require(
				Relative.find("gargantuan-node") == std::string::npos &&
					Relative.find("nodeentitlement") == std::string::npos &&
					Relative.find("node_entitlement") == std::string::npos &&
					Relative.find("private-node-entitlements") == std::string::npos &&
					Relative.find("entitlements.grpc.pb") == std::string::npos && !Relative.ends_with(".proto"),
				"generic package included a private backend adapter, schema, or configuration artifact"
			);
		}
	}
}

int main() {
	using namespace gargantuan;
	if (!SDL_Init(0)) {
		std::cerr << "[Package:Test] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;
	try {
		TestWorkspace Workspace;
		const auto Runtime = std::filesystem::absolute(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT).lexically_normal();
		BootstrapProjectRuntimeSchema(Workspace.ProjectRoot);
		DiskFilesystem Filesystem(Workspace.ProjectRoot);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto ReopenedProject = Project::fromExisting(&Filesystem);
		Require(
			ProjectValue.Identity == ReopenedProject.Identity && ProjectValue.Identity.IsValid(),
			"project identity did not remain stable across reopen"
		);
		auto World = ProjectValue.DeserializeGame();
		World->InitializeLoadedProjectRevision();
		World->Root = Workspace.ProjectRoot;
		World->Filesystem = &Filesystem;
		const auto InitialRevision = World->GetAuthoritativeRevision();
		auto Payload = PackageBuilder::Capture(ProjectValue, World, InitialRevision, InitialRevision);
		Require(
			Payload.Identity == ProjectValue.Identity && !Payload.UnsavedChanges,
			"package snapshot identity/revision state is incorrect"
		);
		const auto RuntimeCatalog = Json::parse(Payload.Assets.CatalogJson);
		Require(
			RuntimeCatalog.value("Format", "") == "GargantuanRuntimeAssets" &&
				RuntimeCatalog.value("Version", 0) == 1 && RuntimeCatalog.contains("Assets"),
			"runtime asset catalog contract is missing"
		);
		for (const auto &Asset : RuntimeCatalog["Assets"])
			Require(
				!Asset.contains("Source") && !Asset.contains("SourceGroupId") && !Asset.contains("LogicalKey") &&
					Asset.size() == 8,
				"runtime asset catalog leaked authoring provenance"
			);
		Require(std::ranges::any_of(RuntimeCatalog["Assets"], [](const Json &Asset) {
			return Asset.value("Kind", "") == "Audio" &&
				Asset.value("Reference", "") == "asset://a0d10f78c368437daac002a4e59fdd64";
		}), "FirstCompleteGame canonical Audio asset is missing from the runtime package closure");
		auto AnimationAsset = std::ranges::find_if(RuntimeCatalog["Assets"], [](const Json &Asset) {
			return Asset.value("Kind", "") == "Animation";
		});
		Require(AnimationAsset != RuntimeCatalog["Assets"].end() &&
			(*AnimationAsset)["Dependencies"].size() == 1 &&
			std::ranges::any_of(RuntimeCatalog["Assets"], [&](const Json &Asset) {
				return Asset.value("Kind", "") == "Mesh" &&
				Asset.value("AssetId", "") == (*AnimationAsset)["Dependencies"][0].get<std::string>();
			}), "FirstCompleteGame Animation and its compatible skinned Mesh are not a closed package dependency");

		const auto ReleaseA = Workspace.Root / "ReleaseA";
		const auto ReleaseB = Workspace.Root / "ReleaseB";
		const auto BuildStarted = std::chrono::steady_clock::now();
		auto First = Build(Payload, Runtime, ReleaseA);
		const auto BuildMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - BuildStarted).count();
		auto Second = Build(Payload, Runtime, ReleaseB);
		Require(
			First.Ok && Second.Ok && !HasError(First.Diagnostics) && !HasError(Second.Diagnostics),
			"clean Release package did not build twice"
		);
		Require(
			ReadText(ReleaseA / "game.package.json") == ReadText(ReleaseB / "game.package.json"),
			"equivalent package inputs did not produce a deterministic manifest"
		);
		Require(
			First.Size.TotalBytes == First.Size.RuntimeBytes + First.Size.ProjectBytes + First.Size.AssetBytes +
										 First.Size.ShaderBytes + First.Size.OtherBytes,
			"package size breakdown does not sum to total bytes"
		);
		Require(!HasError(PackageBuilder::Validate(ReleaseA)), "fresh package did not validate");
		RequireNoPrivateBackendArtifacts(ReleaseA);
		std::vector<PackageDiagnostic> LoadDiagnostics;
		auto Loaded = PackageBuilder::Load(ReleaseA, LoadDiagnostics);
		Require(
			Loaded && Loaded->Inspection.Identity == Payload.Identity &&
				Loaded->Inspection.Revision == Payload.AuthoritativeRevision &&
				Loaded->Assets.Artifacts.size() == Payload.Assets.Artifacts.size(),
			"validated package payload did not load with its stable identity/assets"
		);
		const auto SourcePath = Workspace.ProjectRoot.generic_string();
		std::filesystem::remove_all(Workspace.ProjectRoot);
		BootstrapPackagedRuntimeSchema(Loaded->PreRunSource);
		auto PackagedWorld = PackageBuilder::LoadWorld(*Loaded, ReleaseA);
		Require(
			PackagedWorld->Filesystem == nullptr && PackagedWorld->Root == ReleaseA / "content",
			"packaged runtime retained authoring filesystem authority"
		);
		HeadlessRenderer Renderer(Vector2(640, 360));
		Engine RuntimeEngine(PackagedWorld, &Renderer);
		RuntimeEngine.ProcessService->Alive = true;
		RuntimeEngine.Step();
		auto InitialRuntimePublication = Renderer.TakeLastPublication();
		Require(
			InitialRuntimePublication != nullptr,
			"packaged runtime did not publish its initial assets/scene/GUI frame"
		);
		RuntimeEngine.Step();
		auto AnimationRuntimePublication = Renderer.TakeLastPublication();
		Require(AnimationRuntimePublication && AnimationRuntimePublication->AnimationPoseUpdates.size() == 1,
			"packaged runtime did not start and publish the canonical animated beacon");
		(void)RuntimeEngine.ProcessEvent(
			KeyEvent{{1}, PhysicalKey::W, LogicalKey::W, KeyModifier::None, ButtonState::Pressed}
		);
		RuntimeEngine.Step();
		(void)RuntimeEngine.ProcessEvent(
			KeyEvent{{1}, PhysicalKey::W, LogicalKey::W, KeyModifier::None, ButtonState::Released}
		);
		(void)RuntimeEngine.ProcessEvent(
			KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Pressed}
		);
		(void)RuntimeEngine.ProcessEvent(
			KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Released}
		);
		RuntimeEngine.Step();
		Require(
			Renderer.TakeLastPublication() != nullptr,
			"packaged gameplay input/scripts/physics/GUI did not produce a completion frame"
		);
		(void)RuntimeEngine.ProcessEvent(
			PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Pressed, {320.0f, 241.0f}}
		);
		(void)RuntimeEngine.ProcessEvent(
			PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Released, {320.0f, 241.0f}}
		);
		RuntimeEngine.Step();
		Require(
			Renderer.TakeLastPublication() != nullptr, "packaged GUI restart input did not produce a runtime frame"
		);
		RuntimeEngine.Destroy();
		PackagedWorld.reset();

		const auto Development = Workspace.Root / "Development";
		auto DevelopmentResult = Build(Payload, Runtime, Development, PackageConfiguration::Development);
		Require(
			DevelopmentResult.Ok &&
				Json::parse(ReadText(Development / "game.package.json"))["Configuration"] == "Development",
			"Development package configuration was not recorded and validated"
		);

		const auto Relocated = Workspace.Root / "Relocated";
		CopyPackage(ReleaseA, Relocated);
		Require(!HasError(PackageBuilder::Validate(Relocated)), "relocated package depended on its original directory");
		std::vector<PackageDiagnostic> InspectionDiagnostics;
		auto Inspection = PackageBuilder::Inspect(Relocated, InspectionDiagnostics);
#if defined(_WIN32)
		constexpr const char *PlayerFileName = "GargantuanPlayer.exe";
#else
		constexpr const char *PlayerFileName = "GargantuanPlayer";
#endif
		Require(
			Inspection && Inspection->PlayerExecutable == Relocated / PlayerFileName,
			"relocated package did not derive its player from the package root"
		);
#if !defined(_WIN32)
		constexpr auto ExecutePermissions = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
											std::filesystem::perms::others_exec;
		Require(
			(std::filesystem::status(ReleaseA / PlayerFileName).permissions() & ExecutePermissions) ==
					(std::filesystem::status(Runtime / PlayerFileName).permissions() & ExecutePermissions) &&
				(std::filesystem::status(ReleaseA / PlayerFileName).permissions() & ExecutePermissions) !=
					std::filesystem::perms::none,
			"package staging did not preserve the player executable permissions"
		);
#endif

		const auto Manifest = Json::parse(ReadText(ReleaseA / "game.package.json"));
		Require(
			Manifest["Content"].size() >= 20 && Manifest["Player"] == PlayerFileName &&
				std::ranges::any_of(
					Manifest["Content"], [](const Json &Entry) { return Entry["Path"] == "notices/Gargantuan.txt"; }
				),
			"package manifest omitted player/runtime/notices"
		);
#if defined(_WIN32)
		for (const auto *RuntimeFile : {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"})
			Require(
				std::ranges::any_of(
					Manifest["Content"],
					[RuntimeFile](const Json &Entry) {
						return Entry["Path"] == RuntimeFile && Entry["Category"] == "Runtime";
					}
				),
				"package manifest omitted an explicit MSVC runtime dependency"
			);
#endif
		for (const auto &Entry : Manifest["Content"])
			Require(
				Entry["Path"].get<std::string>().find("telemetry") == std::string::npos,
				"optional telemetry entered the standalone runtime package"
			);
		for (const auto &Entry : Manifest["Content"]) {
			const auto PackageFile = ReleaseA / std::filesystem::path(Entry["Path"].get<std::string>());
			if (std::filesystem::file_size(PackageFile) <= 16 * 1024 * 1024) {
				const auto Content = ReadText(PackageFile);
				const auto EngineSourceRoot = std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT)
										  .parent_path()
										  .parent_path();
				Require(
					Content.find(SourcePath) == std::string::npos &&
						Content.find(EngineSourceRoot.string()) == std::string::npos &&
						Content.find(EngineSourceRoot.generic_string()) == std::string::npos,
					"package content exposed the authoring source root"
				);
			}
		}

		const auto OptionalRuntime = Workspace.Root / "RuntimeWithOptionalTelemetry";
		std::filesystem::copy(Runtime, OptionalRuntime, std::filesystem::copy_options::recursive);
		WriteText(OptionalRuntime / "optional-telemetry.dll", "not a declared runtime dependency");
		const auto OptionalOutput = Workspace.Root / "OptionalOutput";
		auto Optional = Build(Payload, OptionalRuntime, OptionalOutput);
		Require(
			Optional.Ok && ReadText(OptionalOutput / "game.package.json") == ReadText(ReleaseA / "game.package.json"),
			"undeclared optional runtime content changed the closed package output"
		);

		const auto Unrelated = Workspace.Root / "Unrelated";
		std::filesystem::create_directory(Unrelated);
		WriteText(Unrelated / "keep.txt", "preserve me");
		auto Refused = Build(Payload, Runtime, Unrelated);
		Require(
			!Refused.Ok && ReadText(Unrelated / "keep.txt") == "preserve me" &&
				!std::filesystem::exists(Unrelated / "game.package.json"),
			"packager overwrote an unrelated destination"
		);

		const auto BaselineManifest = ReadText(ReleaseA / "game.package.json");
		for (const auto Failure : {
				 PackageFailurePoint::Snapshot,
				 PackageFailurePoint::Validate,
				 PackageFailurePoint::StageRuntime,
				 PackageFailurePoint::StageContent,
				 PackageFailurePoint::HashManifest,
				 PackageFailurePoint::ManifestWrite,
				 PackageFailurePoint::Finalize,
			 }) {
			auto Failed = Build(Payload, Runtime, ReleaseA, PackageConfiguration::Release, Failure);
			Require(
				!Failed.Ok && ReadText(ReleaseA / "game.package.json") == BaselineManifest &&
					!HasError(PackageBuilder::Validate(ReleaseA)),
				"injected build failure did not preserve the previous valid destination"
			);
		}
		PackageCancellationToken Cancellation;
		Cancellation.Cancel();
		auto Cancelled = Build(
			Payload, Runtime, ReleaseA, PackageConfiguration::Release, PackageFailurePoint::None, Cancellation
		);
		Require(
			!Cancelled.Ok && Cancelled.Cancelled && ReadText(ReleaseA / "game.package.json") == BaselineManifest,
			"cancelled package build changed the previous valid destination"
		);
		for (const auto CancellationPhase : {
				 PackagePhase::StageRuntime,
				 PackagePhase::StageContent,
				 PackagePhase::HashManifest,
			 }) {
			PackageCancellationToken PhaseCancellation;
			auto PhaseCancelled = PackageBuilder::Build({
				.Payload = Payload,
				.RuntimeDistributionRoot = Runtime,
				.OutputDirectory = ReleaseA,
				.Configuration = PackageConfiguration::Release,
				.Cancellation = PhaseCancellation,
				.Progress = [PhaseCancellation, CancellationPhase](PackagePhase Phase) {
					if (Phase == CancellationPhase) PhaseCancellation.Cancel();
				},
			});
			Require(
				!PhaseCancelled.Ok && PhaseCancelled.Cancelled &&
					ReadText(ReleaseA / "game.package.json") == BaselineManifest,
				"phase cancellation changed the previous valid destination"
			);
		}
		Require(
			std::ranges::none_of(
				std::filesystem::directory_iterator(Workspace.Root),
				[](const auto &Entry) { return Entry.path().filename().string().starts_with(".gpk-"); }
			),
			"package failure or cancellation left a staging/backup sibling behind"
		);
		auto ReplacedPackage = Build(Payload, Runtime, ReleaseA);
		Require(
			ReplacedPackage.Ok && ReadText(ReleaseA / "game.package.json") == BaselineManifest,
			"recognized prior package was not replaced by a validated deterministic candidate"
		);

		const auto MissingRuntime = Workspace.Root / "MissingRuntime";
		std::filesystem::copy(Runtime, MissingRuntime, std::filesystem::copy_options::recursive);
		std::filesystem::remove(MissingRuntime / "shaders/gui.frag.spv");
		auto MissingResult = Build(Payload, MissingRuntime, Workspace.Root / "MissingOutput");
		Require(
			!MissingResult.Ok && HasDiagnostic(MissingResult.Diagnostics, "InvalidDistribution"),
			"missing required runtime shader was not diagnosed before publication"
		);

		auto StalePayload = Payload;
		auto StaleCatalog = Json::parse(StalePayload.Assets.CatalogJson);
		StaleCatalog["Assets"][0]["State"] = "Stale";
		StalePayload.Assets.CatalogJson = StaleCatalog.dump();
		auto Stale = Build(StalePayload, Runtime, Workspace.Root / "StalePackage");
		Require(
			Stale.Ok && HasDiagnostic(Stale.Diagnostics, "StaleAsset"),
			"last-known-good stale asset was not packaged with an explicit warning"
		);
		auto FailedPayload = Payload;
		auto FailedCatalog = Json::parse(FailedPayload.Assets.CatalogJson);
		FailedCatalog["Assets"][0]["State"] = "Failed";
		FailedPayload.Assets.CatalogJson = FailedCatalog.dump();
		auto FailedAsset = Build(FailedPayload, Runtime, Workspace.Root / "FailedAssetPackage");
		Require(
			!FailedAsset.Ok && HasDiagnostic(FailedAsset.Diagnostics, "InvalidRuntimeCatalog"),
			"failed runtime asset state was published instead of blocking the package"
		);
		auto MissingArtifactPayload = Payload;
		MissingArtifactPayload.Assets.Artifacts.pop_back();
		auto MissingArtifact = Build(MissingArtifactPayload, Runtime, Workspace.Root / "MissingArtifactPackage");
		Require(
			!MissingArtifact.Ok && HasDiagnostic(MissingArtifact.Diagnostics, "InvalidRuntimeCatalog"),
			"runtime asset catalog was packaged without its canonical artifact"
		);
		auto CorruptArtifactPayload = Payload;
		auto CorruptArtifactBytes = std::make_shared<std::vector<std::uint8_t>>(
			*CorruptArtifactPayload.Assets.Artifacts.front().Bytes
		);
		CorruptArtifactBytes->push_back(0xff);
		CorruptArtifactPayload.Assets.Artifacts.front().Bytes = std::move(CorruptArtifactBytes);
		auto CorruptArtifact = Build(CorruptArtifactPayload, Runtime, Workspace.Root / "CorruptArtifactPackage");
		Require(
			!CorruptArtifact.Ok && HasDiagnostic(CorruptArtifact.Diagnostics, "InvalidRuntimeCatalog"),
			"canonical AssetContentId did not verify the artifact bytes"
		);
		auto MissingDependencyPayload = Payload;
		auto MissingDependencyCatalog = Json::parse(MissingDependencyPayload.Assets.CatalogJson);
		MissingDependencyCatalog["Assets"][0]["Dependencies"].push_back("11111111111111111111111111111111");
		MissingDependencyPayload.Assets.CatalogJson = MissingDependencyCatalog.dump();
		auto MissingDependency = Build(MissingDependencyPayload, Runtime, Workspace.Root / "MissingDependencyPackage");
		Require(
			!MissingDependency.Ok && HasDiagnostic(MissingDependency.Diagnostics, "InvalidRuntimeCatalog"),
			"runtime asset catalog accepted a dependency outside its closed snapshot"
		);

		const auto Tampered = Workspace.Root / "Tampered";
		CopyPackage(ReleaseA, Tampered);
		WriteText(Tampered / "content/game.instance.json", ReadText(Tampered / "content/game.instance.json") + " ");
		ExpectInvalid(Tampered, "content hash mismatch was accepted");
		const auto Traversal = Workspace.Root / "Traversal";
		CopyPackage(ReleaseA, Traversal);
		auto TraversalManifest = Json::parse(ReadText(Traversal / "game.package.json"));
		TraversalManifest["Content"][0]["Path"] = "../escape";
		WriteText(Traversal / "game.package.json", TraversalManifest.dump());
		ExpectInvalid(Traversal, "manifest traversal path was accepted");
		const auto NewerVersion = Workspace.Root / "NewerVersion";
		CopyPackage(ReleaseA, NewerVersion);
		auto NewerManifest = Json::parse(ReadText(NewerVersion / "game.package.json"));
		NewerManifest["PackageFormatVersion"] = GamePackageFormatVersion + 1;
		WriteText(NewerVersion / "game.package.json", NewerManifest.dump());
		ExpectInvalid(NewerVersion, "newer unsupported package format was accepted");
		const auto NewerCompatibility = Workspace.Root / "NewerCompatibility";
		CopyPackage(ReleaseA, NewerCompatibility);
		auto CompatibilityManifest = Json::parse(ReadText(NewerCompatibility / "game.package.json"));
		CompatibilityManifest["RuntimeCompatibility"] = RuntimeCompatibilityVersion + 1;
		WriteText(NewerCompatibility / "game.package.json", CompatibilityManifest.dump());
		ExpectInvalid(NewerCompatibility, "newer unsupported runtime compatibility was accepted");
		const auto UnknownField = Workspace.Root / "UnknownField";
		CopyPackage(ReleaseA, UnknownField);
		auto UnknownManifest = Json::parse(ReadText(UnknownField / "game.package.json"));
		UnknownManifest["Unknown"] = true;
		WriteText(UnknownField / "game.package.json", UnknownManifest.dump());
		ExpectInvalid(UnknownField, "unknown package manifest field was accepted");
		const auto Malformed = Workspace.Root / "Malformed";
		CopyPackage(ReleaseA, Malformed);
		WriteText(Malformed / "game.package.json", "{not-json");
		ExpectInvalid(Malformed, "malformed package manifest was accepted");
		const auto Duplicate = Workspace.Root / "Duplicate";
		CopyPackage(ReleaseA, Duplicate);
		auto DuplicateManifest = Json::parse(ReadText(Duplicate / "game.package.json"));
		DuplicateManifest["Content"].push_back(DuplicateManifest["Content"].back());
		WriteText(Duplicate / "game.package.json", DuplicateManifest.dump());
		ExpectInvalid(Duplicate, "duplicate package path was accepted");
		const auto Absolute = Workspace.Root / "Absolute";
		CopyPackage(ReleaseA, Absolute);
		auto AbsoluteManifest = Json::parse(ReadText(Absolute / "game.package.json"));
		AbsoluteManifest["Content"][0]["Path"] = "C:/escape";
		WriteText(Absolute / "game.package.json", AbsoluteManifest.dump());
		ExpectInvalid(Absolute, "absolute package path was accepted");
		const auto CaseCollision = Workspace.Root / "CaseCollision";
		CopyPackage(ReleaseA, CaseCollision);
		auto CaseManifest = Json::parse(ReadText(CaseCollision / "game.package.json"));
		auto Collision = CaseManifest["Content"].back();
		auto CollisionPath = Collision["Path"].get<std::string>();
		std::ranges::transform(CollisionPath, CollisionPath.begin(), [](unsigned char Value) {
			return static_cast<char>(std::toupper(Value));
		});
		Collision["Path"] = CollisionPath;
		CaseManifest["Content"].push_back(std::move(Collision));
		WriteText(CaseCollision / "game.package.json", CaseManifest.dump());
		ExpectInvalid(CaseCollision, "case-colliding package path was accepted");
		const auto Oversized = Workspace.Root / "Oversized";
		CopyPackage(ReleaseA, Oversized);
		auto OversizedManifest = Json::parse(ReadText(Oversized / "game.package.json"));
		OversizedManifest["DisplayName"] = std::string(257, 'x');
		WriteText(Oversized / "game.package.json", OversizedManifest.dump());
		ExpectInvalid(Oversized, "oversized package display name was accepted");
		const auto MissingContent = Workspace.Root / "MissingContent";
		CopyPackage(ReleaseA, MissingContent);
		std::filesystem::remove(MissingContent / "shaders/gui.vert.spv");
		ExpectInvalid(MissingContent, "missing required package content was accepted");
		const auto UndeclaredContent = Workspace.Root / "UndeclaredContent";
		CopyPackage(ReleaseA, UndeclaredContent);
		WriteText(UndeclaredContent / "user-file.txt", "must not be deleted by package replacement");
		ExpectInvalid(UndeclaredContent, "undeclared package-directory content was accepted");
		auto RefusedUndeclaredReplacement = Build(Payload, Runtime, UndeclaredContent);
		Require(
			!RefusedUndeclaredReplacement.Ok &&
				ReadText(UndeclaredContent / "user-file.txt") == "must not be deleted by package replacement",
			"package replacement deleted undeclared user content"
		);
		const auto ModifiedAsset = Workspace.Root / "ModifiedAsset";
		CopyPackage(ReleaseA, ModifiedAsset);
		const auto FirstContentId = RuntimeCatalog["Assets"][0]["ContentId"].get<std::string>();
		WriteText(ModifiedAsset / ("content/assets/artifacts/" + FirstContentId + ".gasset"), "modified");
		ExpectInvalid(ModifiedAsset, "modified canonical asset artifact was accepted");

#if defined(_WIN32)
		auto LongParent = Workspace.Root;
		while ((LongParent / "Package" / "content/assets/artifacts" /
				"0000000000000000000000000000000000000000000000000000000000000000.gasset")
				   .native()
				   .size() <= 245)
			LongParent /= "bounded-path-segment";
		std::filesystem::create_directories(LongParent);
		auto LongPathResult = Build(Payload, Runtime, LongParent / "Package");
		Require(
			!LongPathResult.Ok && HasDiagnostic(LongPathResult.Diagnostics, "BuildFailed"),
			"unsupported Windows staging path length did not fail before publication"
		);
#endif

		auto RenamedPayload = Payload;
		RenamedPayload.DisplayName = "Renamed Complete Game";
		auto Renamed = Build(RenamedPayload, Runtime, Workspace.Root / "RenamedPackage");
		Require(
			Renamed.Ok && Renamed.Identity == Payload.Identity,
			"display-name change incorrectly changed durable ProjectId"
		);
		const auto OtherIdentity = ProjectId::New();
		Require(
			OtherIdentity != Payload.Identity &&
				GetPackageUserDataRoot(OtherIdentity) != GetPackageUserDataRoot(Payload.Identity),
			"standalone user-data root is not isolated by ProjectId"
		);

		World->Destroy();
		std::cout << "[Package:Test] manifestEntries=" << Manifest["Content"].size()
				  << " totalBytes=" << First.Size.TotalBytes << " assetBytes=" << First.Size.AssetBytes
				  << " buildMs=" << BuildMilliseconds << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:Test] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/FileLink.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "PackageSourcePaths.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <span>
#include <sstream>
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
			Root = std::filesystem::temp_directory_path() / ("gpk-" + Suffix);
			if (!std::filesystem::create_directory(Root))
				throw std::runtime_error("Packaging test workspace already exists");
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

	void CheckPrivatePathScanner(const std::filesystem::path &Fixture) {
		using namespace gargantuan::test;
		const std::array<std::string, 2> Roots{"/src", "/authoring/project with spaces"};
		std::istringstream Relative("vendor/src/file ./src/part ../src/x /src-extra /src2 /other/src/part");
		Require(!FindPackagePath(Relative, Roots), "relative source paths or similar directory names matched /src");
		for (const auto Offset : {std::size_t(0), PackageScanChunkBytes-4, PackageScanChunkBytes-1, PackageScanChunkBytes}) {
			std::istringstream Input(std::string(Offset, '\0') + "/src");
			const auto Match = FindPackagePath(Input, Roots);
			Require(Match && Match->RootIndex == 0 && Match->Offset == Offset, "missed exact root or chunk-edge path");
		}
		std::istringstream Embedded(std::string(PackageScanChunkBytes-1, 'v') + "/src/relative.cpp");
		Require(!FindPackagePath(Embedded, Roots), "chunk overlap lost the preceding relative path component");
		std::istringstream Longer(std::string(PackageScanChunkBytes-4, '\0') + "/srcExtra/File");
		Require(!FindPackagePath(Longer, Roots), "chunk overlap lost the following path component");
		constexpr std::size_t LateOffset = 16 * 1024 * 1024 + PackageScanChunkBytes - 3;
		{
			std::ofstream Output(Fixture, std::ios::binary);
			Output.seekp(LateOffset);
			Output << Roots[1] << "/entry.cpp";
			Require(Output.good(), "could not write the large package path scanner fixture");
		}
		std::ifstream Late(Fixture, std::ios::binary);
		const auto Match = FindPackagePath(Late, Roots);
		Require(Match && Match->RootIndex == 1 && Match->Offset == LateOffset,
			"missed an absolute authoring path past 16 MiB spanning two scan chunks");
		const std::array<std::string, 1> Drive{"c:/project/authorroot"};
		for (const auto *Spelling : {R"(binary:C:\PROJECT\AuthorRoot\part.cpp)",
			R"(binary:C:\project/AuthorRoot\part.cpp)", "binary:C:/Project/AuthorRoot/part.cpp"}) {
			std::istringstream Native(Spelling);
			const auto WindowsMatch = FindPackagePath(Native, Drive, PackagePathStyle::Windows);
			Require(WindowsMatch && WindowsMatch->Offset == 7, "missed a native or mixed-separator Windows authoring path");
		}
		std::istringstream Siblings(R"(C:\project/AuthorRoot-extra\file C:/PROJECT/AuthorRoot2/x vendor\src\file .\project\authorroot\file)");
		Require(!FindPackagePath(Siblings, Drive, PackagePathStyle::Windows), "matched a sibling or relative Windows path");
	}

	void RequirePublicPackagePaths(const std::filesystem::path &File, std::span<const std::string> Roots) {
		std::ifstream Input(File, std::ios::binary);
		if (!Input) throw std::runtime_error("Could not open packaged file " + File.string());
		if (const auto Hit = test::FindPackagePath(Input, Roots))
			throw std::runtime_error("Private path " + Roots[Hit->RootIndex] + " in " + File.string() +
				" at byte " + std::to_string(Hit->Offset));
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

	template <typename Operation> void RequireRejected(Operation &&Run, std::string_view Message) {
		try {
			Run();
		} catch (const std::exception &) {
			return;
		}
		throw std::runtime_error(std::string(Message));
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

	void TestTerrainClientBootstrap(Project &ProjectValue, const std::filesystem::path &Runtime,
		const std::filesystem::path &Root) {
		const auto World = ProjectValue.DeserializeGame();
		const auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		Require(Space && Space->GetTerrain(), "terrain fixture requires the canonical Workspace shell");
		auto Field = std::make_unique<terrain::Field>();
		const std::array Writes{
			terrain::SampleWrite{{0, 3, 4}, {-123, 7}},
			terrain::SampleWrite{{128, 4, 5}, {-342, 9}},
		};
		const std::array Keys{terrain::GetChunkKey(Writes[0].Point), terrain::GetChunkKey(Writes[1].Point)};
		auto Prepared = terrain::Field::Prepare(Field->Capture(Keys).value(), Writes).value();
		Require(Field->Commit(std::move(Prepared)).has_value(), "prepare durable near and distant terrain leaves");
		Space->RestoreTerrainField(std::move(Field));
		World->InitializeLoadedProjectRevision();
		const auto Revision = World->GetAuthoritativeRevision();
		const auto Payload = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		const auto Managed = PackageBuilder::Capture(ProjectValue, World, Revision, Revision, PackageAudience::Trusted,
			RuntimeDistributionRequest{.Preset = RuntimeDistributionPreset::Compiled});
		for (const auto Target : {RuntimeDistributionTarget::DistributedPlayer, RuntimeDistributionTarget::DedicatedServer}) {
			const auto Audience = Target == RuntimeDistributionTarget::DistributedPlayer
				? PackageAudience::DistributedPlayer : PackageAudience::Trusted;
			bool UnsupportedTarget = false;
			try {
				(void)PackageBuilder::Capture(ProjectValue, World, Revision, Revision, Audience,
					RuntimeDistributionRequest{.Target = Target});
			} catch (const RuntimeDistributionError &Error) {
				UnsupportedTarget = Error.Code == "UnsupportedTerrainTarget";
			}
			Require(UnsupportedTarget, "managed network terrain must fail before projecting or packaging project content");
		}
		World->Destroy();
		Require(Payload.TerrainArtifacts.size() == 3, "capture exactly two terrain leaves and one manifest");
		const auto Development = Root / "TerrainDevelopment";
		const auto Release = Root / "TerrainRelease";
		const auto BuildTerrainPackage = [&](const std::filesystem::path &Destination, PackageConfiguration Configuration) {
			const auto Result = Build(Payload, Runtime, Destination, Configuration);
			for (const auto &Diagnostic : Result.Diagnostics)
				if (Diagnostic.Severity == PackageDiagnosticSeverity::Error)
					throw std::runtime_error("Terrain fixture package build failed with " + Diagnostic.Code + ": " +
						Diagnostic.Message + " " + Diagnostic.Item);
			Require(Result.Ok, "build terrain fixture package");
		};
		BuildTerrainPackage(Development, PackageConfiguration::Development);
		BuildTerrainPackage(Release, PackageConfiguration::Release);

		std::vector<PackageDiagnostic> Diagnostics;
		const auto FullPackage = PackageBuilder::Load(Development, Diagnostics);
		Require(FullPackage && FullPackage->TerrainArtifacts.size() == 3, "complete package load retains every terrain leaf");
		const auto CompleteWorld = PackageBuilder::LoadWorld(*FullPackage, Development);
		const auto CompleteSpace = std::dynamic_pointer_cast<Workspace>(CompleteWorld->GetService("Workspace"));
		Require(CompleteSpace && CompleteSpace->GetTerrain()->GetField().Read(Writes[1].Point) == Writes[1].Value,
			"complete World loader restores the distant terrain value");
		CompleteWorld->Destroy();
		const auto Relative = [&](const terrain::TerrainArtifact &Artifact) {
			return std::filesystem::path("content") / terrain::GetTerrainArtifactPath(Artifact.Id).value();
		};
		const auto Leaf = Relative(Payload.TerrainArtifacts.front());
		const auto Manifest = Relative(Payload.TerrainArtifacts.back());
		const auto ManagedOutput = Root / "TerrainManagedOffline";
		const auto ManagedBuild = Build(Managed, Runtime, ManagedOutput, PackageConfiguration::Development);
		Require(ManagedBuild.Ok && !ManagedBuild.ManifestSha256.empty(), "build managed compiled Offline Player with terrain");
		const RuntimeDistributionAdmission Pinned{.ExpectedManifestSha256 = ManagedBuild.ManifestSha256};
		const auto ManagedPackage = PackageBuilder::Load(ManagedOutput, Diagnostics, Pinned,
			PackageRuntimeLoadMode::LoopbackDevelopmentClient);
		Require(ManagedPackage && ManagedPackage->RuntimeCode && ManagedPackage->TerrainArtifacts.size() == 3 &&
			ManagedPackage->LoadMode == PackageRuntimeLoadMode::Complete, "managed Offline Player must always load complete terrain");
		const auto ManagedWorld = PackageBuilder::LoadWorld(*ManagedPackage, ManagedOutput);
		const auto ManagedSpace = std::dynamic_pointer_cast<Workspace>(ManagedWorld->GetService("Workspace"));
		Require(ManagedSpace && ManagedSpace->GetTerrain()->GetField().Read(Writes[1].Point) == Writes[1].Value,
			"managed catalog binding preserves the same distant terrain value");
		ManagedWorld->Destroy();
		Require(std::filesystem::remove(ManagedOutput / Leaf) && !PackageBuilder::Load(ManagedOutput, Diagnostics, Pinned,
			PackageRuntimeLoadMode::LoopbackDevelopmentClient), "managed Offline Player rejects missing terrain leaves");
		for (const auto &Artifact : Payload.TerrainArtifacts)
			if (Artifact.Id.Purpose == terrain::TerrainArtifactPurpose::Chunk)
				Require(std::filesystem::remove(Development / Relative(Artifact)), "remove local dormant GTCH fixture");
		ExpectInvalid(Development, "full audit accepted a missing terrain leaf");
		Require(!PackageBuilder::Load(Development, Diagnostics), "offline startup accepted a missing terrain leaf");

		using enum PackageRuntimeLoadMode;
		const auto Client = PackageBuilder::Load(Development, Diagnostics, {}, LoopbackDevelopmentClient);
		Require(Client && Client->LoadMode == LoopbackDevelopmentClient && Client->TerrainArtifacts.size() == 1 &&
			Client->TerrainArtifacts.front().Id == Payload.TerrainArtifacts.back().Id &&
			Client->PreRunSource == FullPackage->PreRunSource && Client->Assets.CatalogJson == FullPackage->Assets.CatalogJson,
			"loopback startup loads only the canonical manifest and normal trusted startup data");
		const auto CodeWorld = PackageBuilder::LoadClientCodeWorld(*Client, Development);
		const auto CodeSpace = std::dynamic_pointer_cast<Workspace>(CodeWorld->GetService("Workspace"));
		Require(CodeSpace && CodeSpace->GetTerrain()->GetField().GetStats().MaterializedPages == 0,
			"client code hydration leaves the temporary terrain field empty");
		CodeWorld->Destroy();
		bool Denied = false;
		try { (void)PackageBuilder::LoadWorld(*Client, Development); }
		catch (const std::invalid_argument &) { Denied = true; }
		Require(Denied, "manifest-only payload cannot enter the complete gameplay loader");

		const auto ReleaseClient = PackageBuilder::Load(Release, Diagnostics, {}, LoopbackDevelopmentClient);
		Require(ReleaseClient && ReleaseClient->LoadMode == PackageRuntimeLoadMode::Complete &&
			ReleaseClient->TerrainArtifacts.size() == 3, "Release packages always perform complete admission");
		Require(std::filesystem::remove(Release / Leaf) &&
			!PackageBuilder::Load(Release, Diagnostics, {}, LoopbackDevelopmentClient),
			"Release packages cannot opt into missing terrain leaves");

		auto Metadata = Json::parse(ReadText(Development / "game.package.json"));
		const auto OriginalMetadata = Metadata.dump();
		for (auto &Entry : Metadata["Content"])
			if (Entry["Path"] == Leaf.generic_string()) Entry["Size"] = Entry["Size"].get<std::uint64_t>() + 1;
		const auto Content = Metadata["Content"].dump();
		Metadata["ContentTableSha256"] = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Content.data()), Content.size())).ToString();
		WriteText(Development / "game.package.json", Metadata.dump());
		Require(!PackageBuilder::Load(Development, Diagnostics, {}, LoopbackDevelopmentClient) &&
			HasDiagnostic(Diagnostics, "InvalidTerrainClosure"), "scoped admission rejects leaf metadata inconsistent with GTMF");
		WriteText(Development / "game.package.json", OriginalMetadata);
		auto ManifestBytes = ReadText(Development / Manifest);
		ManifestBytes.front() ^= 1;
		WriteText(Development / Manifest, ManifestBytes);
		Require(!PackageBuilder::Load(Development, Diagnostics, {}, LoopbackDevelopmentClient),
			"scoped admission still rejects corrupt manifest bytes");
		std::cout << "[Package:Terrain] Client verified its manifest and code without local GTCH leaves\n";
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
					Relative.find("nodecontent") == std::string::npos &&
					Relative.find("private-node-content") == std::string::npos &&
					Relative.find("entitlements.grpc.pb") == std::string::npos &&
					Relative.find("content.grpc.pb") == std::string::npos && !Relative.ends_with(".proto"),
				"generic package included a private backend adapter, schema, or configuration artifact"
			);
		}
	}

	void CheckDistributedPlayerPackage(const TestWorkspace &Paths, const std::filesystem::path &Runtime,
		DiskFilesystem &Filesystem, const Project &ProjectValue, const std::shared_ptr<DataModel> &World) {
		const auto Audience = PackageAudience::DistributedPlayer;
		const auto Policy = Paths.ProjectRoot / ".gargantuan/package-audience.json";
		const auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		const auto Records = Assets->GetCatalog(false);
		const auto SharedAsset = std::ranges::find_if(Records, [](const auto &Record) {
			return Record.Dependencies.empty();
		});
		Require(SharedAsset != Records.end(), "audience fixture has no dependency-free shared asset");
		const auto PrivateAsset = std::ranges::find_if(Records, [&](const auto &Record) {
			return Record.ContentId != SharedAsset->ContentId;
		});
		Require(PrivateAsset != Records.end(), "audience fixture has no distinct private asset");

		const auto MakeFolder = [](const auto &Parent) {
			auto FolderValue = std::make_shared<Folder>();
			FolderValue->SetName("Audience.Fixture");
			FolderValue->SetArchivable(true);
			FolderValue->SetParent(Parent);
			return FolderValue;
		};
		const auto MakeScript = [](const auto &Parent, std::string Name, Enums::RunContext Role,
			std::string Source) {
			auto ScriptValue = std::make_shared<Script>();
			ScriptValue->SetName(std::move(Name));
			ScriptValue->SetRunContext(Role);
			ScriptValue->SetSource(std::move(Source));
			ScriptValue->SetArchivable(true);
			ScriptValue->SetParent(Parent);
			return ScriptValue;
		};
		const auto FolderValue = MakeFolder(World);
		(void)MakeScript(FolderValue, "ClientFixture", Enums::RunContext::Client, "-- shared-client-marker");
		(void)MakeScript(FolderValue, "ServerFixture", Enums::RunContext::Server, "-- server-fixture-marker");
		auto Module = std::make_shared<ModuleScript>();
		Module->SetName("SharedFixture");
		Module->SetSource("return 'shared-module-marker'");
		Module->SetArchivable(true);
		auto SharedStorage = World->GetService("ReplicatedStorage");
		SharedStorage->SetArchivable(true);
		Module->SetParent(SharedStorage);
		auto PrivateModule = std::make_shared<ModuleScript>();
		PrivateModule->SetName("ServerModuleFixture");
		PrivateModule->SetSource("return 'server-module-marker'");
		PrivateModule->SetArchivable(true);
		PrivateModule->SetParent(FolderValue);
		const auto MountedDirectory = Paths.ProjectRoot / "AudienceMountFixture";
		std::filesystem::create_directory(MountedDirectory);
		WriteText(MountedDirectory / "MountFixture.client.luau", "-- mounted-source-marker");
		auto Link = std::make_shared<FileLink>();
		Link->SetName("PrivateSourceMount");
		Link->SetPath("AudienceMountFixture");
		Link->SetArchivable(true);
		Link->SetParent(FolderValue);
		SourceMount Mount(Filesystem);
		const auto Mounted = Link->Synchronize(Mount);
		Require(Mounted && *Mounted == 1, "audience fixture could not mount its ordinary client source");

		const auto Revision = World->GetAuthoritativeRevision();
		const auto Capture = [&] { return PackageBuilder::Capture(ProjectValue, World, Revision, Revision, Audience); };
		RequireRejected(Capture, "distributed capture accepted a missing asset audience policy");
		const auto WritePolicy = [&](AssetId Chosen) {
			WriteText(Policy, Json({{"Version", 1}, {"SharedAssets", Json::array({Chosen.ToString()})}}).dump());
		};
		const auto DependentAsset = std::ranges::find_if(Records, [](const auto &Record) {
			return !Record.Dependencies.empty();
		});
		Require(DependentAsset != Records.end(), "audience fixture has no dependency-bearing asset");
		WritePolicy(DependentAsset->Id);
		RequireRejected(Capture, "a shared asset silently pulled in an unlisted dependency");
		WritePolicy(SharedAsset->Id);
		const auto Payload = Capture();
		const auto Output = Paths.Root / "DistributedPlayer";
		if (!std::filesystem::exists(Runtime / "distributed-player-capability.json")) {
			Require(!Build(Payload, Runtime, Output).Ok && !std::filesystem::exists(Output),
				"a runtime without the distributed Player capability published an artifact");
			return;
		}

		Require(Build(Payload, Runtime, Output).Ok, "distributed Player package failed to build");
		Require(!HasError(PackageBuilder::Validate(Output, Audience)), "distributed Player validation failed");
		Require(HasDiagnostic(PackageBuilder::Validate(Output, PackageAudience::Trusted), "AudienceMismatch"),
			"trusted validation accepted a distributed Player artifact");
		const auto ManifestText = ReadText(Output / "game.package.json");
		const auto Manifest = Json::parse(ManifestText);
		const auto Code = ReadText(Output / "content/client-code.json");
		Require(Manifest["Audience"] == "DistributedPlayer" && Manifest["PackageFormatVersion"] == 3 &&
			Code.contains("shared-client-marker") && Code.contains("shared-module-marker") &&
			!Code.contains("server-fixture-marker") && !Code.contains("server-module-marker") &&
			!Code.contains("AudienceMountFixture") && !Code.contains("mounted-source-marker") &&
			!std::filesystem::exists(Output / "content/game.instance.json") &&
			!std::filesystem::exists(Output / "content/prerun.luau") &&
			!std::filesystem::exists(Output / "content/regions") &&
			!std::filesystem::exists(Output / ".gargantuan"),
			"distributed Player project bytes did not match their audience");
		const auto Catalog = Json::parse(ReadText(Output / "content/assets/catalog.json"));
		Require(Catalog["Assets"].size() == 1 && Catalog["Assets"][0]["AssetId"] == SharedAsset->Id.ToString() &&
			std::filesystem::exists(Output / ("content/assets/artifacts/" + SharedAsset->ContentId.ToString() + ".gasset")) &&
			!std::filesystem::exists(Output / ("content/assets/artifacts/" + PrivateAsset->ContentId.ToString() + ".gasset")),
			"distributed Player artifact closure did not match the explicit shared asset");

		std::vector<PackageDiagnostic> Diagnostics;
		const auto Loaded = PackageBuilder::Load(Output, Diagnostics);
		Require(Loaded && Loaded->Assets.Artifacts.size() == 1 && Loaded->ProjectJson.empty() && !Loaded->PreRunSource,
			"distributed Player load did not retain only its client payload");
		auto Replica = std::make_shared<DataModel>();
		const auto ReplicaFolder = MakeFolder(Replica);
		const auto ClientTarget = MakeScript(ReplicaFolder, "ClientFixture", Enums::RunContext::Client, "");
		auto ReplicaModule = std::make_shared<ModuleScript>();
		ReplicaModule->SetName("SharedFixture");
		ReplicaModule->SetSource("return 'shared-module-marker'");
		ReplicaModule->SetParent(Replica->GetService("ReplicatedStorage"));
		Require(PackageBuilder::HydrateDistributedClientCode(*Loaded, Replica) >= 2 &&
			ClientTarget->GetSource() == "-- shared-client-marker" &&
			ReplicaModule->GetSource() == "return 'shared-module-marker'", "legitimate client and shared code did not hydrate");
		Replica->Destroy();

		const auto Trusted = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		Require(!Build(Trusted, Runtime, Output).Ok && ReadText(Output / "game.package.json") == ManifestText,
			"a trusted build replaced the distributed destination");
		const auto TrustedOutput = Paths.Root / "TrustedAudience";
		Require(Build(Trusted, Runtime, TrustedOutput).Ok &&
			HasDiagnostic(PackageBuilder::Validate(TrustedOutput, Audience), "AudienceMismatch") &&
			std::filesystem::exists(TrustedOutput / "content/game.instance.json"),
			"trusted offline package behavior changed or passed the distribution gate");
		const auto ConcurrentOutput = Paths.Root / "ConcurrentAudience";
		bool PublishedDuringStaging = false;
		const auto Superseded = PackageBuilder::Build({
			.Payload = Trusted,
			.RuntimeDistributionRoot = Runtime,
			.OutputDirectory = ConcurrentOutput,
			.Progress = [&](PackagePhase Phase) {
				if (Phase == PackagePhase::Finalize)
					PublishedDuringStaging = Build(Payload, Runtime, ConcurrentOutput).Ok;
			},
		});
		Require(PublishedDuringStaging && !Superseded.Ok &&
			!HasError(PackageBuilder::Validate(ConcurrentOutput, Audience)),
			"a staged trusted build replaced a distributed artifact published before finalization");

		const auto OlderRuntime = Paths.Root / "OlderRuntime";
		CopyPackage(Runtime, OlderRuntime);
		std::filesystem::remove(OlderRuntime / "distributed-player-capability.json");
		const auto OldRuntimeOutput = Paths.Root / "OldRuntimeOutput";
		Require(!Build(Payload, OlderRuntime, OldRuntimeOutput).Ok && !std::filesystem::exists(OldRuntimeOutput),
			"a Player runtime without the package format capability published a distributed artifact");
		WriteText(OlderRuntime / "distributed-player-capability.json", Json({
			{"PackageFormatVersion", DistributedPlayerPackageFormatVersion}, {"PlayerSha256", std::string(64, 'a')},
		}).dump());
		const auto WrongDigest = Build(Payload, OlderRuntime, OldRuntimeOutput);
		Require(!WrongDigest.Ok && !std::filesystem::exists(OldRuntimeOutput) &&
			std::ranges::any_of(WrongDigest.Diagnostics, [](const PackageDiagnostic &Diagnostic) {
				return Diagnostic.Item.ends_with("Distributed Player capability does not match the staged executable");
			}), "a capability for different executable bytes passed the staging check");

		const auto PreRun = Paths.ProjectRoot / ".gargantuan/prerun.luau";
		Require(!std::filesystem::exists(PreRun), "audience fixture unexpectedly has PreRun");
		WriteText(PreRun, "-- trusted-schema-fixture");
		RequireRejected(Capture, "distributed capture accepted unsplit project schema source");
	}

	std::string PackageTextHash(std::string_view Bytes) {
		return AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Bytes.data()), Bytes.size())).ToString();
	}

	void CheckManagedRuntimeDistribution(const std::filesystem::path &StagedRuntime) {
		TestWorkspace Paths;
		auto Runtime = StagedRuntime;
		if (!std::filesystem::exists(Runtime / "runtime-distribution-capability.json")) {
			Runtime = Paths.Root / "ManagedRuntimeFixture";
			CopyPackage(StagedRuntime, Runtime);
			const auto Distribution = Json::parse(ReadText(Runtime / "runtime-distribution.json"));
			const auto Executable = Runtime / Distribution["Player"].get<std::string>();
			WriteText(Runtime / "runtime-distribution-capability.json", Json({
				{"Version", 1}, {"PackageFormatVersion", RuntimeDistributionPackageFormatVersion},
				{"ExecutableSha256", PackageTextHash(ReadText(Executable))},
			}).dump());
		}
		DiskFilesystem Filesystem(Paths.ProjectRoot);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto World = ProjectValue.DeserializeGame();
		World->InitializeLoadedProjectRevision();
		const auto SharedStorage = World->GetService("ReplicatedStorage");
		SharedStorage->SetArchivable(true);
		const auto AddModule = [&](std::string Name, std::string Source) {
			auto Module = std::make_shared<ModuleScript>();
			Module->SetName(std::move(Name));
			Module->SetSource(std::move(Source));
			Module->SetArchivable(true);
			Module->SetParent(SharedStorage);
			return Module->GetSourceId();
		};
		const auto OpenId = AddModule("ManagedCraftFixture", "return 'RDP_OPEN_CRAFT_MARKER'");
		const auto CompiledId = AddModule("ManagedMovementFixture", "-- RDP_COMPILED_SOURCE_MARKER\nreturn 'RDP_COMPILED_CONSTANT'");
		const auto ProtectedId = AddModule("ManagedGenerationFixture", "-- RDP_PROTECTED_SOURCE_MARKER\nreturn 'RDP_PROTECTED_CONSTANT'");
		auto ServerScript = std::make_shared<Script>();
		ServerScript->SetName("ManagedServerFixture");
		ServerScript->SetRunContext(Enums::RunContext::Server);
		ServerScript->SetSource("-- RDP_PRIVATE_SERVER_SOURCE_MARKER\nlocal PrivateInventory = 'RDP_PRIVATE_SERVER_CONSTANT'");
		ServerScript->SetArchivable(true);
		ServerScript->SetParent(World);
		const auto ServerId = ServerScript->GetSourceId();
		const auto Persisted = World->GetAuthoritativeRevision();
		ProjectValue.PersistGameAtomically(ProjectValue.CaptureGame(World, Persisted));
		World->Destroy();
		World = ProjectValue.DeserializeGame();
		World->InitializeLoadedProjectRevision();
		const auto Revision = World->GetAuthoritativeRevision();
		const auto PolicyFile = Paths.ProjectRoot / ".gargantuan/runtime-distribution.json";
		WriteText(PolicyFile, R"({"Version":1})");

		const auto Capture = [&](RuntimeDistributionRequest Request) {
			const auto Audience = Request.Target == RuntimeDistributionTarget::DistributedPlayer ?
				PackageAudience::DistributedPlayer : PackageAudience::Trusted;
			return PackageBuilder::Capture(ProjectValue, World, Revision, Revision, Audience, Request);
		};
		const auto BuildManaged = [&](const GamePayload &Payload, const std::filesystem::path &Destination,
			std::optional<RuntimePackageProvisionedSecurity> Security = std::nullopt) {
			return PackageBuilder::Build({.Payload = Payload, .RuntimeDistributionRoot = Runtime,
				.OutputDirectory = Destination, .DistributionSecurity = std::move(Security)});
		};
		const auto CatalogPath = "content/runtime-scripts.json";
		const auto ProjectPath = "content/game.instance.json";
		const auto ManifestPath = "game.package.json";
		const auto SignaturePath = "game.package.sig";
		const auto CompiledPath = "content/scripts/" + CompiledId + ".luac";
		const auto ProtectedPath = "content/scripts/" + ProtectedId + ".gse";
		std::vector<PackageDiagnostic> Diagnostics;

		const auto OpenOutput = Paths.Root / "ManagedOpen";
		const auto Open = BuildManaged(Capture({.Preset = RuntimeDistributionPreset::Open}), OpenOutput);
		Require(Open.Ok && Json::parse(ReadText(OpenOutput / ManifestPath))["PackageFormatVersion"] == RuntimeDistributionPackageFormatVersion,
			"Open distribution publication");
		Require(ReadText(OpenOutput / ProjectPath).contains("RDP_OPEN_CRAFT_MARKER") &&
			ReadText(OpenOutput / ProjectPath).contains("RDP_COMPILED_SOURCE_MARKER") &&
			!std::filesystem::exists(OpenOutput / CompiledPath), "Open distribution retains authored source");
		Require(PackageBuilder::Load(OpenOutput, Diagnostics).has_value() && !HasError(Diagnostics), "unsigned Open admits source");
		Require(HasDiagnostic(PackageBuilder::Validate(OpenOutput, std::nullopt, {.RequirePublisher = true}), "PublisherRequired"),
			"signed deployment rejects unsigned Open");

		const auto CompiledOutput = Paths.Root / "ManagedCompiled";
		const auto Compiled = BuildManaged(Capture({.Preset = RuntimeDistributionPreset::Compiled}), CompiledOutput);
		Require(Compiled.Ok && !Compiled.ManifestSha256.empty(), "compiled distribution publication");
		const auto CompiledUntrusted = PackageBuilder::Load(CompiledOutput, Diagnostics);
		Require(!CompiledUntrusted && HasDiagnostic(Diagnostics, "UntrustedBytecode"), "unsigned Compiled requires native provenance");
		Require(HasDiagnostic(PackageBuilder::Validate(CompiledOutput), "UntrustedBytecode"),
			"native admission validation reports unsigned Compiled provenance failure");
		const auto Pinned = RuntimeDistributionAdmission{.ExpectedManifestSha256 = Compiled.ManifestSha256};
		Require(!HasError(PackageBuilder::Validate(CompiledOutput, std::nullopt, Pinned)), "exact out-of-band manifest digest validates");
		Require(HasDiagnostic(PackageBuilder::Validate(CompiledOutput, std::nullopt,
			{.ExpectedManifestSha256 = std::string(64, 'a')}), "ManifestDigestMismatch"), "foreign manifest digest rejected");
		const auto CompiledLoaded = PackageBuilder::Load(CompiledOutput, Diagnostics, Pinned);
		Require(CompiledLoaded && CompiledLoaded->RuntimeCode && !HasError(Diagnostics) &&
			!ReadText(CompiledOutput / ProjectPath).contains("RDP_COMPILED_SOURCE_MARKER") &&
			ReadText(CompiledOutput / CompiledPath).contains("RDP_COMPILED_CONSTANT") &&
			!ReadText(CompiledOutput / CompiledPath).contains("RDP_COMPILED_SOURCE_MARKER"),
			"compiled representation has executable bytes without original source");
		const auto Rebuilt = BuildManaged(Capture({.Preset = RuntimeDistributionPreset::Compiled}), Paths.Root / "ManagedCompiledAgain");
		Require(Rebuilt.Ok && Rebuilt.ManifestSha256 == Compiled.ManifestSha256, "unsigned compiled builds are deterministic");

		const auto SessionIdentity = ProjectId::New().ToString();
		const auto WriteHybridPolicy = [&] {
			WriteText(PolicyFile, Json({{"Version", 1}, {"SessionIdentity", SessionIdentity}, {"Overrides", {
				{OpenId, {{"Representation", "Source"}}},
				{CompiledId, {{"Representation", "Compiled"}}},
				{ProtectedId, {{"Representation", "Protected"}}},
			}}}).dump());
		};
		WriteHybridPolicy();
		RuntimeDistributionRequest HybridRequest{.Preset = RuntimeDistributionPreset::Hybrid, .RequirePublisher = true,
			.SigningKeyId = "TestPublisher", .KeyAccess = RuntimeDistributionKeyAccess::LocalProvisioned, .ContentKeyId = "TestContent"};
		const auto HybridOutput = Paths.Root / "ManagedHybrid";
		const auto HybridPayload = Capture(HybridRequest);
		if (!IsRuntimePackageCryptoAvailable()) {
			const auto Disabled = BuildManaged(HybridPayload, HybridOutput, RuntimePackageProvisionedSecurity{});
			Require(!Disabled.Ok && !std::filesystem::exists(HybridOutput), "crypto-disabled build refuses signed protected publication");
			World->Destroy();
			return;
		}

		constexpr std::array<unsigned char, 32> SeedBytes{
			0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
			0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
		};
		std::array<unsigned char, 32> ContentBytes{};
		for (std::size_t Index = 0; Index < ContentBytes.size(); ++Index) ContentBytes[Index] = static_cast<unsigned char>(Index + 1);
		const auto ProjectIdentity = ProjectValue.Identity.ToString();
		const RuntimePackageKeyScope SigningScope{ProjectIdentity, "", ProjectIdentity, "TestPublisher"};
		const RuntimePackageKeyScope ContentScope{ProjectIdentity, "", ProjectIdentity, "TestContent"};
		RuntimePackagePublicKey PublicKey{};
		Require(GetRuntimePackagePublicKey(RuntimePackageSecret(SeedBytes), PublicKey) == RuntimePackageCryptoStatus::Ok,
			"derive test publisher verification key");
		const RuntimePackageProvisionedSecurity Security{
			.Status = RuntimePackageCryptoStatus::Ok,
			.Signer = MakeProvisionedRuntimePackageSigner(SigningScope, RuntimePackageSecret(SeedBytes)),
			.Trust = MakeProvisionedRuntimePackageTrustProvider(SigningScope, PublicKey),
			.Keys = MakeProvisionedRuntimePackageKeyProvider(ContentScope, RuntimePackageSecret(ContentBytes)),
		};
		auto RuntimeSecurity = Security;
		RuntimeSecurity.Signer.reset();
		const RuntimeDistributionAdmission SignedAdmission{.Security = RuntimeSecurity, .RequirePublisher = true};
		const auto Hybrid = BuildManaged(HybridPayload, HybridOutput, Security);
		Require(Hybrid.Ok && !HasError(PackageBuilder::Validate(HybridOutput, std::nullopt, SignedAdmission)), "signed Hybrid publication validates");
		const auto Snapshot = ReadText(HybridOutput / ProjectPath);
		const auto Ciphertext = ReadText(HybridOutput / ProtectedPath);
		Require(Snapshot.contains("RDP_OPEN_CRAFT_MARKER") && !Snapshot.contains("RDP_COMPILED_SOURCE_MARKER") &&
			!Snapshot.contains("RDP_PROTECTED_SOURCE_MARKER") && !Snapshot.contains("RDP_PROTECTED_CONSTANT") &&
			Ciphertext.starts_with("GRS1") && !Ciphertext.contains("RDP_PROTECTED_CONSTANT"), "Hybrid source and encrypted bytes match policy");
		const auto HybridLoaded = PackageBuilder::Load(HybridOutput, Diagnostics, SignedAdmission);
		Require(HybridLoaded && !HasError(Diagnostics), "signed encrypted Hybrid admits without signing seed");
		auto RuntimeWorld = PackageBuilder::LoadWorld(*HybridLoaded, HybridOutput);
		const auto ReadModule = [&](std::string_view Name) {
			return std::dynamic_pointer_cast<ModuleScript>(RuntimeWorld->FindFirstChild(std::string(Name), true));
		};
		const auto Craft = ReadModule("ManagedCraftFixture");
		const auto Movement = ReadModule("ManagedMovementFixture");
		const auto Generation = ReadModule("ManagedGenerationFixture");
		Require(Craft && Movement && Generation && Craft->GetSource().contains("RDP_OPEN_CRAFT_MARKER") &&
			Movement->GetSource().empty() && Movement->HasRuntimeScriptCode() &&
			Generation->GetSource().empty() && Generation->HasRuntimeScriptCode(), "runtime binds mixed code without materializing protected source");
		RuntimeWorld->Destroy();

		auto Missing = SignedAdmission;
		Missing.Security->Keys.reset();
		Require(!PackageBuilder::Load(HybridOutput, Diagnostics, Missing) && HasDiagnostic(Diagnostics, "KeyUnavailable"),
			"a signed release fails closed without its content key");
		auto Wrong = SignedAdmission;
		Wrong.Security->Keys = MakeProvisionedRuntimePackageKeyProvider(ContentScope, RuntimePackageSecret(SeedBytes));
		Require(!PackageBuilder::Load(HybridOutput, Diagnostics, Wrong) && HasDiagnostic(Diagnostics, "ScriptAuthenticationFailed"),
			"wrong content key cannot hydrate protected code");
		const auto Manifest = ReadText(HybridOutput / ManifestPath);
		const auto Signature = ReadText(HybridOutput / SignaturePath);
		WriteText(HybridOutput / ManifestPath, Manifest + ' ');
		Require(!PackageBuilder::Load(HybridOutput, Diagnostics, SignedAdmission) && HasDiagnostic(Diagnostics, "InvalidSignature"),
			"publisher signs exact manifest bytes");
		WriteText(HybridOutput / ManifestPath, Manifest);
		auto ChangedSignature = Signature;
		ChangedSignature.front() ^= 1;
		WriteText(HybridOutput / SignaturePath, ChangedSignature);
		Require(!PackageBuilder::Load(HybridOutput, Diagnostics, SignedAdmission) && HasDiagnostic(Diagnostics, "InvalidSignature"),
			"detached signature tampering rejected");
		WriteText(HybridOutput / SignaturePath, Signature);
		auto ChangedCiphertext = Ciphertext;
		ChangedCiphertext.back() ^= 1;
		WriteText(HybridOutput / ProtectedPath, ChangedCiphertext);
		Require(!PackageBuilder::Load(HybridOutput, Diagnostics, SignedAdmission) && HasError(Diagnostics),
			"stored encrypted bytes must match trusted manifest digest");
		WriteText(HybridOutput / ProtectedPath, Ciphertext);
		Require(!BuildManaged(HybridPayload, HybridOutput, RuntimeSecurity).Ok && ReadText(HybridOutput / ManifestPath) == Manifest &&
			ReadText(HybridOutput / SignaturePath) == Signature && ReadText(HybridOutput / ProtectedPath) == Ciphertext,
			"failed signed build preserves previous protected output");

		if (std::filesystem::exists(Runtime / "distributed-player-capability.json")) {
			WriteText(Paths.ProjectRoot / ".gargantuan/package-audience.json", R"({"Version":1,"SharedAssets":[]})");
			HybridRequest.Target = RuntimeDistributionTarget::DistributedPlayer;
			const auto PlayerOutput = Paths.Root / "ManagedPlayerHybrid";
			const auto Player = BuildManaged(Capture(HybridRequest), PlayerOutput, Security);
			Require(Player.Ok, "signed Hybrid distributed Player publication");
			const auto ClientCode = ReadText(PlayerOutput / "content/client-code.json");
			const auto Catalog = ReadText(PlayerOutput / CatalogPath);
			Require(ClientCode.contains("RDP_OPEN_CRAFT_MARKER") && !ClientCode.contains("RDP_PRIVATE_SERVER_SOURCE_MARKER") &&
				!ClientCode.contains("RDP_PROTECTED_SOURCE_MARKER") && !Catalog.contains(ServerId) &&
				!std::filesystem::exists(PlayerOutput / ("content/scripts/" + ServerId + ".luac")) &&
				!std::filesystem::exists(PlayerOutput / ProjectPath) &&
				std::filesystem::exists(PlayerOutput / ProtectedPath), "Foundation 10A excludes server code before hybrid representation");
			const auto LoadedPlayer = PackageBuilder::Load(PlayerOutput, Diagnostics, SignedAdmission);
			Require(LoadedPlayer && !HasError(Diagnostics), "distributed Hybrid exact signed bytes admit");
			const auto Replica = std::make_shared<DataModel>();
			const auto SharedReplica = Replica->GetService("ReplicatedStorage");
			const auto ReplicaModule = [&](std::string Name) {
				auto Module = std::make_shared<ModuleScript>();
				Module->SetName(std::move(Name));
				Module->SetParent(SharedReplica);
				return Module;
			};
			const auto ClientCraft = ReplicaModule("ManagedCraftFixture");
			const auto ClientMovement = ReplicaModule("ManagedMovementFixture");
			const auto ClientGeneration = ReplicaModule("ManagedGenerationFixture");
			Require(PackageBuilder::HydrateDistributedClientCode(*LoadedPlayer, Replica) >= 3 &&
				ClientCraft->GetSource().contains("RDP_OPEN_CRAFT_MARKER") &&
				ClientMovement->GetSource().empty() && ClientMovement->HasRuntimeScriptCode() &&
				ClientGeneration->GetSource().empty() && ClientGeneration->HasRuntimeScriptCode(),
				"replica hydrates packaged code without delivering compiled or protected source");
			Replica->Destroy();
		}
		World->Destroy();
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
		CheckPrivatePathScanner(Workspace.Root / "path-scan-fixture.bin");
		const auto Runtime = std::filesystem::absolute(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT).lexically_normal();
		BootstrapProjectRuntimeSchema(Workspace.ProjectRoot);
		DiskFilesystem Filesystem(Workspace.ProjectRoot);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto ReopenedProject = Project::fromExisting(&Filesystem);
		Require(
			ProjectValue.Identity == ReopenedProject.Identity && ProjectValue.Identity.IsValid(),
			"project identity did not remain stable across reopen"
		);
		TestTerrainClientBootstrap(ProjectValue, Runtime, Workspace.Root);
		auto World = ProjectValue.DeserializeGame();
		// The sample's dynamic and skinned subtrees correctly remain bootstrap.
		// Give this partition/determinism fixture an explicitly static unit.
		auto StaticContent = std::make_shared<Part>();
		StaticContent->SetName("PackagingStaticFixture");
		StaticContent->SetArchivable(true);
		StaticContent->SetAnchored(true);
		StaticContent->SetCFrame(CFrame(10000, 0, 0));
		auto ContentRoot = World->FindFirstChildOfClass("Workspace", false);
		Require(ContentRoot != nullptr, "packaging fixture has no Workspace");
		StaticContent->SetParent(ContentRoot);
		World->InitializeLoadedProjectRevision();
		World->Root = Workspace.ProjectRoot;
		World->Filesystem = &Filesystem;
		const auto InitialRevision = World->GetAuthoritativeRevision();
		auto Payload = PackageBuilder::Capture(ProjectValue, World, InitialRevision, InitialRevision);
		Require(
			Payload.Identity == ProjectValue.Identity && !Payload.UnsavedChanges,
			"package snapshot identity/revision state is incorrect"
		);
		Require(!Payload.ContentUnits.empty(), "package capture did not partition any independently available content");
		auto RepeatedPayload = PackageBuilder::Capture(ProjectValue, World, InitialRevision, InitialRevision);
		Require(
			RepeatedPayload.ProjectJson == Payload.ProjectJson &&
			RepeatedPayload.ContentUnits.size() == Payload.ContentUnits.size(),
			"equivalent package capture did not produce a deterministic partition"
		);
		for (std::size_t Index = 0; Index < Payload.ContentUnits.size(); ++Index)
			Require(
				RepeatedPayload.ContentUnits[Index].Entry.Key == Payload.ContentUnits[Index].Entry.Key &&
				RepeatedPayload.ContentUnits[Index].Entry.Digest == Payload.ContentUnits[Index].Entry.Digest &&
				RepeatedPayload.ContentUnits[Index].Payload == Payload.ContentUnits[Index].Payload,
				"equivalent package capture changed a content key, digest, or payload"
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
			ReadText(ReleaseA / "content/content.manifest.json") ==
				ReadText(ReleaseB / "content/content.manifest.json"),
			"equivalent package inputs did not produce a deterministic content manifest"
		);
		Require(
			First.Size.TotalBytes == First.Size.RuntimeBytes + First.Size.ProjectBytes + First.Size.AssetBytes +
										 First.Size.ShaderBytes + First.Size.OtherBytes,
			"package size breakdown does not sum to total bytes"
		);
		Require(!HasError(PackageBuilder::Validate(ReleaseA)), "fresh package did not validate");
		for (const auto Shader : {"opaque-portable.frag.spv", "opaque-instanced.vert.spv", "shadow-instanced.vert.spv"}) {
			Require(std::filesystem::is_regular_file(ReleaseA / "shaders" / Shader),
				"foundation renderer shader is absent from the published package");
		}
		RequireNoPrivateBackendArtifacts(ReleaseA);
		std::vector<PackageDiagnostic> LoadDiagnostics;
		auto Loaded = PackageBuilder::Load(ReleaseA, LoadDiagnostics);
		Require(
			Loaded && Loaded->Inspection.Identity == Payload.Identity &&
				Loaded->Inspection.Revision == Payload.AuthoritativeRevision &&
				Loaded->Assets.Artifacts.size() == Payload.Assets.Artifacts.size(),
			"validated package payload did not load with its stable identity/assets"
		);
		const std::array PrivateRoots{test::PackagePathRoot(Workspace.ProjectRoot),
			test::PackagePathRoot(GARGANTUAN_PACKAGE_SOURCE_ROOT), test::PackagePathRoot(GARGANTUAN_PACKAGE_BUILD_ROOT)};
		std::filesystem::remove_all(Workspace.ProjectRoot);
		BootstrapPackagedRuntimeSchema(Loaded->PreRunSource);
		auto PackagedWorld = PackageBuilder::LoadWorld(*Loaded, ReleaseA);
		Require(
			PackagedWorld->Filesystem == nullptr && PackagedWorld->Root == ReleaseA / "content",
			"packaged runtime retained authoring filesystem authority"
		);
		HeadlessRenderer Renderer(Vector2(640, 360));
		auto LocalContent = PackageBuilder::GetLocalContentConfiguration(*Loaded, ReleaseA);
		Require(LocalContent.has_value(), "package did not expose its local content provider configuration");
		Engine RuntimeEngine(PackagedWorld, &Renderer, {}, {.Content = std::move(LocalContent)});
		RuntimeEngine.ProcessService->Alive = true;
		RuntimeEngine.Step();
		auto InitialRuntimePublication = Renderer.TakeLastPublication();
		Require(
			InitialRuntimePublication != nullptr,
			"packaged runtime did not publish its initial assets/scene/GUI frame"
		);
		RuntimeEngine.Step();
		auto AnimationRuntimePublication = Renderer.TakeLastPublication();
		auto PackagedAnimatedBeacon = std::dynamic_pointer_cast<MeshPart>(
			PackagedWorld->FindFirstDescendant("AnimatedBeacon")
		);
		auto PackagedPose = AnimationRuntimePublication && PackagedAnimatedBeacon
			? std::ranges::find_if(AnimationRuntimePublication->AnimationPoseUpdates, [&](const auto &Pose) {
				return Pose.Object == PackagedAnimatedBeacon->GetObjectId();
			})
			: decltype(AnimationRuntimePublication->AnimationPoseUpdates)::const_iterator{};
		Require(AnimationRuntimePublication && PackagedAnimatedBeacon &&
			PackagedPose != AnimationRuntimePublication->AnimationPoseUpdates.end(),
			"packaged runtime did not start and publish the canonical animated beacon");
		Require(
			PackagedPose->Mode == RenderAnimationSkinningMode::GpuPalette &&
				!PackagedPose->PosedMesh.IsValid() && PackagedPose->Palette.Entries &&
				!PackagedPose->Palette.Entries->empty() && AnimationRuntimePublication->MeshVertexUpdates.empty(),
			"headless packaged runtime must publish a semantic palette without CPU-skinned vertex data"
		);
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
		constexpr std::array MicrosoftRuntimeFiles{
			"concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
			"msvcp140_codecvt_ids.dll", "vccorlib140.dll", "vcruntime140.dll", "vcruntime140_1.dll",
			"vcruntime140_threads.dll", "vc_redist.x64.exe",
		};
		Require(std::ranges::any_of(Manifest["Content"], [](const Json &Entry) {
			return Entry["Path"] == "notices/MSVC-Runtime.txt" && Entry["Category"] == "Notice";
		}), "Windows package manifest omitted the centrally serviced VC++ prerequisite notice");
		const auto Prerequisite = ReadText(ReleaseA / "notices/MSVC-Runtime.txt");
		Require(Prerequisite.contains("requires version ") && !Prerequisite.contains("@GARGANTUAN_MSVC_RUNTIME_MINIMUM@"),
			"Windows package prerequisite notice omitted the required toolset version");
		for (const auto *RuntimeFile : MicrosoftRuntimeFiles) {
			const auto Present = std::ranges::any_of(Manifest["Content"], [RuntimeFile](const Json &Entry) {
				auto Name = std::filesystem::path(Entry["Path"].get<std::string>()).filename().string();
				std::ranges::transform(Name, Name.begin(), [](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
				return Name == RuntimeFile;
			});
			Require(!Present, "game package bundled a separately acquired Microsoft runtime component");
		}
#endif
		for (const auto &Entry : Manifest["Content"])
			Require(
				Entry["Path"].get<std::string>().find("telemetry") == std::string::npos,
				"optional telemetry entered the standalone runtime package"
			);
		RequirePublicPackagePaths(ReleaseA / "game.package.json", PrivateRoots);
		for (const auto &Entry : Manifest["Content"])
			RequirePublicPackagePaths(ReleaseA / Entry.at("Path").get<std::string>(), PrivateRoots);
		const std::filesystem::path ServerRuntime = GARGANTUAN_SERVER_RUNTIME_DISTRIBUTION_ROOT;
		const auto ServerManifestFile = ServerRuntime / "runtime-distribution.json";
		RequirePublicPackagePaths(ServerManifestFile, PrivateRoots);
		const auto ServerManifest = Json::parse(ReadText(ServerManifestFile));
		for (const auto &Entry : ServerManifest.at("Files"))
			RequirePublicPackagePaths(ServerRuntime / Entry.at("Path").get<std::string>(), PrivateRoots);

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
		for (const auto Shader : {"opaque-portable.frag.spv", "opaque-instanced.vert.spv", "shadow-instanced.vert.spv"}) {
			const auto RemovedShader = MissingRuntime / "shaders" / Shader;
			std::filesystem::copy_file(Runtime / "shaders/gui.frag.spv", MissingRuntime / "shaders/gui.frag.spv",
				std::filesystem::copy_options::overwrite_existing);
			std::filesystem::remove(RemovedShader);
			const auto Rejected = Build(Payload, MissingRuntime, Workspace.Root / "MissingFoundationOutput");
			Require(!Rejected.Ok && HasDiagnostic(Rejected.Diagnostics, "InvalidDistribution") &&
				!std::filesystem::exists(Workspace.Root / "MissingFoundationOutput"),
				"missing foundation shader was accepted or published a partial package");
			std::filesystem::copy_file(Runtime / "shaders" / Shader, RemovedShader);
		}

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
		TestWorkspace AudienceWorkspace;
		DiskFilesystem AudienceFilesystem(AudienceWorkspace.ProjectRoot);
		auto AudienceProject = Project::fromExisting(&AudienceFilesystem);
		auto AudienceWorld = AudienceProject.DeserializeGame();
		AudienceWorld->InitializeLoadedProjectRevision();
		CheckDistributedPlayerPackage(AudienceWorkspace, Runtime, AudienceFilesystem, AudienceProject, AudienceWorld);
		AudienceWorld->Destroy();
		CheckManagedRuntimeDistribution(Runtime);
		std::cout << "[Package:Test] manifestEntries=" << Manifest["Content"].size()
				  << " totalBytes=" << First.Size.TotalBytes << " assetBytes=" << First.Size.AssetBytes
				  << " buildMs=" << BuildMilliseconds << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:Test] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

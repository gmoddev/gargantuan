#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	Json *FindObject(Json &Node, std::string_view Name) {
		if (!Node.is_object()) return nullptr;
		if (Node.value("Name", "") == Name) return &Node;
		if (!Node.contains("Children") || !Node["Children"].is_array()) return nullptr;
		for (auto &Child : Node["Children"])
			if (auto *Found = FindObject(Child, Name)) return Found;
		return nullptr;
	}

	GamePayload ExpandPayload(const GamePayload &Source, std::size_t InstanceCount, std::size_t ScriptCount) {
		auto Result = Source;
		auto Document = Json::parse(Result.ProjectJson);
		auto *Workspace = FindObject(Document, "Workspace");
		auto *RoundManager = FindObject(Document, "RoundManager");
		if (!Workspace || !RoundManager) throw std::runtime_error("Benchmark fixture is incomplete");
		const auto ScriptTemplate = *RoundManager;
		for (std::size_t Index = 0; Index < InstanceCount; ++Index) {
			Workspace->at("Children")
				.push_back({
					{"Name", "SyntheticFolder" + std::to_string(Index)},
					{"ClassName", "Folder"},
					{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
					{"ClassDefinitionVersion", 1},
					{"Properties", Json::object()},
					{"Attributes", Json::object()},
					{"Extensions", Json::array()},
					{"CustomProperties", Json::array()},
					{"Tags", Json::array()},
					{"Children", Json::array()},
				});
		}
		for (std::size_t Index = 0; Index < ScriptCount; ++Index) {
			auto Script = ScriptTemplate;
			Script["Name"] = "SyntheticScript" + std::to_string(Index);
			Workspace->at("Children").push_back(std::move(Script));
		}
		Result.ProjectJson = Document.dump();
		return Result;
	}

	void RunBuild(
		std::string_view Label,
		const GamePayload &Payload,
		const std::filesystem::path &Runtime,
		const std::filesystem::path &Output
	) {
		const auto Started = std::chrono::steady_clock::now();
		auto Result = PackageBuilder::Build({
			.Payload = Payload,
			.RuntimeDistributionRoot = Runtime,
			.OutputDirectory = Output,
			.Configuration = PackageConfiguration::Release,
		});
		const auto Total =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
		if (!Result.Ok) throw std::runtime_error("Packaging benchmark build failed");
		std::cout << "[Package:Benchmark] case=" << Label << " snapshotBytes=" << Payload.ProjectJson.size()
				  << " totalBytes=" << Result.Size.TotalBytes << " totalMs=" << Total;
		for (const auto &Timing : Result.Timings)
			std::cout << ' ' << GetPackagePhaseName(Timing.Phase) << "Us=" << Timing.Duration.count();
		std::cout << '\n';
	}
}

int main(int argc, char **argv) {
	using namespace gargantuan;
	if (!SDL_Init(0)) return 1;
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;
	const bool Quick = argc == 2 && std::string_view(argv[1]) == "--quick";
	try {
		const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
		const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-package-benchmark-" + Suffix);
		struct Cleanup final {
			std::filesystem::path Root;
			~Cleanup() {
				std::error_code Ignored;
				std::filesystem::remove_all(Root, Ignored);
			}
		} CleanupValue{Root};
		std::filesystem::create_directory(Root);
		const auto ProjectRoot = Root / "Project";
		std::filesystem::copy(
			std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT),
			ProjectRoot,
			std::filesystem::copy_options::recursive
		);

		BootstrapProjectRuntimeSchema(ProjectRoot);
		DiskFilesystem Filesystem(ProjectRoot);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto World = ProjectValue.DeserializeGame();
		World->InitializeLoadedProjectRevision();
		World->Root = ProjectRoot;
		World->Filesystem = &Filesystem;
		const auto Revision = World->GetAuthoritativeRevision();
		const auto CaptureStarted = std::chrono::steady_clock::now();
		auto Small = PackageBuilder::Capture(ProjectValue, World, Revision, Revision);
		const auto CaptureMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - CaptureStarted).count();
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->FindFirstChildOfClass("AssetService", false));
		if (!Assets) throw std::runtime_error("Benchmark fixture has no AssetService");
		const auto AssetsStarted = std::chrono::steady_clock::now();
		auto AssetSnapshot = Assets->CaptureRuntimeAssets();
		const auto AssetMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - AssetsStarted).count();
		std::cout << "[Package:Benchmark] authoritativeCaptureMs=" << CaptureMilliseconds
				  << " assetEnumerationMs=" << AssetMilliseconds << " assetCount=" << AssetSnapshot.Artifacts.size()
				  << '\n';

		const auto Runtime = std::filesystem::absolute(GARGANTUAN_RUNTIME_DISTRIBUTION_ROOT).lexically_normal();
		RunBuild("small", Small, Runtime, Root / "Small");
		RunBuild("medium", ExpandPayload(Small, Quick ? 100 : 1000, Quick ? 2 : 20), Runtime, Root / "Medium");
		RunBuild("large", ExpandPayload(Small, Quick ? 500 : 5000, Quick ? 10 : 100), Runtime, Root / "Large");
		World->Destroy();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:Benchmark] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/SchemaId.hpp"

#include <SDL3/SDL.h>
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
			const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
			Root = std::filesystem::temp_directory_path() / ("gargantuan-package-host-" + Suffix);
			ProjectRoot = Root / "Project";
			OutputRoot = Root / "Package";
			std::filesystem::create_directory(Root);
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
		std::filesystem::path OutputRoot;
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	std::string ReadText(const std::filesystem::path &Path) {
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) throw std::runtime_error("Could not read packaged EditorHost fixture");
		return {std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()};
	}

	bool HasNamedObject(const Json &Node, std::string_view Name) {
		if (!Node.is_object()) return false;
		if (Node.value("Name", "") == Name) return true;
		if (!Node.contains("Children") || !Node["Children"].is_array()) return false;
		return std::ranges::any_of(Node["Children"], [&](const Json &Child) { return HasNamedObject(Child, Name); });
	}
}

int main() {
	using namespace gargantuan;
	if (!SDL_Init(0)) {
		std::cerr << "[Package:EditorHostTest] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;
	try {
		TestWorkspace Workspace;
		EditorHost Host("package-host-test");
		std::size_t RequestNumber = 0;
		auto Call = [&](std::string Method, Json Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(
				Json{
					{"Version", EditorHostProtocolVersion},
					{"RequestId", std::to_string(++RequestNumber)},
					{"SessionToken", "package-host-test"},
					{"Method", std::move(Method)},
					{"Params", std::move(Parameters)},
				}
					.dump()
			));
		};

		auto Handshake = Call("Handshake");
		Require(
			Handshake["Ok"] && std::ranges::contains(Handshake["Result"]["Capabilities"], "PackageBuild") &&
				std::ranges::contains(Handshake["Result"]["Capabilities"], "PackageBuildProgress") &&
				std::ranges::contains(Handshake["Result"]["Capabilities"], "PackageBuildCancellation"),
			"EditorHost did not publish the complete package job capability contract"
		);
		auto Opened = Call("OpenProject", {{"Root", Workspace.ProjectRoot.generic_string()}});
		Require(Opened["Ok"], "EditorHost package fixture did not open");
		auto Snapshot = Call("GetSnapshot");
		Require(Snapshot["Ok"], "EditorHost package fixture did not establish a snapshot cursor");
		const auto &Objects = Snapshot["Result"]["Snapshot"]["Objects"];
		auto WorkspaceObject = std::ranges::find_if(Objects, [](const Json &Object) {
			return Object.value("ClassName", "") == "Workspace";
		});
		Require(WorkspaceObject != Objects.end(), "EditorHost package fixture has no Workspace");

		auto State = Call("GetProjectState");
		const auto InitialRevision = State["Result"]["AuthoritativeRevision"].get<std::uint64_t>();
		auto UnsavedMutation = Call(
			"CreateInstance",
			{
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
				{"DefinitionVersion", 1},
				{"Parent", (*WorkspaceObject)["Id"]},
				{"Name", "Packaged Unsaved Folder"},
				{"ExpectedRevision", InitialRevision},
			}
		);
		Require(UnsavedMutation["Ok"], "unsaved authoritative mutation failed before package capture");
		const auto CapturedRevision = UnsavedMutation["Result"]["AuthoritativeRevision"].get<std::uint64_t>();

		auto Started = Call(
			"StartPackageBuild",
			{
				{"OutputDirectory", Workspace.OutputRoot.generic_string()},
				{"Configuration", "Release"},
				{"ExpectedRevision", CapturedRevision},
			}
		);
		Require(
			Started["Ok"] && Started["Result"]["CapturedRevision"] == CapturedRevision &&
				Started["Result"]["CapturedUnsavedChanges"],
			"EditorHost did not capture the exact unsaved authoritative revision"
		);
		const auto JobId = Started["Result"]["JobId"];

		auto ConcurrentMutation = Call(
			"CreateInstance",
			{
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
				{"DefinitionVersion", 1},
				{"Parent", (*WorkspaceObject)["Id"]},
				{"Name", "Later Authoring Folder"},
				{"ExpectedRevision", CapturedRevision},
			}
		);
		Require(ConcurrentMutation["Ok"], "authoring was incorrectly locked after immutable package capture");

		Json Completed;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do {
			Completed = Call("GetPackageBuildState", {{"JobId", JobId}});
			Require(Completed["Ok"], "EditorHost package progress polling failed");
			if (Completed["Result"]["Complete"].get<bool>()) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		} while (std::chrono::steady_clock::now() < Deadline);
		Require(
			Completed["Result"].value("Complete", false) && Completed["Result"].value("Ok", false) &&
				Completed["Result"]["CapturedRevision"] == CapturedRevision &&
				Completed["Result"]["CapturedUnsavedChanges"].get<bool>() &&
				Completed["Result"]["Size"]["TotalBytes"].get<std::uint64_t>() > 0,
			"EditorHost package job did not complete with bounded progress/result metadata"
		);
		const auto PackagedWorld = Json::parse(ReadText(Workspace.OutputRoot / "content/game.instance.json"));
		Require(
			HasNamedObject(PackagedWorld, "Packaged Unsaved Folder") &&
				!HasNamedObject(PackagedWorld, "Later Authoring Folder"),
			"package job did not preserve its immutable capture boundary"
		);
		const auto PackageManifest = Json::parse(ReadText(Workspace.OutputRoot / "game.package.json"));
		Require(
			PackageManifest["Revision"] == CapturedRevision && PackageManifest["UnsavedChanges"].get<bool>(),
			"package manifest did not disclose its captured unsaved revision"
		);

		auto Stale = Call(
			"StartPackageBuild",
			{
				{"OutputDirectory", (Workspace.Root / "Stale").generic_string()},
				{"Configuration", "Release"},
				{"ExpectedRevision", CapturedRevision},
			}
		);
		Require(
			!Stale["Ok"].get<bool>() && Stale["Error"]["Code"] == "Conflict",
			"stale Studio package capture silently overwrote a newer authoritative revision"
		);

		auto PlayStarted = Call("StartPlaySession");
		Require(PlayStarted["Ok"], "package fixture could not enter Play");
		const auto PlaySessionId = PlayStarted["Result"]["PlaySessionId"];
		auto DuringPlay = Call(
			"StartPackageBuild",
			{
				{"OutputDirectory", (Workspace.Root / "DuringPlay").generic_string()},
				{"Configuration", "Release"},
			}
		);
		Require(
			!DuringPlay["Ok"].get<bool>() && DuringPlay["Error"]["Code"] == "PlaySessionActive",
			"packaging was allowed to capture the runtime Play world"
		);
		Require(
			Call("StopPlaySession", {{"PlaySessionId", PlaySessionId}})["Ok"], "package fixture could not leave Play"
		);

		EditorHost Restricted("restricted-package-host", ScriptSecurityContext::ServerRuntime());
		auto RestrictedCall = [&](std::string Method, Json Parameters) {
			return Json::parse(Restricted.HandleRequest(
				Json{
					{"Version", EditorHostProtocolVersion},
					{"RequestId", "restricted"},
					{"SessionToken", "restricted-package-host"},
					{"Method", std::move(Method)},
					{"Params", std::move(Parameters)},
				}
					.dump()
			));
		};
		auto UnauthorizedStart = RestrictedCall(
			"StartPackageBuild",
			{
				{"OutputDirectory", (Workspace.Root / "Unauthorized").generic_string()},
				{"Configuration", "Release"},
			}
		);
		auto UnauthorizedPoll = RestrictedCall("GetPackageBuildState", {{"JobId", "1"}});
		Require(
			!UnauthorizedStart["Ok"].get<bool>() && UnauthorizedStart["Error"]["Code"] == "Unauthorized" &&
				!UnauthorizedPoll["Ok"].get<bool>() && UnauthorizedPoll["Error"]["Code"] == "Unauthorized",
			"package job authority was available outside the Studio command capability boundary"
		);

		std::cout << "[Package:EditorHostTest] revision=" << CapturedRevision
				  << " totalBytes=" << Completed["Result"]["Size"]["TotalBytes"] << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Package:EditorHostTest] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

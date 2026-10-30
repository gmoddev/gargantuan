#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <thread>

int main() {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	const auto Root = std::filesystem::temp_directory_path() /
					  ("gargantuan-storage-editor-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
			SDL_Quit();
		}
	} CleanupValue{Root};
	try {
		if (!SDL_Init(0)) throw std::runtime_error("SDL initialization failed");
		std::filesystem::create_directory(Root);
		BootstrapNativeRuntimeSchema();
		std::size_t Checks = 0, Sequence = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		EditorHost Host("storage-editor-test");
		auto Call = [&](std::string Method, Json Parameters) {
			auto Response = Json::parse(Host.HandleRequest(
				Json{
					{"Version", 1},
					{"RequestId", std::to_string(++Sequence)},
					{"SessionToken", "storage-editor-test"},
					{"Method", Method},
					{"Params", std::move(Parameters)}
				}.dump()
			));
			if (!Response["Ok"])
				std::cout << "[Storage:EditorHost] " << Method << " " << Response["Error"].dump() << '\n';
			return Response;
		};
		const auto ProjectRoot = Root / "Project";
		Check(
			Call("CreateProject", {{"Destination", ProjectRoot.generic_string()}, {"Name", "Shared storage"}})["Ok"],
			"Create authoring fixture"
		);
		std::ofstream PreRun(ProjectRoot / ".gargantuan" / "prerun.luau");
		PreRun << "Schema:RegisterClass({Namespace='Game',Name='SharedItems',Version=1,Base='Engine.Folder',Service="
				  "true,Replicated=true,Properties={}})";
		PreRun.close();
		Check(
			Call("OpenProject", {{"Root", ProjectRoot.generic_string()}})["Ok"],
			"Open project with custom replicated service"
		);
		auto FindObject = [&](std::string_view ClassName) {
			auto Response = Call("GetSnapshot", Json::object());
			if (!Response["Ok"]) throw std::runtime_error("Snapshot failed");
			for (const auto &Object : Response["Result"]["Snapshot"]["Objects"])
				if (Object.value("ClassName", "") == ClassName) return Object["Id"];
			throw std::runtime_error("Missing service in normal Studio snapshot");
		};
		const auto BuiltIn = FindObject("ReplicatedStorage"), Custom = FindObject("Game.SharedItems");
		Check(
			!BuiltIn.is_null() && !Custom.is_null(), "Normal Explorer projection exposes both native and custom roots"
		);
		auto Create = [&](std::string_view ClassName, const Json &Parent, std::string Name) {
			return Call(
				"CreateInstance",
				{{"ClassSchemaId", SchemaId::FromNativeName("Engine", ClassName).ToString()},
				 {"DefinitionVersion", 1},
				 {"Parent", Parent},
				 {"Name", Name}}
			);
		};
		auto Folder = Create("Folder", BuiltIn, "StoredFolder");
		Check(Folder["Ok"], "Existing Studio command creates a storage descendant");
		auto Module = Create("ModuleScript", Custom, "SharedModule");
		Check(Module["Ok"], "Existing Studio command creates a module under custom storage");
		const auto ModuleId = Module["Result"]["Object"];
		auto Source = Call("GetScriptSource", {{"Object", ModuleId}});
		Check(Source["Ok"], "Existing source editor reads stored module");
		const std::string ModuleSource = "return { Shared = true }";
		Check(
			Call(
				"SetScriptSource",
				{{"Object", ModuleId},
				 {"ExpectedSourceVersion", Source["Result"]["SourceVersion"]},
				 {"Source", ModuleSource}}
			)["Ok"],
			"Existing source editor commits stored module"
		);
		Check(
			Call("ReparentInstance", {{"Object", ModuleId}, {"Parent", BuiltIn}})["Ok"],
			"Authorized descendant reparent remains supported"
		);
		auto Copy = Call("DuplicateInstance", {{"Object", ModuleId}});
		Check(Copy["Ok"], "Authorized descendant duplication remains supported");
		Check(
			Call("DestroyInstance", {{"Object", Copy["Result"]["Object"]}})["Ok"],
			"Authorized descendant deletion remains supported"
		);
		Check(
			!Call("DestroyInstance", {{"Object", BuiltIn}})["Ok"] &&
				!Call("DuplicateInstance", {{"Object", Custom}})["Ok"],
			"Normal commands preserve singleton root lifecycle"
		);
		Check(
			Call("SaveProject", Json::object())["Ok"] &&
				Call("OpenProject", {{"Root", ProjectRoot.generic_string()}})["Ok"],
			"Save/reopen through actual authoring boundary"
		);
		const auto ReopenedModule = FindObject("ModuleScript");
		Check(
			Call("GetScriptSource", {{"Object", ReopenedModule}})["Result"]["Source"] == ModuleSource,
			"Stored source survives reopen"
		);
		auto Started = Call(
			"StartPackageBuild",
			{{"OutputDirectory", (Root / "Package").generic_string()}, {"Configuration", "Release"}}
		);
		Check(Started["Ok"], "Capture package through existing EditorHost job");
		Json Completed;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do {
			Completed = Call("GetPackageBuildState", {{"JobId", Started["Result"]["JobId"]}});
			if (!Completed["Ok"]) throw std::runtime_error("Package job failed");
			if (Completed["Result"].value("Complete", false)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		} while (std::chrono::steady_clock::now() < Deadline);
		Check(
			Completed["Result"].value("Complete", false) && Completed["Result"].value("Ok", false),
			"Package closure succeeds"
		);
		std::vector<PackageDiagnostic> Diagnostics;
		auto Payload = PackageBuilder::Load(Root / "Package", Diagnostics);
		Check(
			Payload && Payload->PreRunSource && Payload->PreRunSource->find("Replicated=true") != std::string::npos,
			"Package retains canonical custom-service declaration"
		);
		auto PackageWorld = PackageBuilder::LoadWorld(*Payload, Root / "Package");
		auto PackageModule = std::dynamic_pointer_cast<ModuleScript>(
			PackageWorld->GetService("ReplicatedStorage")->FindFirstChild("SharedModule", false)
		);
		const auto LoadedCustom = PackageWorld->FindFirstChildOfClass("Game.SharedItems", false);
		Check(
			PackageModule && PackageModule->GetSource() == ModuleSource && LoadedCustom &&
				PackageWorld->GetService("Game.SharedItems") == LoadedCustom,
			"Runtime package preserves service identities and module closure"
		);
		PackageWorld->Destroy();
		std::cout << "[Storage:EditorHost] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Storage:EditorHost] FAIL " << Error.what() << '\n';
		return 1;
	}
}

#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace {
	using Json = nlohmann::ordered_json;
	Json Encode(std::string_view Source) {
		constexpr std::string_view Digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Encoded;
		for (std::size_t Index = 0; Index < Source.size(); Index += 3) {
			const auto Count = std::min<std::size_t>(3, Source.size() - Index);
			std::uint32_t Value = 0;
			for (std::size_t Offset = 0; Offset < 3; ++Offset)
				Value = (Value << 8) | (Offset < Count ? static_cast<unsigned char>(Source[Index + Offset]) : 0);
			Encoded += Digits[(Value >> 18) & 63];
			Encoded += Digits[(Value >> 12) & 63];
			Encoded += Count > 1 ? Digits[(Value >> 6) & 63] : '=';
			Encoded += Count > 2 ? Digits[Value & 63] : '=';
		}
		Json Chunks = Json::array();
		for (std::size_t Offset = 0; Offset < Encoded.size(); Offset += 65536)
			Chunks.push_back(Encoded.substr(Offset, 65536));
		if (Chunks.empty()) Chunks.push_back("");
		return Chunks;
	}
}

int main() {
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
					  ("gargantuan-prerun-editor-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
			std::filesystem::remove_all(std::filesystem::path(Root.string() + "-copy"), Ignored);
			std::filesystem::remove(std::filesystem::path(Root.string() + "-external.luau"), Ignored);
			SDL_Quit();
		}
	} CleanupValue{Root};
	try {
		if (!SDL_Init(0)) throw std::runtime_error("SDL initialization failed");
		BootstrapNativeRuntimeSchema();
		std::size_t Checks = 0, Sequence = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		EditorHost Host("prerun-editor");
		auto Call = [&](std::string Method, Json Parameters) {
			return Json::parse(Host.HandleRequest(
				Json{
					{"Version", 1},
					{"RequestId", std::to_string(++Sequence)},
					{"SessionToken", "prerun-editor"},
					{"Method", Method},
					{"Params", std::move(Parameters)}
				}.dump()
			));
		};
		Check(
			Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "PreRun Editing"}})["Ok"],
			"Create project"
		);
		auto Initial = Call("GetPreRunSource", Json::object());
		Check(
			Initial["Ok"] && !Initial["Result"]["Exists"] && !Initial["Result"]["ReloadRequired"],
			"Absent canonical source is an editable document, not a Script Instance"
		);
		const auto InitialSchema = GetRuntimeSchemaLifecycle().GetActiveGeneration();
		const auto CallerDomain = GetCurrentScriptSecurityContext().Domain;
		const auto InitialState = Call("GetProjectState", Json::object())["Result"];
		auto Parameters = [&](const Json &State, std::string_view Source) {
			return Json{
				{"ProjectRoot", State["ProjectRoot"]},
				{"ExpectedSourceRevision", State["SourceRevision"]},
				{"SourceBase64", Encode(Source)}
			};
		};
		const std::string ValidSource = R"(
Schema:RegisterEnum({Namespace="Game",Name="Quality",Version=1,Items={Normal=1,Rare=2}})
Schema:RegisterExtension({Namespace="Game",Name="Marker",Version=1,Target="Engine.Part",Properties={Score={Type="Integer",Default=4}}})
Schema:RegisterClass({Namespace="Game",Name="PreRunFolder",Version=1,Base="Engine.Folder",Properties={Enabled={Type="Boolean",Default=true}}})
)";
		for (int Attempt = 0; Attempt < 3; ++Attempt) {
			auto Churn = Call(
				"ValidatePreRun",
				Parameters(Initial["Result"], "while true do local Values = table.create(1024, 'temporary') end")
			);
			Check(
				Churn["Ok"] && !Churn["Result"]["Validated"] &&
					Churn["Result"]["Diagnostics"][0]["Code"] == "PreRunBudgetExceeded" &&
					GetRuntimeSchemaLifecycle().GetActiveGeneration() == InitialSchema,
				"GC allocation churn times out without recursive interrupt or partial schema publication"
			);
		}
		for (const auto *ProtectedLoop :
			 {"while true do pcall(function() while true do end end) end",
			  "local T=setmetatable({}, {__index=function() while true do end end}); pcall(function() return T.Value "
			  "end); while true do end",
			  "Schema:RegisterClass({Namespace='Game',Name='NeverPublished',Version=1,Base='Engine.Folder',Properties={"
			  "Marker={Type='Boolean',Default=true}}}); while true do xpcall(function() while true do end end, "
			  "function() return 'caught' end) end"}) {
			auto Cancelled = Call("ValidatePreRun", Parameters(Initial["Result"], ProtectedLoop));
			Check(
				Cancelled["Ok"] && !Cancelled["Result"]["Validated"] &&
					Cancelled["Result"]["Diagnostics"][0]["Code"] == "PreRunBudgetExceeded" &&
					GetRuntimeSchemaLifecycle().GetActiveGeneration() == InitialSchema &&
					!GetActiveRuntimeSchemaRegistry().FindClassByName("Game.NeverPublished") &&
					GetCurrentScriptSecurityContext().Domain == CallerDomain,
				"Deadline cancellation escapes protected calls and metamethod C boundaries without granting more "
				"execution"
			);
		}
		auto Validate = Call("ValidatePreRun", Parameters(Initial["Result"], ValidSource));
		Check(
			Validate["Ok"] && Validate["Result"]["Validated"] && !Validate["Result"]["ActiveSchemaChanged"],
			"Validate all existing Schema interfaces in a disposable candidate"
		);
		Check(
			GetRuntimeSchemaLifecycle().GetActiveGeneration() == InitialSchema &&
				!GetActiveRuntimeSchemaRegistry().FindClassByName("Game.PreRunFolder") &&
				!ReadProjectPreRunSource(Root) && Call("GetProjectState", Json::object())["Result"] == InitialState,
			"Successful validation neither saves source nor mutates active registry or project"
		);
		for (const auto &BadSource :
			 {std::string("Schema:RegisterClass("),
			  std::string("while true do end"),
			  std::string("local T={} while true do table.insert(T,string.rep('x',65536)) end"),
			  ValidSource + ValidSource,
			  std::string(
				  "Schema:RegisterClass({Namespace='Game',Name='Bad',Version=1,Base='Engine.Unknown',Properties={X={"
				  "Type='Boolean',Default=true}}})"
			  ),
			  std::string("assert(game == nil and io == nil and os == nil); error('sandbox rejection')")}) {
			auto Rejected = Call("ValidatePreRun", Parameters(Initial["Result"], BadSource));
			Check(
				Rejected["Ok"] && !Rejected["Result"]["Validated"] && !Rejected["Result"]["Diagnostics"].empty() &&
					GetRuntimeSchemaLifecycle().GetActiveGeneration() == InitialSchema,
				"Invalid, timed-out, exhausted or denied candidates never publish partial schema"
			);
		}
		auto Saved = Call("SetPreRunSource", Parameters(Initial["Result"], ValidSource));
		Check(
			Saved["Ok"] && Saved["Result"]["ReloadRequired"] && ReadProjectPreRunSource(Root) == ValidSource &&
				GetRuntimeSchemaLifecycle().GetActiveGeneration() == InitialSchema,
			"Saving source is atomic and explicitly pending reload"
		);
		auto Stale = Call("SetPreRunSource", Parameters(Initial["Result"], "-- stale buffer"));
		auto PendingPlay = Call("StartPlaySession", Json::object());
		auto PendingPackage = Call(
			"StartPackageBuild",
			{{"OutputDirectory", (Root / "PendingPackage").generic_string()}, {"Configuration", "Release"}}
		);
		Check(
			!PendingPlay["Ok"] && PendingPlay["Error"]["Code"] == "PreRunReloadRequired" && !PendingPackage["Ok"] &&
				PendingPackage["Error"]["Code"] == "PreRunReloadRequired",
			"Play/package cannot mix pending source with an old active schema"
		);
		Check(
			!Stale["Ok"] && Stale["Error"]["Code"] == "Conflict",
			"Second stale editor cannot overwrite committed source"
		);
		auto Current = Call("GetPreRunSource", Json::object())["Result"];
		std::ofstream External(Root / ".gargantuan/prerun.luau", std::ios::binary | std::ios::app);
		External << "-- external\n";
		External.close();
		auto ExternalConflict = Call("ValidatePreRun", Parameters(Current, ValidSource));
		Check(
			!ExternalConflict["Ok"] && ExternalConflict["Error"]["Code"] == "Conflict",
			"External source change invalidates a stale validation buffer"
		);
		Current = Call("GetPreRunSource", Json::object())["Result"];
		Host.SetPersistenceCheckpointForTesting([&] {
			std::ofstream Changed(Root / ".gargantuan/prerun.luau");
			Changed << "-- race winner";
		});
		auto Race = Call("SetPreRunSource", Parameters(Current, ValidSource));
		Host.SetPersistenceCheckpointForTesting({});
		Check(
			!Race["Ok"] && Race["Error"]["Code"] == "Conflict" && ReadProjectPreRunSource(Root) == "-- race winner",
			"Last-moment conflict preserves the external writer and removes temporary source"
		);
		for (const auto &Entry : std::filesystem::directory_iterator(Root / ".gargantuan"))
			Check(
				Entry.path().filename().string().find(".saving-") == std::string::npos, "No abandoned temporary source"
			);
		Current = Call("GetPreRunSource", Json::object())["Result"];
		Check(
			Call("SetPreRunSource", Parameters(Current, ValidSource))["Ok"] &&
				Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"],
			"Explicit reopen activates saved source"
		);
		const auto Active = Call("GetPreRunSource", Json::object())["Result"];
		Check(
			!Active["ReloadRequired"] && GetActiveRuntimeSchemaRegistry().FindClassByName("Game.PreRunFolder") &&
				GetRuntimeSchemaLifecycle().GetActiveGeneration() > InitialSchema,
			"Only project reload publishes the new schema generation"
		);
		auto OldProject = Call("SetPreRunSource", Parameters(Current, ValidSource));
		Check(
			!OldProject["Ok"] && OldProject["Error"]["Code"] == "StaleProject",
			"Reopened project rejects old object generation"
		);
		const auto Snapshot = Call("GetSnapshot", Json::object());
		Json Parent;
		for (const auto &Object : Snapshot["Result"]["Snapshot"]["Objects"])
			if (Object["ClassName"] == "Workspace") Parent = Object["Id"];
		auto Created = Call(
			"CreateInstance",
			{{"ClassSchemaId", SchemaId::FromCustomClassName("Game", "PreRunFolder").ToString()},
			 {"DefinitionVersion", 1},
			 {"Parent", Parent},
			 {"Name", "Custom authored object"}}
		);
		Check(
			Created["Ok"] && Call("SaveProject", Json::object())["Ok"] &&
				Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"],
			"Activated custom class is authorable and persists through save/reopen"
		);
		Current = Call("GetPreRunSource", Json::object())["Result"];
		Check(
			Call("SetPreRunSource", Parameters(Current, std::string(MaximumPreRunSourceBytes, ' ')))["Ok"],
			"Exact 256 KiB source fits bounded Base64 chunks"
		);
		Current = Call("GetPreRunSource", Json::object())["Result"];
		auto TooLarge = Call("SetPreRunSource", Parameters(Current, std::string(MaximumPreRunSourceBytes + 1, ' ')));
		Check(!TooLarge["Ok"], "Oversized source is rejected");
		auto Malformed = Parameters(Current, "");
		Malformed["SourceBase64"] = Json::array({"/w=="});
		Check(!Call("SetPreRunSource", Malformed)["Ok"], "Invalid UTF-8 source is rejected");
		Malformed["SourceBase64"] = Json::array({"Zh=="});
		Check(!Call("SetPreRunSource", Malformed)["Ok"], "Noncanonical Base64 is rejected");
		Malformed = Parameters(Current, "-- valid");
		Malformed["Path"] = "../outside.luau";
		Check(!Call("SetPreRunSource", Malformed)["Ok"], "Caller cannot select another filesystem path");
		Check(Call("SetPreRunSource", Parameters(Current, ValidSource))["Ok"], "Source recovers after rejected edits");
		Current = Call("GetPreRunSource", Json::object())["Result"];
		std::cout << "[PreRun:EditorHost] exercising active Play lifecycle" << std::endl;
		auto Playing = Call("StartPlaySession", Json::object());
		Check(Playing["Ok"], "Active schema remains runnable after source recovery");
		auto PlayingSave = Call("SetPreRunSource", Parameters(Current, "-- blocked"));
		auto PlayingValidation = Call("ValidatePreRun", Parameters(Current, ValidSource));
		Check(
			!PlayingSave["Ok"] && PlayingSave["Error"]["Code"] == "PlaySessionActive" && !PlayingValidation["Ok"] &&
				PlayingValidation["Error"]["Code"] == "PlaySessionActive",
			"PreRun save and validation reject incompatible Play lifecycle"
		);
		Check(
			Call("StopPlaySession", {{"PlaySessionId", Playing["Result"]["PlaySessionId"]}})["Ok"],
			"Play cleanup succeeds"
		);
		const auto ExternalPath = std::filesystem::path(Root.string() + "-external.luau");
		std::ofstream ExternalSource(ExternalPath);
		ExternalSource << "-- outside project";
		ExternalSource.close();
		std::filesystem::remove(Root / ".gargantuan/prerun.luau");
		std::error_code LinkError;
		std::filesystem::create_symlink(ExternalPath, Root / ".gargantuan/prerun.luau", LinkError);
		if (!LinkError) {
			Check(
				!Call("GetPreRunSource", Json::object())["Ok"] &&
					!Call("SetPreRunSource", Parameters(Current, "-- escape"))["Ok"],
				"Linked source cannot escape the canonical project resource"
			);
			std::filesystem::remove(Root / ".gargantuan/prerun.luau");
		} else
			std::cout << "[PreRun:EditorHost] symlink test unavailable: " << LinkError.message() << '\n';
		std::ofstream RestoredSource(Root / ".gargantuan/prerun.luau", std::ios::binary);
		RestoredSource << ValidSource;
		RestoredSource.close();
		const auto CopyRoot = std::filesystem::path(Root.string() + "-copy");
		std::cout << "[PreRun:EditorHost] exercising Save As lifecycle" << std::endl;
		const auto SavedAs = Call("SaveProjectAs", {{"Destination", CopyRoot.generic_string()}});
		if (!SavedAs["Ok"]) std::cout << "[PreRun:EditorHost] Save As " << SavedAs["Error"].dump() << std::endl;
		Check(
			SavedAs["Ok"] && ReadProjectPreRunSource(CopyRoot) == ValidSource,
			"Save As copies the exact bounded canonical source"
		);
		auto Retargeted = Call("SetPreRunSource", Parameters(Current, "-- old location"));
		Check(
			!Retargeted["Ok"] && Retargeted["Error"]["Code"] == "Conflict",
			"Save As cannot silently retarget an old source token"
		);
		Check(
			Call("OpenProject", {{"Root", CopyRoot.generic_string()}})["Ok"],
			"Schema replacement uses the canonical world reconstruction boundary"
		);
		auto ReplacedSchema = Call("GetPreRunSource", Json::object());
		Check(
			ReplacedSchema["Ok"] && ReplacedSchema["Result"]["SchemaGeneration"] > Current["SchemaGeneration"],
			"Project reopen publishes a fresh schema generation after destroying the old world"
		);
		auto RetiredBuffer = Call("ValidatePreRun", Parameters(Current, ValidSource));
		Check(
			!RetiredBuffer["Ok"] && RetiredBuffer["Error"]["Code"] == "StaleProject",
			"Old schema/project buffers cannot validate against a replacement world"
		);
		EditorHost Denied("denied", ScriptSecurityContext::ClientRuntime());
		auto Unauthorized = Json::parse(Denied.HandleRequest(
			Json{
				{"Version", 1},
				{"RequestId", "denied"},
				{"SessionToken", "denied"},
				{"Method", "GetPreRunSource"},
				{"Params", Json::object()}
			}.dump()
		));
		Check(
			!Unauthorized["Ok"] && Unauthorized["Error"]["Code"] == "Unauthorized",
			"Runtime client lacks PreRun authoring authority"
		);
		std::cout << "[PreRun:EditorHost] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[PreRun:EditorHost] FAIL " << Error.what() << '\n';
		return 1;
	}
}

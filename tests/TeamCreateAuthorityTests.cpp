// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/InstanceProperty.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "../src/serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

int main() {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-team-authority-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
		BootstrapNativeRuntimeSchema();
		EditorHost Host("team-authority");
		std::size_t Sequence = 0;
		auto Call = [&](const char *Method, Json Params, std::uint64_t Author = 0) {
			Json Envelope{{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", "team-authority"}, {"Method", Method}, {"Params", std::move(Params)}};
			if (Author != 0) Envelope["Team"] = {{"Author", Author}};
			return Json::parse(Host.HandleRequest(Envelope.dump()));
		};
		auto Require = [](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
		};
		auto Success = [&](const Json &Response) -> Json {
			if (!Response.at("Ok").get<bool>()) throw std::runtime_error(Response.dump());
			return Response.at("Result");
		};
		auto Rejects = [&](const Json &Response, const char *Code) {
			if (Response.at("Ok").get<bool>() || Response.at("Error").at("Code") != Code)
				throw std::runtime_error(std::string("Expected ") + Code + ": " + Response.dump());
		};
		const auto TeamHandshake = Success(Call("Handshake", Json::object(), 1));
		const auto LocalHandshake = Success(Call("Handshake", Json::object()));
		Require(TeamHandshake["TeamAuthoringVersion"] == 1, "Team contract is versioned");
		Require(TeamHandshake["ScriptSecurityVersion"] == 1 && TeamHandshake["StudioExecutionDomain"] == "Studio" &&
			TeamHandshake["ScriptSecurityVersion"] == LocalHandshake["ScriptSecurityVersion"] &&
			TeamHandshake["StudioExecutionDomain"] == LocalHandshake["StudioExecutionDomain"],
			"Both handshakes expose the Studio bootstrap security contract");
		Require(!LocalHandshake.contains("TeamAuthoringVersion"), "The local handshake stays compatible");
		Rejects(Call("Handshake", Json::object(), 9), "MalformedRequest");
		Rejects(Call("BeginTransaction", {{"Label", "Remote group"}}, 1), "Unauthorized");
		Rejects(Call("ConfigureViewport", Json::object(), 1), "Unauthorized");
		Success(Call("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Team project"}}));
		Success(Call("GetSnapshot", Json::object()));
		Rejects(Call("PollChanges", Json::object(), 1), "SnapshotRequired");
		const auto First = Success(Call("GetSnapshot", Json::object(), 1));
		Rejects(Call("PollChanges", Json::object(), 2), "SnapshotRequired");
		Require(Success(Call("GetSnapshot", Json::object(), 2))["Snapshot"] == First["Snapshot"],
			"Both authors see the same initial snapshot independently");
		const auto &State = First["ProjectState"];
		Require(State["CurrentDestination"].is_null() && State["ProjectId"].is_string() && State["SchemaGeneration"].is_number(),
			"Team state exposes project and schema identity without its filesystem destination");
		Require(Success(Call("GetProjectState", Json::object()))["CurrentDestination"].is_string(), "Local state retains the destination");
		const auto Scope = First["Snapshot"]["Cursor"]["Scope"];
		const auto ScopeId = JsonCodec::DecodeObjectId(Scope);
		auto World = ScopeId ? std::dynamic_pointer_cast<DataModel>(ObjectRegistry::Get().Lookup(ScopeId->ToObjectId())) : nullptr;
		Require(World != nullptr, "Snapshot binds to the authoritative project");
		const auto Workspace = World->FindFirstChild("Workspace", false);
		Require(Workspace != nullptr, "Project exposes Workspace");
		auto EncodeId = [](const std::shared_ptr<Instance> &Object) {
			return JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Object->GetObjectId()));
		};
		auto Revision = [&] { return World->GetAuthoritativeRevision(); };
		auto Create = [&](const char *Class, const char *Name, std::uint64_t Author) {
			return Success(Call("CreateInstance", {{"ClassSchemaId", SchemaId::FromNativeName("Engine", Class).ToString()},
				{"DefinitionVersion", 1}, {"Parent", EncodeId(Workspace)}, {"Name", Name}, {"ExpectedRevision", Revision()}}, Author))
				.at("Object");
		};
		const auto A = Create("Part", "A", 1);
		const auto B = Create("Part", "B", 2);
		const auto Script = Create("ModuleScript", "Shared", 1);
		Success(Call("GetSnapshot", Json::object(), 1));
		Success(Call("GetSnapshot", Json::object(), 2));
		auto Rename = [&](const Json &Id, const char *Name, std::uint64_t Author) {
			return Call("SetProperty", {{"Object", Id}, {"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", Name}}}}, Author);
		};
		const auto BeforeRename = Revision();
		Success(Rename(A, "First", 1));
		Success(Rename(A, "Second", 2));
		Require(World->FindFirstChild("Second", true) != nullptr, "Accepted field updates follow Engine commit order");
		Rejects(Call("DestroyInstance", {{"Object", A}, {"ExpectedRevision", BeforeRename}}, 1), "Conflict");
		Rejects(Call("DestroyInstance", {{"Object", A}}, 1), "MalformedRequest");
		const auto Top = Success(Call("GetTeamHistory", Json::object(), 1));
		Require(Top["Entries"].back()["Author"] == 2 && Top["CanUndo"] == false, "History reports the winning author");
		Rejects(Call("Undo", {{"ExpectedRevision", Revision()}}, 1), "HistoryConflict");
		auto Undo = Success(Call("Undo", {{"ExpectedRevision", Revision()}}, 2));
		Require(Undo["CanUndo"] == false && Undo["CanRedo"] == true &&
			World->FindFirstChild("First", true) != nullptr, "The top author can undo their write safely");
		Rejects(Call("Redo", {{"ExpectedRevision", Revision()}}, 1), "HistoryConflict");
		Success(Call("Redo", {{"ExpectedRevision", Revision()}}, 2));
		Require(World->FindFirstChild("Second", true) != nullptr, "The same author can redo their write");

		auto ReadSource = [&] { return Success(Call("GetScriptSource", {{"Object", Script}}, 2)); };
		const auto Version = ReadSource()["SourceVersion"];
		Success(Call("SetScriptSource", {{"Object", Script}, {"ExpectedSourceVersion", Version},
			{"Source", "return 17"}}, 1));
		Rejects(Call("SetScriptSource", {{"Object", Script}, {"ExpectedSourceVersion", Version},
			{"Source", "return 19"}}, 2), "SourceConflict");
		Require(ReadSource()["Source"] == "return 17", "A stale source submit cannot overwrite the authoritative text");

		Success(Call("GetSnapshot", Json::object(), 1));
		Success(Call("GetSnapshot", Json::object(), 2));
		Success(Rename(A, "Before batch", 1));
		const auto *NameProperty = Workspace->FindProperty("Name");
		Require(NameProperty != nullptr, "Native Name has an authoritative schema identity");
		auto Write = [&](const Json &Id, const char *Name) {
			return Json{{"Object", Id}, {"DeclaringClassSchemaId", NameProperty->DeclaringSchemaId.ToString()},
				{"DeclaringDefinitionVersion", NameProperty->DeclaringDefinitionVersion}, {"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", Name}}}};
		};
		Success(Call("SetPropertyBatch", {{"PropertyBatchVersion", 1}, {"Scope", Scope}, {"ExpectedRevision", Revision()},
			{"Writes", Json::array({Write(A, "Batched A"), Write(B, "Batched B")})}}, 2));
		const auto BatchHistory = Success(Call("GetTeamHistory", Json::object(), 2));
		Require(BatchHistory["Entries"].back()["Author"] == 2 && BatchHistory["Entries"].back()["ChangeCount"] == 2,
			"Prepared batches retain attribution through the existing history");
		const auto Page = Success(Call("PollChanges", {{"MaximumRecords", 2}}, 1));
		Require(Page["Records"].size() == 1 && Page["HasMore"] == true, "A page stops before a commit it cannot carry intact");
		const auto Batch = Success(Call("PollChanges", {{"MaximumRecords", 2}}, 1));
		Require(Batch["Records"].size() == 2 && Batch["HasMore"] == false, "The next page delivers the complete batch");
		const auto Independent = Success(Call("PollChanges", Json::object(), 2));
		Require(Independent["Records"].size() == 3 && Independent["HasMore"] == false,
			"Polling as one author does not consume another author's journal");
		Success(Call("GetSnapshot", Json::object(), 3));
		Success(Call("Undo", {{"ExpectedRevision", Revision()}}, 2));
		Rejects(Call("PollChanges", {{"MaximumRecords", 1}}, 3), "ResnapshotRequired");
		Rejects(Call("PollChanges", Json::object(), 3), "SnapshotRequired");
		Success(Call("GetSnapshot", Json::object(), 3));
		Require(Success(Call("PollChanges", Json::object(), 3))["Records"].empty(), "Replacement snapshot establishes a fresh anchor");

		auto &Journal = ChangeJournal::Get();
		const auto Capacity = Journal.GetCapacity();
		Journal.SetCapacity(1);
		Success(Call("GetSnapshot", Json::object(), 1));
		Success(Rename(A, "Eviction first", 1));
		Success(Rename(A, "Eviction second", 1));
		Rejects(Rename(A, "Evicted author write", 3), "ResnapshotRequired");
		Journal.SetCapacity(Capacity);
		Success(Call("GetSnapshot", Json::object(), 1));
		const auto Saved = Success(Call("SaveProject", Json::object(), 1));
		Require(Saved["Dirty"] == false && Saved["CurrentDestination"].is_null(), "Team save persists the authoritative project");
		Success(Call("OpenProject", {{"Root", Root.generic_string()}}));
		Rejects(Call("PollChanges", Json::object(), 1), "SnapshotRequired");
		const auto Reopened = Success(Call("GetSnapshot", Json::object(), 1));
		Require(Reopened["ProjectState"]["ProjectId"] == State["ProjectId"] &&
			Reopened["Snapshot"]["Cursor"]["Scope"] != Scope, "Reopen preserves project identity with a new authority scope");
		std::cout << "[TeamCreate:Authority] PASS\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[TeamCreate:Authority] FAIL " << Error.what() << '\n';
		return 1;
	}
}

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	int Checks = 0;
	void Check(bool Value, std::string_view Label) {
		if (!Value) throw std::runtime_error(std::string(Label));
		++Checks;
	}
	std::string Read(const std::filesystem::path &Path) {
		std::ifstream File(Path, std::ios::binary);
		return {std::istreambuf_iterator<char>(File), {}};
	}
	struct Client {
		EditorHost Host{"binding-test"};
		std::size_t Serial = 0;
		Json Call(std::string Method, Json Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(Json{{"Version", EditorHostProtocolVersion},
				{"RequestId", std::to_string(++Serial)}, {"SessionToken", "binding-test"},
				{"Method", Method}, {"Params", std::move(Parameters)}}.dump()));
		}
		Json Ok(std::string Method, Json Parameters = Json::object()) {
			auto Reply = Call(Method, std::move(Parameters));
			Check(Reply.value("Ok", false), Method + " rejected: " + Reply.dump());
			return Reply["Result"];
		}
		void Error(std::string Method, Json Parameters, std::string_view Code) {
			auto Reply = Call(Method, std::move(Parameters));
			Check(!Reply.value("Ok", false) && Reply["Error"]["Code"].get<std::string>() == Code,
				Method + " expected " + std::string(Code) + ": " + Reply.dump());
		}
	};
	Json FindObject(const Json &Snapshot, std::string_view Name) {
		for (const auto &Object : Snapshot["Snapshot"]["Objects"])
			if (Object.value("Name", "") == Name) return Object;
		throw std::runtime_error("Missing fixture object: " + std::string(Name));
	}
	Json Create(Client &Api, const Json &Parent, std::string_view Class, std::string_view Name) {
		return Api.Ok("CreateInstance", {{"Parent", Parent}, {"Name", Name},
			{"ClassSchemaId", SchemaId::FromNativeName("Engine", Class).ToString()}, {"DefinitionVersion", 1}})["Object"];
	}
	void TestNativeIdentityAndPersistence() {
		auto World = std::make_shared<DataModel>(); World->SetArchivable(true);
		auto ScriptValue = std::make_shared<Script>(); ScriptValue->SetArchivable(true);
		ScriptValue->SetName("Same name"); ScriptValue->SetSource("-- original"); ScriptValue->SetParent(World);
		auto Module = std::make_shared<ModuleScript>(); Module->SetArchivable(true); Module->SetParent(World);
		Check(ProjectId::Parse(ScriptValue->GetSourceId()).has_value() && ScriptValue->GetSourceId() != Module->GetSourceId(),
			"new Scripts have distinct canonical resource identities");
		const auto *Property = ScriptValue->FindProperty("SourceId");
		Check(Property && Property->PersistencePolicy == InstanceProperty::Persistence::Saved &&
			Property->ReplicationPolicy == InstanceProperty::Replication::None &&
			Property->ReadPermission == Enums::Permission::Engine && Property->WritePermission == Enums::Permission::Engine,
			"SourceId is saved, nonreplicated and native-only schema state");
		Check(ScriptValue->ApplyPropertyMutation("SourceId", ProjectId::New().ToString(), Enums::Permission::None,
			ScriptSecurityContext::StudioCoreUi()) == MutationStatus::Unauthorized, "ordinary mutation cannot choose SourceId");
		bool Rejected = false;
		try { ScriptValue->SetSourceId(ProjectId::New().ToString()); } catch (const std::invalid_argument &) { Rejected = true; }
		Check(Rejected, "adopted Script identity cannot change through native setter");
		auto Clone = std::dynamic_pointer_cast<Script>(ScriptValue->Clone());
		Check(Clone && Clone->GetSourceId() != ScriptValue->GetSourceId() && Clone->GetSource() == ScriptValue->GetSource(),
			"Instance.Clone preserves source but creates a fresh logical resource");
		Clone->SetParent(World);
		std::shared_ptr<Instance> Root = World;
		auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		std::istringstream Input(Encoded);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		Check(Loaded.Ok && Loaded.Instance, "persistent resource identity roundtrip succeeds");
		auto LoadedScript = std::dynamic_pointer_cast<Script>(Loaded.Instance->Children[0]);
		Check(LoadedScript && LoadedScript->GetSourceId() == ScriptValue->GetSourceId() && LoadedScript->WasSourceIdLoaded() &&
			LoadedScript->GetObjectId() != ScriptValue->GetObjectId(), "reopen preserves SourceId and allocates a fresh live ObjectId");
		auto Document = Json::parse(Encoded);
		for (const auto &Invalid : {std::string(32, '0'), std::string(32, 'A'), std::string("short")}) {
			auto Bad = Document; Bad["Children"][0]["Properties"]["SourceId"] = {{"String", Invalid}};
			std::istringstream Stream(Bad.dump());
			Check(!InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Stream).Ok,
				"malformed persisted SourceId is rejected transactionally");
		}
		auto Duplicate = Document;
		Duplicate["Children"][1]["Properties"]["SourceId"] = Duplicate["Children"][0]["Properties"]["SourceId"];
		std::istringstream DuplicateInput(Duplicate.dump());
		auto BadState = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, DuplicateInput);
		Check(!BadState.Ok && !BadState.Instance, "duplicate persisted resource identities reject the complete candidate");
		Loaded.Instance->Destroy(); World->Destroy();
	}
	void TestEditorProtocol(const std::filesystem::path &Root) {
		std::filesystem::create_directories(Root / ".gargantuan");
		{
			std::ofstream PreRun(Root / ".gargantuan/prerun.luau", std::ios::binary);
			PreRun << "Schema:RegisterClass({Namespace='Game',Name='SourceLabel',Version=1,Base='Engine.Folder',"
				"Properties={SourceId={Type='String',Default='ordinary label'}}})";
			Check(bool(PreRun), "non-Script custom property fixture written");
		}
		{
			auto World = std::make_shared<DataModel>(); World->SetArchivable(true);
			auto ScriptValue = std::make_shared<Script>(); ScriptValue->SetArchivable(true); ScriptValue->SetName("Legacy");
			ScriptValue->SetSource("-- saved legacy source"); ScriptValue->SetParent(World);
			std::shared_ptr<Instance> Base = World;
			auto Legacy = Json::parse(InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Base));
			Legacy["Children"][0]["Properties"].erase("SourceId");
			std::ofstream File(Root / ".gargantuan/project.instance.json", std::ios::binary); File << Legacy.dump();
			Check(bool(File), "legacy fixture written");
		}
		const auto LegacyBytes = Read(Root / ".gargantuan/project.instance.json");
		Client Api;
		auto Handshake = Api.Ok("Handshake");
		Check(std::ranges::find(Handshake["Capabilities"], Json("ScriptDocumentBinding")) != Handshake["Capabilities"].end(),
			"optional binding capability advertised");
		Api.Ok("OpenProject", {{"Root", Root.generic_string()}});
		{
			const auto Class = SchemaId::FromCustomClassName("Game", "SourceLabel");
			const auto *Definition = InstanceClassRegistry::GetDefinitionBySchemaId(Class);
			Check(Definition != nullptr, "non-Script custom SourceId class registered");
			auto Value = InstanceClassRegistry::Construct(*Definition); Value->SetArchivable(true);
			Check(Value->ApplyCustomClassPropertyMutation(Class, 1, "SourceId", std::string("not a Script identity"),
				ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success, "ordinary custom SourceId accepts arbitrary label");
			auto Copy = Value->Clone();
			Check(std::get<std::string>(Copy->GetCustomClassPropertyValue(Class, "SourceId", ScriptSecurityContext::CoreTrusted())) ==
				"not a Script identity", "non-Script SourceId custom property is preserved through Clone");
			Copy->Destroy(); Value->Destroy();
		}
		auto Snapshot = Api.Ok("GetSnapshot");
		auto Object = FindObject(Snapshot, "Legacy")["Id"];
		auto Receipt = Api.Ok("GetScriptBinding", {{"Object", Object}});
		auto Binding = Receipt["Binding"];
		Check(Receipt["IdentityOrigin"] == "Generated" && Binding.size() == 5 && Receipt["Object"] == Object,
			"legacy missing identity is generated in memory and explicitly identified");
		Check(Read(Root / ".gargantuan/project.instance.json") == LegacyBytes, "binding read never rewrites legacy scene");
		const auto BeforeReads = Api.Ok("GetProjectState");
		Check(Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Object"] == Object, "binding resolves current authoring object");
		for (int Index = 0; Index < 8; ++Index) Api.Ok("ResolveScriptBinding", {{"Binding", Binding}});
		Check(Api.Ok("GetProjectState") == BeforeReads, "binding reads leave revision, history and dirty state unchanged");
		Api.Ok("OpenProject", {{"Root", Root.generic_string()}});
		Api.Ok("GetSnapshot");
		Api.Error("ResolveScriptBinding", {{"Binding", Binding}}, "ResourceNotFound");
		Object = FindObject(Api.Ok("GetSnapshot"), "Legacy")["Id"];
		Receipt = Api.Ok("GetScriptBinding", {{"Object", Object}}); Binding = Receipt["Binding"];
		Api.Ok("SaveProject");
		Check(Read(Root / ".gargantuan/project.instance.json") != LegacyBytes, "explicit Save persists legacy SourceId");
		const auto OldScope = Receipt["Scope"];
		Api.Ok("OpenProject", {{"Root", Root.generic_string()}}); Api.Ok("GetSnapshot");
		Receipt = Api.Ok("ResolveScriptBinding", {{"Binding", Binding}});
		Check(Receipt["Object"] != Object && Receipt["Scope"] != OldScope && Receipt["Binding"] == Binding &&
			Receipt["IdentityOrigin"] == "Loaded", "saved binding survives replacement session with fresh live identities");
		Api.Error("GetScriptBinding", {{"Object", Object}}, "StaleObject"); Object = Receipt["Object"];
		Api.Ok("SetProperty", {{"Object", Object}, {"ClassSchemaId", SchemaId::FromNativeName("Engine", "Script").ToString()},
			{"ClassDefinitionVersion", 1}, {"DeclaringClassSchemaId", SchemaId::FromNativeName("Engine", "Instance").ToString()},
			{"DeclaringDefinitionVersion", 1}, {"Property", "Name"}, {"Value", {{"Type", "String"}, {"Value", "Renamed"}}}});
		Check(Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Object"] == Object, "rename cannot retarget a binding");
		auto Parent = Create(Api, FindObject(Api.Ok("GetSnapshot"), "Workspace")["Id"], "Folder", "Target");
		Api.Ok("ReparentInstance", {{"Object", Object}, {"Parent", Parent}});
		Check(Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Object"] == Object, "reparent preserves durable identity");
		const auto Source = Api.Ok("GetScriptSource", {{"Object", Object}});
		Api.Ok("SetScriptSource", {{"Object", Object}, {"ExpectedSourceVersion", Source["SourceVersion"]}, {"Source", "-- edited source"}});
		Check(Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Binding"] == Binding, "source edits preserve resource identity");
		Api.Ok("Undo");
		auto Copy = Api.Ok("DuplicateInstance", {{"Object", Object}})["Object"];
		auto CopyBinding = Api.Ok("GetScriptBinding", {{"Object", Copy}})["Binding"];
		Check(CopyBinding["SourceId"] != Binding["SourceId"], "authoring duplicate receives a fresh resource identity");
		Api.Ok("Undo"); Api.Error("ResolveScriptBinding", {{"Binding", CopyBinding}}, "ResourceNotFound");
		Api.Ok("Redo");
		auto Redone = Api.Ok("ResolveScriptBinding", {{"Binding", CopyBinding}})["Object"];
		Check(Redone != Copy && Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Object"] == Object,
			"Redo restores duplicate identity without borrowing the original resource identity");
		Api.Ok("DestroyInstance", {{"Object", Object}});
		Api.Error("ResolveScriptBinding", {{"Binding", Binding}}, "ResourceNotFound");
		auto SameName = Create(Api, Parent, "Script", "Renamed");
		Check(Api.Ok("GetScriptBinding", {{"Object", SameName}})["Binding"]["SourceId"] != Binding["SourceId"],
			"same-name replacement does not reuse a deleted resource identity");
		Api.Error("ResolveScriptBinding", {{"Binding", Binding}}, "ResourceNotFound");
		Api.Ok("Undo"); Api.Ok("Undo");
		Object = Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Object"];
		Check(Api.Ok("GetScriptSource", {{"Object", Object}})["Source"] == "-- saved legacy source",
			"Undo delete restores the same logical resource through ordinary source authority");
		for (const auto &Bad : {Json::object(), Json{{"Binding", Binding}, {"Path", "Renamed"}}, Json{{"Binding", "invalid"}}})
			Api.Error("ResolveScriptBinding", Bad, "MalformedRequest");
		for (const auto &Field : {"Version", "ProjectId", "SourceId", "ClassSchemaId", "ClassDefinitionVersion"}) {
			auto Bad = Binding; Bad.erase(Field); Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		}
		for (const auto &Field : {"ProjectId", "SourceId", "ClassSchemaId"}) {
			auto Bad = Binding; Bad[Field] = std::string(32, '0'); Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		}
		for (const auto &Value : {Json(0), Json(-1), Json(2), Json("1")}) {
			auto WrongVersion = Binding; WrongVersion["Version"] = Value;
			Api.Error("ResolveScriptBinding", {{"Binding", WrongVersion}}, "MalformedRequest");
		}
		auto Bad = Binding; Bad["SourceId"] = std::string(4096, 'a');
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		Bad = Binding; Bad["Path"] = "Renamed";
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		auto Upper = Binding["ClassSchemaId"].get<std::string>();
		std::ranges::transform(Upper, Upper.begin(), [](unsigned char Character) { return static_cast<char>(std::toupper(Character)); });
		Bad = Binding; Bad["ClassSchemaId"] = Upper;
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		Bad = Binding; Bad["ClassDefinitionVersion"] = 4294967296ull;
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "MalformedRequest");
		Bad = Binding; Bad["ClassSchemaId"] = SchemaId::FromNativeName("Engine", "ModuleScript").ToString();
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "IncompatibleBinding");
		Bad = Binding; Bad["ProjectId"] = ProjectId::New().ToString();
		Api.Error("ResolveScriptBinding", {{"Binding", Bad}}, "StaleProject");
		Api.Error("GetScriptBinding", {{"Object", Parent}}, "NotScript");
		Api.Error("GetScriptBinding", {{"Object", Object}, {"Scope", Receipt["Scope"]}}, "MalformedRequest");
		const auto BeforeDeniedState = Api.Ok("GetProjectState");
		const auto BeforeDeniedIdentity = Api.Ok("GetScriptBinding", {{"Object", Object}});
		Api.Ok("GetSnapshot");
		Check(Api.Ok("PollChanges")["Records"].empty(), "SourceId denial starts from a clean journal cursor");
		const auto DeniedIdentityWrite = Api.Call("SetProperty", {{"Object", Object}, {"ClassSchemaId", SchemaId::FromNativeName("Engine", "Script").ToString()},
			{"ClassDefinitionVersion", 1}, {"DeclaringClassSchemaId", SchemaId::FromNativeName("Engine", "LuaSourceContainer").ToString()},
			{"DeclaringDefinitionVersion", 1}, {"Property", "SourceId"},
			{"Value", {{"Type", "String"}, {"Value", ProjectId::New().ToString()}}}});
		Check(!DeniedIdentityWrite.value("Ok", false) && DeniedIdentityWrite["Error"]["Code"] == "ReadOnly",
			"Studio rejects SourceId at the frozen noneditable property gate: " + DeniedIdentityWrite.dump());
		std::cout << "[Editor:ScriptBinding] SourceIdWrite " << DeniedIdentityWrite.dump() << '\n';
		Check(Api.Ok("GetScriptBinding", {{"Object", Object}}) == BeforeDeniedIdentity,
			"rejected SourceId write leaves the exact resource identity and revision unchanged");
		Check(Api.Ok("GetProjectState") == BeforeDeniedState,
			"rejected SourceId write leaves dirty state and complete retained history unchanged");
		Check(Api.Ok("PollChanges")["Records"].empty(), "rejected SourceId write emits no journal records");
		std::size_t ScriptCount = 2;
		for (const std::size_t Count : {2u, 32u, 128u}) {
			while (ScriptCount < Count) { Create(Api, Parent, "Script", "ResolutionFixture"); ++ScriptCount; }
			std::vector<double> Samples;
			std::size_t MaximumReplyBytes = 0;
			for (int Index = 0; Index < 72; ++Index) {
				const auto Start = std::chrono::steady_clock::now();
				auto Reply = Api.Call("ResolveScriptBinding", {{"Binding", Binding}});
				const auto Microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
				Check(Reply.value("Ok", false) && Reply["Result"]["Object"] == Object, "bounded lookup workload preserves exact identity");
				if (Index >= 8) { Samples.push_back(Microseconds); MaximumReplyBytes = std::max(MaximumReplyBytes, Reply.dump().size()); }
			}
			std::ranges::sort(Samples);
			std::cout << "[Editor:ScriptBinding] Resolution Scripts=" << Count << " Samples=64 Warmups=8 P50Us=" << Samples[31]
				<< " P95Us=" << Samples[60] << " P99Us=" << Samples[63] << " MaxReplyBytes=" << MaximumReplyBytes
				<< " Boundary=InProcessHostJsonRead Allocations=NOT_MEASURED\n";
		}
		Api.Ok("SaveProjectAs", {{"Destination", (Root.parent_path() / "SavedAs").generic_string()}});
		Check(Api.Ok("ResolveScriptBinding", {{"Binding", Binding}})["Binding"] == Binding, "Save As preserves existing project-lineage identity");
		Api.Ok("CreateProject", {{"Destination", (Root.parent_path() / "DifferentProject").generic_string()}, {"Name", "Different"}});
		Api.Error("ResolveScriptBinding", {{"Binding", Binding}}, "StaleProject");
		EditorHost Denied("denied", ScriptSecurityContext{});
		for (const auto &Method : {"GetScriptBinding", "ResolveScriptBinding"}) {
			auto Reply = Json::parse(Denied.HandleRequest(Json{{"Version", EditorHostProtocolVersion}, {"RequestId", "1"},
				{"SessionToken", "denied"}, {"Method", Method}, {"Params", Json::object()}}.dump()));
			Check(Reply["Error"]["Code"] == "Unauthorized", "binding capability independently enforced");
		}
	}
}
int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestNativeIdentityAndPersistence();
		const auto Root = std::filesystem::temp_directory_path() / ("gargantuan-script-bindings-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		TestEditorProtocol(Root / "Project");
		std::cout << "[Editor:ScriptBinding] PASS Checks=" << Checks << " Fixture=" << Root.generic_string() << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Editor:ScriptBinding] " << Error.what() << '\n'; return 1;
	}
}

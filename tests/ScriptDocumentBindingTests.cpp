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
#include "../src/serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

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
	void TestPlaySourceLocation(const std::filesystem::path &Root) {
		Check(SDL_Init(0), "initialize SDL for local Play");
		struct ScopeExit { ~ScopeExit() { SDL_Quit(); } } Cleanup;
		Client Api;
		const auto Hello = Api.Ok("Handshake");
		Check(Hello["PlayScriptFailureLocationVersion"] == 1 &&
			std::ranges::find(Hello["Capabilities"], Json("PlayScriptFailureLocation")) != Hello["Capabilities"].end(),
			"local Play source location version advertised");
		Api.Ok("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Play Source Location"}});
		const auto Parent = FindObject(Api.Ok("GetSnapshot"), "Workspace")["Id"];
		const auto Failing = Create(Api, Parent, "Script", "RepeatedName");
		const auto Nested = Create(Api, Parent, "Script", "RepeatedName");
		const auto Commit = [&](const Json &Object, const std::string &Text) {
			const auto Current = Api.Ok("GetScriptSource", {{"Object", Object}});
			Api.Ok("SetScriptSource", {{"Object", Object}, {"ExpectedSourceVersion", Current["SourceVersion"]}, {"Source", Text}});
		};
		Commit(Failing, "-- First authoring revision\nreturn nil");
		Commit(Failing, "warn('RepeatedName:1999: spoofed print')\nerror('RepeatedName:9999: actual failure text', 0)");
		Commit(Nested, "local function throwError()\nerror('nested failure', 0)\nend\nthrowError()");
		const auto Authored = Api.Ok("GetScriptBinding", {{"Object", Failing}});
		const auto Source = Api.Ok("GetScriptSource", {{"Object", Failing}});
		Api.Error("StartPlaySession", {{"IncludeScriptFailureLocations", "true"}}, "MalformedRequest");
		auto Start = Api.Ok("StartPlaySession");
		for (const auto &Entry : Start["Diagnostics"])
			Check(!Entry.contains("Source"), "legacy Play launch omits authored source locations");
		Api.Ok("StopPlaySession", {{"PlaySessionId", Start["PlaySessionId"]}});
		Start = Api.Ok("StartPlaySession", {{"IncludeScriptFailureLocations", true}});
		Json Location;
		std::size_t Located = 0;
		const auto ReadRows = [&](const Json &Packet) {
			for (const auto &Entry : Packet["Diagnostics"]) {
				if (!Entry.contains("Source")) continue;
				Check(Entry["Severity"] == "Error" && Entry["Message"].get<std::string>().find("actual failure text") != std::string::npos,
					"print, nested functions, and name collisions cannot fabricate an authored location");
				Location = Entry["Source"];
				++Located;
			}
		};
		ReadRows(Start);
		Api.Host.PumpPlaySessionForTesting();
		ReadRows(Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", Start["PlaySessionId"]}}));
		ReadRows(Api.Ok("StopPlaySession", {{"PlaySessionId", Start["PlaySessionId"]}}));
		Check(Located == 1 && Location["Binding"] == Authored["Binding"] && Location["Scope"] == Authored["Scope"] &&
			Location["Object"] == Failing &&
			Location["SourceVersion"] == Source["SourceVersion"] && Location["LaunchAuthoritativeRevision"] == Start["LaunchAuthoritativeRevision"] &&
			Location["Line"] == 2, "trusted VM line and exact authored launch binding survive Stop");
		const Json Resolve{{"Binding", Location["Binding"]}, {"ExpectedScope", Location["Scope"]}, {"ExpectedObject", Location["Object"]},
			{"ExpectedSourceVersion", Location["SourceVersion"]}};
		Check(Api.Ok("ResolveScriptBinding", Resolve)["Object"] == Failing, "post-Stop location resolves exact live source");
		auto WrongObject = Resolve;
		WrongObject["ExpectedObject"] = Nested;
		Api.Error("ResolveScriptBinding", WrongObject, "StaleObject");
		Commit(Failing, "-- Revised after Stop");
		Api.Error("ResolveScriptBinding", Resolve, "SourceVersionConflict");
		Api.Ok("SaveProject");
		Api.Ok("OpenProject", {{"Root", Root.generic_string()}});
		Api.Error("ResolveScriptBinding", Resolve, "StaleProject");
	}

	void TestPlayRootDebugger(const std::filesystem::path &Root) {
		Check(SDL_Init(0), "initialize SDL for root debugger");
		struct ScopeExit { ~ScopeExit() { SDL_Quit(); } } Cleanup;
		Client Api;
		Check(Api.Ok("Handshake")["PlayRootDebugVersion"] == 1, "local root debugger advertised");
		Api.Ok("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Local Root Debugger"}});
		const auto Snapshot = Api.Ok("GetSnapshot");
		const auto Parent = FindObject(Snapshot, "Workspace")["Id"];
		const auto StarterGui = FindObject(Snapshot, "StarterGui")["Id"];
		const auto Screen = Create(Api, StarterGui, "ScreenGui", "PausedHUD");
		Api.Ok("CreateInstance", {{"Parent", Screen}, {"Name", "PauseIndicator"},
			{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Frame").ToString()}, {"DefinitionVersion", 1},
			{"InitialProperties", Json::array({Json{
				{"DeclaringClassSchemaId", SchemaId::FromNativeName("Engine", "GuiObject").ToString()},
				{"DeclaringDefinitionVersion", 1}, {"Property", "Size"},
				{"Value", JsonCodec::EncodeWireValue(WireUDim2{{0, 80}, {0, 40}})},
			}})}});
		const auto Target = Create(Api, Parent, "Script", "SteppedRoot");
		const auto OldSource = Api.Ok("GetScriptSource", {{"Object", Target}});
		Api.Ok("SetScriptSource", {{"Object", Target}, {"ExpectedSourceVersion", OldSource["SourceVersion"]},
			{"Source", "print('root-first')\ntask.delay(0.5, function() print('after-pause-timer') end)\nprint('root-last')"}});
		const auto Source = Api.Ok("GetScriptSource", {{"Object", Target}});
		const auto Authoring = Api.Ok("GetProjectState");
		const Json RootOption{{"Object", Target}, {"SourceVersion", Source["SourceVersion"]}};
		Api.Error("StartPlaySession", {{"DebugRoot", {{ "Object", Target }, {"SourceVersion", OldSource["SourceVersion"]}}}},
			"DebugSourceUnavailable");
		Api.Error("StartPlaySession", {{"DebugRoot", RootOption}, {"PersistenceMode", "Disposable"}}, "MalformedRequest");
		const auto Open = [&] {
			Api.Ok("OpenViewportTransport", {{"Transport", "LatestFrameMailbox"},
				{"Version", LatestFrameMailboxLayout::Version}, {"PixelFormat", "BGRA8"}});
		};
		if (LatestFrameMailbox::IsSupported()) {
			Open();
			Api.Ok("ConfigureViewport", {{"Width", 640u}, {"Height", 480u}});
		}
		const auto Start = Api.Ok("StartPlaySession", {{"DebugRoot", RootOption}});
		const auto Id = Start["PlaySessionId"];
		auto Pause = Start["DebugPause"];
		const auto HasMessage = [](const Json &Packet, std::string_view Message) {
			return std::ranges::any_of(Packet["Diagnostics"], [&](const Json &Entry) {
				return Entry.value("Message", "").find(Message) != std::string::npos;
			});
		};
		Check(Pause["Serial"] == 1 && Pause["CanResume"] == true && Pause["Source"]["Line"] == 1 &&
			Pause["Source"]["Object"] == Target && !HasMessage(Start, "root-first"),
			"entry pause occurs before the selected Script executes its first line");
		if (LatestFrameMailbox::IsSupported()) {
			std::vector<std::uint8_t> Pixels;
			RenderPublicationPtr Captured;
			Api.Host.SetViewportCaptureForTesting([&](RenderPublicationPtr Publication) {
				Captured = Publication;
				const auto Width = Publication->Frame.ViewportWidth;
				const auto Height = Publication->Frame.ViewportHeight;
				Pixels.assign(static_cast<std::size_t>(Width) * Height * 4, 0);
				return EditorViewportCaptureView{.Width = Width, .Height = Height, .BgraPixels = Pixels};
			});
			const auto StartPresentation = [&] {
				Api.Ok("StartViewportPresentation", {{"FrameRateMode", "Adaptive"}});
			};
			std::optional<float> InitialUiRight;
			const auto CheckFrame = [&](std::string_view Stage, std::uint32_t Width, std::uint32_t Height) {
				const auto Previous = Api.Ok("GetViewportPresentationState");
				Captured.reset();
				Api.Host.PumpViewportPresentationForTesting();
				const auto Frame = Api.Ok("GetViewportPresentationState");
				Check(Captured && Captured->FullResync && Captured->Frame.ViewportWidth == Width &&
					Captured->Frame.ViewportHeight == Height && Frame["Mode"] == "Play" &&
					Frame["PlaySessionId"] == Id && Frame["ProducedFrames"] > Previous["ProducedFrames"] &&
					Frame["PresentationFailures"] == Previous["PresentationFailures"],
					std::string(Stage) + ": paused presentation must publish a full frame for the exact Play session and dimensions. " +
						Frame.dump());
				const auto &Ui = Captured->GetUi();
				float Right = 0;
				for (const auto &Batch : Ui.Batches)
					for (const auto &Vertex : Batch.Vertices) Right = std::max(Right, Vertex.Position.x);
				Check(Ui.ViewportWidth == Width && Ui.ViewportHeight == Height && Right > 0,
					std::string(Stage) + ": frozen HUD geometry fits the target presentation");
				if (!InitialUiRight) InitialUiRight = Right;
				Check(std::abs(Right - *InitialUiRight * (static_cast<float>(Width) / 640.0f)) < 0.05f,
					std::string(Stage) + ": frozen HUD geometry scales from the original viewport");
				Check(Api.Ok("GetPlayRootDebugState", {{"PlaySessionId", Id}})["DebugPause"] == Pause &&
					!HasMessage(Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", Id}}), "root-first"),
					"paused presentation does not resume scripts or replace the pause");
			};
			StartPresentation();
			CheckFrame("Entry", 640, 480);
			Api.Ok("ConfigureViewport", {{"Width", 480u}, {"Height", 360u}});
			CheckFrame("Resize", 480, 360);
			Api.Ok("CloseViewportTransport");
			Open();
			StartPresentation();
			CheckFrame("Reattach", 480, 360);
			Api.Ok("CloseViewportTransport");
			Api.Host.SetViewportCaptureForTesting({});
		}
		Api.Host.PumpPlaySessionForTesting();
		Check(Api.Ok("GetPlayRootDebugState", {{"PlaySessionId", Id}})["DebugPause"] == Pause,
			"ordinary simulation pump leaves the paused Script untouched");
		Api.Error("ResumePlayRootDebug", {{"PlaySessionId", Id}, {"PauseSerial", 2}, {"Action", "NextLine"}},
			"DebugPauseUnavailable");
		Api.Error("ResumePlayRootDebug", {{"PlaySessionId", "999"}, {"PauseSerial", 1}, {"Action", "Continue"}},
			"StalePlaySession");
		Api.Error("ResumePlayRootDebug", {{"PlaySessionId", Id}, {"PauseSerial", 1}, {"Action", "Eval"}}, "MalformedRequest");
		Api.Error("SaveProject", Json::object(), "PlaySessionActive");
		const auto Step = [&](std::string Action) {
			return Api.Ok("ResumePlayRootDebug", {{"PlaySessionId", Id}, {"PauseSerial", Pause["Serial"]}, {"Action", Action}})["DebugPause"];
		};
		Pause = Step("NextLine");
		Check(Pause["Serial"] == 2 && Pause["Source"]["Line"] == 2 &&
			HasMessage(Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", Id}}), "root-first"), "next line executes only its predecessor");
		Pause = Step("NextLine");
		Check(Pause["Serial"] == 3 && Pause["Source"]["Line"] == 3, "next line preserves the same root");
		std::this_thread::sleep_for(std::chrono::milliseconds(600));
		Api.Host.PumpPlaySessionForTesting();
		Check(!HasMessage(Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", Id}}), "after-pause-timer"),
			"delayed task remains parked while the debugger owns the runtime");
		Check(Step("Continue").is_null(), "continue retires the paused root");
		Api.Host.PumpPlaySessionForTesting();
		const auto Continued = Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", Id}});
		Check(HasMessage(Continued, "root-last") && !HasMessage(Continued, "after-pause-timer"),
			"continue executes the terminal line and preserves remaining delay time");
		Api.Ok("StopPlaySession", {{"PlaySessionId", Id}});
		const auto StopPaused = Api.Ok("StartPlaySession", {{"DebugRoot", RootOption}});
		Check(StopPaused["DebugPause"]["Source"]["Line"] == 1, "each launch has a fresh pause");
		Api.Ok("StopPlaySession", {{"PlaySessionId", StopPaused["PlaySessionId"]}});
		Api.Error("GetPlayRootDebugState", {{"PlaySessionId", StopPaused["PlaySessionId"]}}, "StalePlaySession");
		Check(Api.Ok("GetProjectState") == Authoring, "runtime stepping never mutates authoritative project state or history");

		const auto Revision = Api.Ok("SetScriptSource", {{"Object", Target}, {"ExpectedSourceVersion", Source["SourceVersion"]},
			{"Source", "print('root-will-fail')\nerror('Forged.luau:99999: continued root', 0)"}})["SourceVersion"];
		const auto FailureStart = Api.Ok("StartPlaySession", {{"DebugRoot", {{"Object", Target}, {"SourceVersion", Revision}}}});
		const auto FailureId = FailureStart["PlaySessionId"];
		Check(FailureStart["DebugPause"]["Source"]["Line"] == 1, "failing root is paused at entry");
		Api.Ok("ResumePlayRootDebug", {{"PlaySessionId", FailureId},
			{"PauseSerial", FailureStart["DebugPause"]["Serial"]}, {"Action", "Continue"}});
		const auto Failure = Api.Ok("PollPlayDiagnostics", {{"PlaySessionId", FailureId}});
		Check(std::ranges::any_of(Failure["Diagnostics"], [&](const Json &Entry) {
			return Entry.value("Message", "").find("continued root") != std::string::npos &&
				Entry.contains("Source") && Entry["Source"]["Object"] == Target &&
				Entry["Source"]["Line"] == 2 && Entry["Source"]["SourceVersion"] == Revision;
		}), "continued root error retains trusted authored identity and line");
		Api.Ok("StopPlaySession", {{"PlaySessionId", FailureId}});
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
		TestPlaySourceLocation(Root / "PlayLocation");
		TestPlayRootDebugger(Root / "PlayDebugger");
		std::cout << "[Editor:ScriptBinding] PASS Checks=" << Checks << " Fixture=" << Root.generic_string() << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Editor:ScriptBinding] " << Error.what() << '\n'; return 1;
	}
}

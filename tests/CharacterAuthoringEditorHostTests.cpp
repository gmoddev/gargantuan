#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
	using Json = nlohmann::ordered_json;

	void Check(bool Condition, std::string Message) {
		if (!Condition) throw std::runtime_error("[Character:EditorHost] " + std::move(Message));
	}

	const Json &FindDefinition(const Json &Schema, std::string_view Name) {
		for (const auto &Definition : Schema.at("Result").at("Definitions"))
			if (Definition.at("CanonicalName").get<std::string>() == Name) return Definition;
		throw std::runtime_error("[Character:EditorHost] Missing schema " + std::string(Name));
	}

	Json FindObject(const Json &Snapshot, std::string_view Name) {
		for (const auto &Object : Snapshot.at("Result").at("Snapshot").at("Objects"))
			if (Object.at("Name").get<std::string>() == Name) return Object.at("Id");
		throw std::runtime_error("[Character:EditorHost] Missing object " + std::string(Name));
	}

	bool ReadBoolean(const Json &Snapshot, std::string_view Name, std::string_view Property) {
		for (const auto &Object : Snapshot.at("Result").at("Snapshot").at("Objects"))
			if (Object.at("Name").get<std::string>() == Name)
				return Object.at("Properties").at(std::string(Property)).at("Value").get<bool>();
		throw std::runtime_error("[Character:EditorHost] Missing boolean property " + std::string(Property));
	}

	bool HasIssue(const Json &Inspection, std::string_view Code) {
		for (const auto &Issue : Inspection.at("Issues"))
			if (Issue.at("Code").get<std::string>() == Code) return true;
		return false;
	}
}

int main() {
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-character-editorhost-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); SDL_Quit(); }
	} CleanupValue{Root};
	try {
#ifdef _WIN32
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
		Check(SDL_Init(0), "SDL initialization failed");
		BootstrapNativeRuntimeSchema();
		EditorHost Host("character-authoring-test");
		std::size_t Sequence = 0;
		auto Call = [&](std::string Method, Json Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", 1}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", "character-authoring-test"},
				{"Method", std::move(Method)}, {"Params", std::move(Parameters)},
			}.dump()));
		};
		auto Success = [&](std::string Method, Json Parameters = Json::object()) {
			auto Response = Call(std::move(Method), std::move(Parameters));
			Check(Response.at("Ok") == true, "RPC rejected: " + Response.dump());
			return Response;
		};
		auto Error = [&](std::string Method, Json Parameters, std::string_view Code) {
			auto Response = Call(std::move(Method), std::move(Parameters));
			Check(Response.at("Ok") == false && Response.at("Error").at("Code") == Code,
				"Wrong RPC rejection: " + Response.dump());
		};

		EditorHost DeniedHost("character-denied-test", ScriptSecurityContext{});
		const auto Denied = Json::parse(DeniedHost.HandleRequest(Json{
			{"Version", 1}, {"RequestId", "denied"}, {"SessionToken", "character-denied-test"},
			{"Method", "GetCharacterComposition"}, {"Params", Json::object()},
		}.dump()));
		Check(Denied.at("Ok") == false && Denied.at("Error").at("Code") == "Unauthorized",
			"ReadDataModel is required before project or identity access");
		Error("GetCharacterComposition", Json::object(), "NoProjectLoaded");
		Success("CreateProject", {{"Destination", Root.generic_string()}, {"Name", "Character Authoring"}});
		Error("GetCharacterComposition", Json::object(), "SnapshotRequired");
		const auto Handshake = Success("Handshake").at("Result");
		bool HasComposition = false, HasBlend = false;
		for (const auto &Capability : Handshake.at("Capabilities")) {
			HasComposition |= Capability == "CharacterComposition";
			HasBlend |= Capability == "AnimationPreviewBlend";
		}
		Check(HasComposition && HasBlend && Handshake.at("CharacterCompositionVersion") == 1 &&
			Handshake.at("AnimationPreviewBlendVersion") == 1,
			"Handshake advertises both versioned creator capabilities");

		const auto Schema = Success("GetSchema");
		const auto CharacterSchema = FindDefinition(Schema, "Engine.Character");
		const auto KinematicSchema = FindDefinition(Schema, "Engine.KinematicCharacter");
		const auto PartSchema = FindDefinition(Schema, "Engine.Part");
		const auto MeshSchema = FindDefinition(Schema, "Engine.MeshPart");
		const auto AnimatorSchema = FindDefinition(Schema, "Engine.Animator");
		const auto AttachmentSchema = FindDefinition(Schema, "Engine.Attachment");
		const auto PlayersSchema = FindDefinition(Schema, "Engine.Players");
		const auto Snapshot = Success("GetSnapshot");
		const auto Scope = Snapshot.at("Result").at("Snapshot").at("Cursor").at("Scope");
		const auto Workspace = FindObject(Snapshot, "Workspace");
		const auto Players = FindObject(Snapshot, "Players");
		Check(Success("GetProjectState").at("Result").at("AuthoritativeRevision") == 1,
			"Canonical player policy root is established before initial project revision");
		for (const auto &Object : Snapshot.at("Result").at("Snapshot").at("Objects"))
			Check(Object.at("ClassName") != "Player" && Object.at("ClassName") != "Script",
				"Authoring service establishment does not initialize gameplay players or scripts");
		auto Create = [&](const Json &Definition, const Json &Parent, std::string Name) {
			return Success("CreateInstance", {
				{"ClassSchemaId", Definition.at("SchemaId")},
				{"DefinitionVersion", Definition.at("DefinitionVersion")},
				{"Parent", Parent}, {"Name", std::move(Name)},
			}).at("Result").at("Object");
		};
		auto Write = [&](const Json &Object, const Json &Concrete, const Json &Declaring,
			std::string Property, Json Value) {
			return Call("SetProperty", {
				{"Object", Object}, {"ClassSchemaId", Concrete.at("SchemaId")},
				{"ClassDefinitionVersion", Concrete.at("DefinitionVersion")},
				{"DeclaringClassSchemaId", Declaring.at("SchemaId")},
				{"DeclaringDefinitionVersion", Declaring.at("DefinitionVersion")},
				{"Property", std::move(Property)}, {"Value", std::move(Value)},
			});
		};
		auto Inspect = [&](const Json &Object, const Json &CurrentScope) {
			return Call("GetCharacterComposition", {
				{"CharacterCompositionVersion", 1}, {"Scope", CurrentScope}, {"Object", Object},
			});
		};
		for (const auto Property : {"CharacterAutoLoads", "DefaultControllerEnabled", "DefaultCameraEnabled"}) {
			Check(ReadBoolean(Snapshot, "Players", Property), "Player policy starts with its declared default");
			Check(Write(Players, PlayersSchema, PlayersSchema, Property,
				{{"Type", "Bool"}, {"Value", false}}).at("Ok") == true,
				"Player policy is authorable through the schema-backed property boundary");
		}
		Success("Undo");
		Check(ReadBoolean(Success("GetSnapshot"), "Players", "DefaultCameraEnabled"),
			"Player policy participates in authoritative Undo");
		Success("Redo");
		Check(!ReadBoolean(Success("GetSnapshot"), "Players", "DefaultCameraEnabled"),
			"Player policy participates in authoritative Redo");
		const auto Character = Create(CharacterSchema, Workspace, "AuthorCharacter");
		const auto Kinematic = Create(KinematicSchema, Workspace, "NetworkCharacter");
		const auto PlainRoot = Create(PartSchema, Character, "PlainRoot");
		const auto ForeignPart = Create(PartSchema, Kinematic, "ForeignPart");
		const auto LoosePart = Create(PartSchema, Workspace, "LoosePart");
		const auto RootReference = Json{{"Type", "ObjectReference"}, {"Value", PlainRoot}};
		Check(Write(Character, CharacterSchema, CharacterSchema, "RootPart", RootReference).at("Ok") == true,
			"Typed RootPart reference binds a descendant Part");
		const auto Bound = Success("GetCharacterComposition", {
			{"CharacterCompositionVersion", 1}, {"Scope", Scope}, {"Object", Character},
		}).at("Result");
		Check(Bound.at("Context") == "Authoring" && Bound.at("CharacterCompositionVersion") == 1 &&
			Bound.at("Scope") == Scope && Bound.at("Character") == Character && Bound.at("RootPart") == PlainRoot &&
			Bound.at("AuthoritativeRevision") == Success("GetProjectState").at("Result").at("AuthoritativeRevision") &&
			Bound.at("Complete") == true && Bound.at("Valid") == true &&
			Bound.at("Meshes").empty() && Bound.at("Attachments").empty() &&
			Bound.at("ObjectsVisited").get<std::size_t>() >= 2,
			"Part-only Character inspection uses authoring scope and needs no Mesh asset");
		const auto BeforeRejectedReference = Success("GetProjectState").at("Result");
		Check(Write(Character, CharacterSchema, CharacterSchema, "RootPart",
			{{"Type", "ObjectReference"}, {"Value", ForeignPart}}).at("Ok") == false &&
			Write(Character, CharacterSchema, CharacterSchema, "RootPart",
				{{"Type", "ObjectReference"}, {"Value", LoosePart}}).at("Ok") == false &&
			Success("GetProjectState").at("Result") == BeforeRejectedReference &&
			Success("GetCharacterComposition", {
				{"CharacterCompositionVersion", 1}, {"Scope", Scope}, {"Object", Character},
			}).at("Result").at("RootPart") == PlainRoot,
			"Foreign and non-descendant RootPart references reject without mutation");
		Check(Write(Character, CharacterSchema, CharacterSchema, "RootPart", {{"Type", "Null"}}).at("Ok") == true,
			"Typed nullable RootPart clear succeeds");
		const auto Cleared = Inspect(Character, Scope).at("Result");
		Check(Cleared.at("RootPart").is_null() && Cleared.at("Valid") == true &&
			HasIssue(Cleared, "MissingRootPart"), "Null-root Character is a valid logical actor");

		std::filesystem::create_directories(Root / "assets");
		std::filesystem::copy_file(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, Root / "assets" / "rig.gltf");
		const auto Import = Success("ImportAsset", {{"Source", "assets/rig.gltf"}});
		Check(Import.at("Result").at("OperationSucceeded") == true, "Skinned fixture imports through authoring RPC");
		const auto Catalog = Success("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		std::string MeshReference;
		for (const auto &Asset : Catalog.at("Result").at("Assets"))
			if (Asset.at("Kind") == "Mesh") MeshReference = Asset.at("Reference").get<std::string>();
		Check(!MeshReference.empty(), "Imported catalog contains canonical skinned Mesh");
		const auto RigDescription = Success("GetAnimationRig", {{"Reference", MeshReference}}).at("Result");
		std::string PrimaryClip, SecondaryClip;
		for (const auto &Clip : RigDescription.at("Clips")) {
			if (Clip.at("Name") == "BeaconPulse") PrimaryClip = Clip.at("Reference").get<std::string>();
			if (Clip.at("Name") == "BeaconLunge") SecondaryClip = Clip.at("Reference").get<std::string>();
		}
		Check(!PrimaryClip.empty() && !SecondaryClip.empty(), "Fixture supplies two compatible clips");
		const auto Mesh = Create(MeshSchema, Kinematic, "SkinnedRoot");
		Create(AnimatorSchema, Mesh, "RigAnimator");
		const auto Attachment = Create(AttachmentSchema, Mesh, "RigSocket");
		Check(Write(Mesh, MeshSchema, MeshSchema, "Mesh", {{"Type", "String"}, {"Value", MeshReference}}).at("Ok") == true &&
			Write(Attachment, AttachmentSchema, AttachmentSchema, "JointPath",
				{{"Type", "String"}, {"Value", "BeaconRoot/BeaconTip"}}).at("Ok") == true &&
			Write(Kinematic, KinematicSchema, CharacterSchema, "RootPart",
				{{"Type", "ObjectReference"}, {"Value", Mesh}}).at("Ok") == true,
			"KinematicCharacter binds a skinned RootPart, Animator, and semantic socket");
		const auto Composed = Inspect(Kinematic, Scope).at("Result");
		Check(Composed.at("RootPart") == Mesh && Composed.at("Complete") == true &&
			Composed.at("Meshes").size() == 1 && Composed.at("Meshes")[0].at("Mesh").at("Reference") == MeshReference &&
			Composed.at("Meshes")[0].at("Mesh").at("Available") == true &&
			Composed.at("Meshes")[0].at("JointCount") == 2 &&
			Composed.at("Meshes")[0].at("AnimatorCount") == 1 &&
			Composed.at("Meshes")[0].at("IsRootPart") == true &&
			Composed.at("Attachments").size() == 1 &&
			Composed.at("Attachments")[0].at("JointPath") == "BeaconRoot/BeaconTip" &&
			Composed.at("Attachments")[0].at("JointPathExists") == true,
			"Inspection resolves canonical Mesh and joint path without evaluating pose");
		Check(Write(Attachment, AttachmentSchema, AttachmentSchema, "JointPath",
			{{"Type", "String"}, {"Value", "BeaconRoot/Unknown"}}).at("Ok") == true,
			"A syntactically valid missing joint path can be authored");
		const auto MissingJoint = Inspect(Kinematic, Scope).at("Result");
		Check(MissingJoint.at("Attachments")[0].at("JointPathExists") == false &&
			HasIssue(MissingJoint, "JointPathUnavailable"),
			"JointPathExists reports canonical skeleton membership, not playback state");
		Check(Write(Attachment, AttachmentSchema, AttachmentSchema, "JointPath",
			{{"Type", "String"}, {"Value", "BeaconRoot/BeaconTip"}}).at("Ok") == true,
			"Restore valid authored joint path");

		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Scope}}, "MalformedRequest");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 2}, {"Scope", Scope}, {"Object", Character}},
			"UnsupportedCapabilityVersion");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Scope},
			{"Object", Character}, {"Extra", true}}, "MalformedRequest");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Json::object()},
			{"Object", Character}}, "MalformedRequest");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Workspace},
			{"Object", Character}}, "StaleProject");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Scope},
			{"Object", Workspace}}, "InvalidCharacter");
		const auto Transient = Create(CharacterSchema, Workspace, "TransientCharacter");
		Success("DestroyInstance", {{"Object", Transient}});
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Scope},
			{"Object", Transient}}, "StaleObject");
		Success("PollChanges");
		const auto BeforeRead = Success("GetProjectState").at("Result");
		const auto Group = Success("BeginTransaction", {{"Label", "Read-only character inspection"}}).at("Result");
		const auto DuringGroup = Inspect(Kinematic, Scope);
		Check(DuringGroup.at("Ok") == true && DuringGroup.at("Result").at("RootPart") == Mesh,
			"Inspection works while a Studio transaction remains open");
		const auto CommittedGroup = Success("CommitTransaction", {{"TransactionId", Group.at("TransactionId")}});
		Check(CommittedGroup.at("Result").at("Status") == "NoChanges" &&
			Success("GetProjectState").at("Result") == BeforeRead &&
			Success("PollChanges").at("Result").at("Records").empty(),
			"Inspection does not close a group, advance revision, or append history/journal changes");

		const auto BeforePreview = Success("GetProjectState").at("Result");
		const auto Open = Success("OpenAnimationPreview", {{"Reference", MeshReference}});
		const auto PreviewId = Open.at("Result").at("PreviewId");
		auto Control = [&](Json Parameters) {
			Parameters["PreviewId"] = PreviewId;
			return Call("ControlAnimationPreview", std::move(Parameters));
		};
		auto Versioned = [&](Json Parameters) {
			Parameters["BlendVersion"] = 1;
			return Control(std::move(Parameters));
		};
		Check(Versioned({{"Action", "BlendWeight"}, {"Value", 0.5}}).at("Error").at("Code") == "ClipRequired",
			"Blend requires a selected pair");
		const auto Legacy = Control({{"Action", "Clip"}, {"Value", PrimaryClip}});
		Check(Legacy.at("Ok") == true && Legacy.at("Result").size() == 1 &&
			Legacy.at("Result").at("PreviewId") == PreviewId,
			"Legacy Clip remains Primary and retains the original response shape");
		Check(Versioned({{"Action", "BlendWeight"}, {"Value", 0.5}}).at("Error").at("Code") == "ClipRequired",
			"One selected clip is insufficient for blending");
		Error("ControlAnimationPreview", {{"PreviewId", PreviewId}, {"Action", "Clip"},
			{"Slot", "Secondary"}, {"Value", SecondaryClip}}, "UnsupportedCapabilityVersion");
		const auto Paired = Versioned({{"Action", "Clip"}, {"Slot", "Secondary"}, {"Value", SecondaryClip}});
		Check(Paired.at("Ok") == true && Paired.at("Result").at("BlendVersion") == 1 &&
			Paired.at("Result").at("BlendWeight") == 0.5 &&
			Paired.at("Result").at("PrimaryPlaybackState") == "Paused" &&
			Paired.at("Result").at("SecondaryPlaybackState") == "Paused" &&
			Paired.at("Result").at("PrimaryTimePosition") == 0 &&
			Paired.at("Result").at("SecondaryTimePosition") == 0,
			"Versioned secondary selection reports a paused balanced pair");
		const auto Seek = Versioned({{"Action", "Seek"}, {"Slot", "Secondary"}, {"Value", 0.5}});
		Check(Seek.at("Ok") == true &&
			std::abs(Seek.at("Result").at("SecondaryTimePosition").get<double>() - 0.5) < 0.001 &&
			Seek.at("Result").at("PrimaryTimePosition") == 0,
			"Slot-specific Seek changes only the selected timeline");
		for (const double Weight : {0.0, 1.0, 0.25}) {
			const auto Changed = Versioned({{"Action", "BlendWeight"}, {"Value", Weight}});
			Check(Changed.at("Ok") == true && Changed.at("Result").at("BlendWeight") == Weight,
				"Blend endpoints and interior weight are accepted");
		}
		for (const double Weight : {-0.1, 1.1, 1e300}) {
			const auto Rejected = Versioned({{"Action", "BlendWeight"}, {"Value", Weight}});
			Check(Rejected.at("Ok") == false && Rejected.at("Error").at("Code") == "InvalidPlayback",
				"Out-of-unit or float-nonfinite blend value is rejected");
		}
		Check(Versioned({{"Action", "BlendWeight"}, {"Value", "half"}}).at("Ok") == false &&
			Versioned({{"Action", "BlendWeight"}, {"Value", 0.5}, {"Slot", "Primary"}}).at("Ok") == false &&
			Versioned({{"Action", "Clip"}, {"Slot", "Other"}, {"Value", SecondaryClip}}).at("Error").at("Code") == "MalformedRequest" &&
			Control({{"Action", "BlendWeight"}, {"Value", 0.5}, {"BlendVersion", 2}}).at("Error").at("Code") == "UnsupportedCapabilityVersion" &&
			Versioned({{"Action", "Play"}, {"Extra", true}}).at("Error").at("Code") == "MalformedRequest",
			"Malformed blend controls, slot, version, and extra fields reject");
		const auto ClearedBlend = Versioned({{"Action", "ClearSecondaryClip"}});
		Check(ClearedBlend.at("Ok") == true && ClearedBlend.at("Result").at("BlendWeight") == 0 &&
			ClearedBlend.at("Result").at("SecondaryTimePosition").is_null() &&
			ClearedBlend.at("Result").at("SecondaryPlaybackState").is_null() &&
			!ClearedBlend.at("Result").at("PrimaryTimePosition").is_null() &&
			Versioned({{"Action", "ClearSecondaryClip"}}).at("Ok") == true &&
			Versioned({{"Action", "BlendWeight"}, {"Value", 0.5}}).at("Error").at("Code") == "ClipRequired",
			"Clear is idempotent, leaves Primary, and removes blend eligibility");
		Check(Success("GetProjectState").at("Result") == BeforePreview &&
			Success("PollChanges").at("Result").at("Records").empty(),
			"Headless preview controls do not mutate project state or journal");
		Success("CloseAnimationPreview", {{"PreviewId", PreviewId}});
		Error("ControlAnimationPreview", {{"PreviewId", PreviewId}, {"Action", "Play"}}, "StalePreview");

		Success("SaveProject");
		Success("OpenProject", {{"Root", Root.generic_string()}});
		const auto Reopened = Success("GetSnapshot");
		const auto FreshScope = Reopened.at("Result").at("Snapshot").at("Cursor").at("Scope");
		const auto ReopenedCharacter = FindObject(Reopened, "AuthorCharacter");
		const auto ReopenedKinematic = FindObject(Reopened, "NetworkCharacter");
		for (const auto Property : {"CharacterAutoLoads", "DefaultControllerEnabled", "DefaultCameraEnabled"})
			Check(!ReadBoolean(Reopened, "Players", Property),
				"Saved player policy survives project establishment and reopen");
		Check(FreshScope != Scope && Inspect(ReopenedCharacter, FreshScope).at("Result").at("RootPart").is_null() &&
			Inspect(ReopenedKinematic, FreshScope).at("Result").at("Meshes").size() == 1,
			"Saved Character composition survives reopen under a fresh project scope");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", Scope},
			{"Object", ReopenedCharacter}}, "StaleProject");
		Error("GetCharacterComposition", {{"CharacterCompositionVersion", 1}, {"Scope", FreshScope},
			{"Object", Character}}, "StaleObject");
		std::cout << "[Character:EditorHost] PASS composition and blend RPC\n";
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Character:EditorHost] FAIL " << Failure.what() << '\n';
		return 1;
	}
}

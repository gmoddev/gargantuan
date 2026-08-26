#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ProximityPrompt.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/services/InteractionService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

namespace {
	using Json = nlohmann::json;
	using namespace gargantuan;

	class TestWorkspace final {
	public:
		TestWorkspace() {
			const auto Suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
				std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
			Root = std::filesystem::temp_directory_path() / ("gargantuan-first-complete-game-" + Suffix);
			std::filesystem::copy(
				std::filesystem::path(GARGANTUAN_FIRST_COMPLETE_GAME_ROOT), Root,
				std::filesystem::copy_options::recursive
			);
		}

		~TestWorkspace() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}

		std::filesystem::path Root;
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	bool HasObject(const Json &Objects, std::string_view Name, std::string_view ClassName = {}) {
		return std::ranges::any_of(Objects, [&](const Json &Object) {
			return Object.value("Name", "") == Name &&
				(ClassName.empty() || Object.value("ClassName", "") == ClassName);
		});
	}

	const Json &GetObject(const Json &Objects, std::string_view Name) {
		auto Match = std::ranges::find_if(Objects, [&](const Json &Object) {
			return Object.value("Name", "") == Name;
		});
		if (Match == Objects.end()) throw std::runtime_error("Missing authored object: " + std::string(Name));
		return *Match;
	}

	const Json &GetObjectById(const Json &Objects, const Json &Id) {
		auto Match = std::ranges::find_if(Objects, [&](const Json &Object) { return Object["Id"] == Id; });
		if (Match == Objects.end()) throw std::runtime_error("Missing authored object identity");
		return *Match;
	}

	bool HasDiagnostic(const Json &Diagnostics, std::string_view Text) {
		return std::ranges::any_of(Diagnostics, [&](const Json &Diagnostic) {
			return Diagnostic.value("Message", "").find(Text) != std::string::npos;
		});
	}

	void AppendDiagnostics(Json &Destination, const Json &Source) {
		for (const auto &Diagnostic : Source) Destination.push_back(Diagnostic);
	}

	struct RuntimeEvidence {
		std::vector<PlayDiagnostic> Diagnostics;
		double RepresentativeFrameMilliseconds = 0.0;
	};

	bool HasDiagnostic(const std::vector<PlayDiagnostic> &Diagnostics, std::string_view Text) {
		return std::ranges::any_of(Diagnostics, [&](const PlayDiagnostic &Diagnostic) {
			return Diagnostic.Message.find(Text) != std::string::npos;
		});
	}

	void AppendDiagnostics(std::vector<PlayDiagnostic> &Destination, std::vector<PlayDiagnostic> Source) {
		Destination.insert(
			Destination.end(), std::make_move_iterator(Source.begin()), std::make_move_iterator(Source.end())
		);
	}

	RenderPublicationPtr TakeLatestRenderPublication(PlaySession &Session) {
		auto Publications = Session.TakeRenderPublications();
		if (Publications.empty()) return nullptr;
		return std::move(Publications.back());
	}

	RuntimeEvidence ProveDefaultPlayerLoop(const std::filesystem::path &Root) {
		DiskFilesystem Filesystem(Root);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		auto AuthoringWorld = ProjectValue.DeserializeGame();
		AuthoringWorld->InitializeLoadedProjectRevision();
		AuthoringWorld->Root = Root;
		AuthoringWorld->Filesystem = &Filesystem;
		auto Launch = ProjectValue.CaptureGame(AuthoringWorld, AuthoringWorld->GetAuthoritativeRevision());
		PlaySession Session(
			{901}, std::move(Launch.Contents), ProjectValue.InstanceFileFormat, Root,
			320, 200, Launch.Revision, std::move(Launch.Assets)
		);
		RuntimeEvidence Evidence;
		AppendDiagnostics(Evidence.Diagnostics, Session.DrainDiagnostics());
		auto InitialPublication = TakeLatestRenderPublication(Session);
		Require(InitialPublication && InitialPublication->FullResync &&
			InitialPublication->Frame.ViewportWidth == 320 && InitialPublication->Frame.ViewportHeight == 200,
			"headless runtime did not publish its initial complete frame");
		Require(InitialPublication->UiChanged && !InitialPublication->GetUi().Batches.empty(),
			"headless runtime publication omitted the authored GUI");
		auto RuntimeWorld = Session.GetWorld();
		auto PlayersValue = RuntimeWorld ? std::dynamic_pointer_cast<Players>(RuntimeWorld->GetService("Players")) : nullptr;
		auto WorkspaceValue = RuntimeWorld ? std::dynamic_pointer_cast<Workspace>(RuntimeWorld->GetService("Workspace")) : nullptr;
		auto InteractionValue = RuntimeWorld ? std::dynamic_pointer_cast<InteractionService>(
			RuntimeWorld->GetService("InteractionService")
		) : nullptr;
		Require(PlayersValue && PlayersValue->GetLocalPlayer(), "default Players runtime did not create LocalPlayer");
		auto LocalPlayer = *PlayersValue->GetLocalPlayer();
		Require(LocalPlayer->GetCharacter().has_value(), "default player runtime did not assemble a character");
		auto Character = *LocalPlayer->GetCharacter();
		Require(std::dynamic_pointer_cast<KinematicCharacter>(Character) && WorkspaceValue && WorkspaceValue->GetCurrentCamera(),
			"default kinematic character or camera did not initialize");
		Require(InteractionValue && !InteractionValue->GetDefaultPresentationEnabled() &&
			!RuntimeWorld->FindFirstChild("DefaultInteractionGui", false),
			"headless runtime did not keep default prompt presentation absent");
		const auto Start = Character->GetPosition();
		const auto ForwardDown = KeyEvent{{1}, PhysicalKey::W, LogicalKey::W, KeyModifier::None, ButtonState::Pressed};
		const auto ForwardUp = KeyEvent{{1}, PhysicalKey::W, LogicalKey::W, KeyModifier::None, ButtonState::Released};
		(void)Session.ProcessEvent(ForwardDown);
		for (int Frame = 0; Frame < 8; ++Frame) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			Session.Step();
		}
		(void)Session.ProcessEvent(ForwardUp);
		Require(glm::length(Character->GetPosition() - Start) > 0.05f,
			"ActionMap input did not produce default kinematic motion through physics");

		Session.Resize(640, 360);
		const auto FrameStart = std::chrono::steady_clock::now();
		Session.Step();
		Evidence.RepresentativeFrameMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - FrameStart
		).count();
		auto ResizedPublication = TakeLatestRenderPublication(Session);
		Require(ResizedPublication && ResizedPublication->Frame.ViewportWidth == 640 &&
			ResizedPublication->Frame.ViewportHeight == 360 &&
			ResizedPublication->GetUi().ViewportWidth == 640 && ResizedPublication->GetUi().ViewportHeight == 360,
			"headless runtime publication did not follow viewport resize");

		auto ActivateCollectible = [&](std::string_view Name, std::chrono::milliseconds Hold) {
			auto Item = std::dynamic_pointer_cast<Part>(WorkspaceValue->FindFirstChild(std::string(Name), true));
			auto Prompt = Item ? std::dynamic_pointer_cast<ProximityPrompt>(
				Item->FindFirstChildOfClass("ProximityPrompt", false)
			) : nullptr;
			auto SoundValue = Item ? std::dynamic_pointer_cast<Sound>(
				Item->FindFirstChildOfClass("Sound", false)
			) : nullptr;
			Require(Item && Prompt && SoundValue && !SoundValue->GetSoundId().empty(),
				"sample collectible is missing its authored prompt or positional audio cue");
			Character->SetPosition(Item->GetPosition() + glm::vec3(0.0f, 1.0f, 0.0f));
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
			Session.Step();
			Require(InteractionValue->GetActivePrompt() == std::optional(Prompt),
				"entering range did not publish the expected active prompt");
			const auto Down = KeyEvent{{1}, PhysicalKey::E, LogicalKey::E, KeyModifier::None, ButtonState::Pressed};
			const auto Up = KeyEvent{{1}, PhysicalKey::E, LogicalKey::E, KeyModifier::None, ButtonState::Released};
			(void)Session.ProcessEvent(Down);
			const auto End = std::chrono::steady_clock::now() + Hold + std::chrono::milliseconds(80);
			do {
				std::this_thread::sleep_for(std::chrono::milliseconds(35));
				Session.Step();
			} while (std::chrono::steady_clock::now() < End);
			(void)Session.ProcessEvent(Up);
			Session.Step();
			Require(!Prompt->GetEnabled() && !InteractionValue->GetAvailable(),
				"validated prompt activation did not collect and retire the prompt");
		};
		ActivateCollectible("Collectible1", std::chrono::milliseconds(0));
		auto LosItem = std::dynamic_pointer_cast<Part>(WorkspaceValue->FindFirstChild("Collectible2", true));
		auto LosPrompt = LosItem ? std::dynamic_pointer_cast<ProximityPrompt>(
									   LosItem->FindFirstChildOfClass("ProximityPrompt", false)
								   )
								 : nullptr;
		Require(
			LosItem && LosPrompt && LosPrompt->GetRequiresLineOfSight(),
			"sample must author Collectible2 as its LOS-enabled interaction"
		);
		Character->SetPosition(LosItem->GetPosition() + glm::vec3(0.0f, 1.0f, 0.0f));
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		Session.Step();
		Require(InteractionValue->GetActivePrompt() == std::optional(LosPrompt),
			"clear path did not expose the LOS-enabled sample prompt");
		auto LosBlocker = std::make_shared<Part>();
		LosBlocker->SetName("RuntimeLosProof");
		LosBlocker->SetAnchored(true);
		LosBlocker->SetCFrame(CFrame(LosItem->GetPosition() + glm::vec3(0.0f, 0.85f, 0.0f)));
		LosBlocker->SetSize({2.0f, 0.1f, 2.0f});
		LosBlocker->SetParent(WorkspaceValue);
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		Session.Step();
		Require(!InteractionValue->GetAvailable(), "runtime rigid blocker did not hide the LOS-enabled sample prompt");
		LosBlocker->Destroy();
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		Session.Step();
		Require(
			InteractionValue->GetActivePrompt() == std::optional(LosPrompt),
			"removing the runtime blocker did not restore the LOS-enabled sample prompt"
		);
		ActivateCollectible("Collectible2", std::chrono::milliseconds(400));
		Character->SetPosition({100.0f, 6.0f, 100.0f});
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		Session.Step();
		Require(!InteractionValue->GetAvailable(), "leaving range did not hide interaction availability");

		const auto CompleteDown = KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Pressed};
		const auto CompleteUp = KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Released};
		(void)Session.ProcessEvent(CompleteDown);
		(void)Session.ProcessEvent(CompleteUp);
		Session.Step();
		Require(TakeLatestRenderPublication(Session) != nullptr,
			"completion did not produce a headless runtime publication");
		(void)Session.ProcessEvent(PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Pressed, {320.0f, 241.0f}});
		(void)Session.ProcessEvent(PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Released, {320.0f, 241.0f}});
		Session.Step();
		Require(TakeLatestRenderPublication(Session) != nullptr,
			"GUI Restart activation did not produce a headless runtime publication");
		AppendDiagnostics(Evidence.Diagnostics, Session.DrainDiagnostics());
		Session.Stop();
		AppendDiagnostics(Evidence.Diagnostics, Session.DrainDiagnostics());
		Require(!std::ranges::any_of(Evidence.Diagnostics, [](const PlayDiagnostic &Diagnostic) {
			return Diagnostic.Severity == "Error";
		}), "headless runtime emitted an error diagnostic");
		RuntimeWorld.reset();
		AuthoringWorld->Destroy();
		return Evidence;
	}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestWorkspace Workspace;
		auto HeadlessRuntimeEvidence = ProveDefaultPlayerLoop(Workspace.Root);
		EditorHost Host("first-complete-game-gate");
		std::size_t RequestNumber = 0;
		auto Call = [&](std::string Method, Json Parameters = Json::object()) {
			Json Request{
				{"Version", EditorHostProtocolVersion},
				{"RequestId", std::to_string(++RequestNumber)},
				{"SessionToken", "first-complete-game-gate"},
				{"Method", std::move(Method)},
				{"Params", std::move(Parameters)},
			};
			return Json::parse(Host.HandleRequest(Request.dump()));
		};

		Require(Call("Handshake")["Ok"], "EditorHost handshake failed");
		auto Opened = Call("OpenProject", {{"Root", Workspace.Root.generic_string()}});
		Require(Opened["Ok"], "sample project did not open");
		auto Schema = Call("GetSchema");
		Require(Schema["Ok"], "runtime schema could not be discovered for generic authoring");
		auto PromptDefinition = std::ranges::find_if(
			Schema["Result"]["Definitions"],
			[](const Json &Definition) { return Definition.value("CanonicalName", "") == "Engine.ProximityPrompt"; }
		);
		Require(
			PromptDefinition != Schema["Result"]["Definitions"].end() &&
				PromptDefinition->value("Constructible", false),
			"ProximityPrompt is not available to Studio's schema-driven Insert Object path"
		);
		for (const auto PropertyName : {
			"Enabled", "ActionText", "ObjectText", "MaxActivationDistance", "HoldDuration", "RequiresLineOfSight"
		})
			Require(std::ranges::any_of((*PromptDefinition)["Properties"], [&](const Json &Property) {
				return Property.value("Name", "") == PropertyName && Property.value("Editable", false);
			}), "a ProximityPrompt property is not available to Studio's schema-driven inspector");
		auto SoundDefinition = std::ranges::find_if(
			Schema["Result"]["Definitions"],
			[](const Json &Definition) { return Definition.value("CanonicalName", "") == "Engine.Sound"; }
		);
		Require(SoundDefinition != Schema["Result"]["Definitions"].end() &&
			SoundDefinition->value("Constructible", false),
			"Sound is not available to Studio's schema-driven Insert Object path");
		for (const auto PropertyName : {"SoundId", "Volume", "PlaybackSpeed", "Looped", "RollOffMinDistance", "RollOffMaxDistance"})
			Require(std::ranges::any_of((*SoundDefinition)["Properties"], [&](const Json &Property) {
				return Property.value("Name", "") == PropertyName && Property.value("Editable", false);
			}), "a Sound property is not available to Studio's schema-driven inspector");
		auto Snapshot = Call("GetSnapshot");
		Require(Snapshot["Ok"], "sample project snapshot failed");
		const auto ObjectsBeforePlay = Snapshot["Result"]["Snapshot"]["Objects"];

		const std::array<std::pair<std::string_view, std::string_view>, 16> ExpectedObjects{{
			{"CollectionCourse", "Folder"}, {"Ground", "Part"}, {"MovingObstacle", "Part"},
			{"ImportedBeacon", "MeshPart"}, {"Collectibles", "Folder"}, {"Collectible1", "Part"},
			{"Collectible2", "Part"}, {"Collectible3", "Part"}, {"CollectionGui", "ScreenGui"},
			{"Hud", "Frame"}, {"Badge", "ImageLabel"}, {"Progress", "TextLabel"},
			{"WinPanel", "Frame"}, {"RestartButton", "TextButton"}, {"RoundCompleteSound", "Sound"},
			{"RoundManager", "Script"},
		}};
		for (const auto &Expected : ExpectedObjects)
			Require(HasObject(ObjectsBeforePlay, Expected.first, Expected.second), "authored hierarchy is incomplete");
		Require(std::ranges::count_if(ObjectsBeforePlay, [](const Json &Object) {
			return Object.value("ClassName", "") == "ProximityPrompt";
		}) == 3, "sample must author exactly one ProximityPrompt under each collectible");
		Require(std::ranges::count_if(ObjectsBeforePlay, [](const Json &Object) {
			return Object.value("ClassName", "") == "Sound";
		}) == 4, "sample must author three positional pickup sounds and one non-positional completion sound");

		const auto &RoundManager = GetObject(ObjectsBeforePlay, "RoundManager");
		auto Source = Call("GetScriptSource", {{"Object", RoundManager["Id"]}});
		Require(Source["Ok"], "RoundManager source could not be read");
		const auto &SourceText = Source["Result"]["Source"].get_ref<const std::string &>();
		for (const auto RequiredText : {
			"ActionMap", "Players", "RunService", "RestartButton.Activated", "CompleteRound", "ProximityPrompt",
			"Prompt.Triggered", "GetSound(Item):Play()", "RoundCompleteSound:Play()"
		})
			Require(SourceText.find(RequiredText) != std::string::npos, "RoundManager omits a required public gameplay API");
		Require(SourceText.find("(Character.Position - Item.Position).Magnitude") == std::string::npos,
			"FirstCompleteGame still contains its removed Luau distance-polling workaround");

		const auto &Collectible1 = GetObject(ObjectsBeforePlay, "Collectible1");
		const auto &Collectible2 = GetObject(ObjectsBeforePlay, "Collectible2");
		auto CreatedPrompt = Call("CreateInstance", {
			{"ClassSchemaId", SchemaId::FromNativeName("Engine", "ProximityPrompt").ToString()},
			{"DefinitionVersion", 1}, {"Parent", Collectible1["Id"]}, {"Name", "AuthoringProbe"},
		});
		Require(CreatedPrompt["Ok"], "generic Studio command path could not insert a ProximityPrompt");
		const auto CreatedPromptId = CreatedPrompt["Result"]["Object"];
		Require(Call("SetProperty", {
			{"Object", CreatedPromptId},
			{"ClassSchemaId", (*PromptDefinition)["SchemaId"]},
			{"ClassDefinitionVersion", (*PromptDefinition)["DefinitionVersion"]},
			{"DeclaringClassSchemaId", (*PromptDefinition)["SchemaId"]},
			{"DeclaringDefinitionVersion", (*PromptDefinition)["DefinitionVersion"]},
			{"Property", "ActionText"},
			{"Value", {{"Type", "String"}, {"Value", "Inspect"}}},
		})["Ok"], "generic Studio property command could not edit a ProximityPrompt");
		Require(Call("Undo")["Ok"], "generic Studio Undo could not restore the prompt property");
		auto UndoSnapshot = Call("GetSnapshot");
		Require(
			UndoSnapshot["Ok"] && GetObjectById(
				UndoSnapshot["Result"]["Snapshot"]["Objects"], CreatedPromptId
			)["EditorProperties"]["ActionText"].value("Value", "") == "Interact",
			"prompt property Undo did not restore its authoritative default"
		);
		Require(Call("Redo")["Ok"], "generic Studio Redo could not reapply the prompt property");
		auto RedoSnapshot = Call("GetSnapshot");
		Require(
			RedoSnapshot["Ok"] && GetObjectById(
				RedoSnapshot["Result"]["Snapshot"]["Objects"], CreatedPromptId
			)["EditorProperties"]["ActionText"].value("Value", "") == "Inspect",
			"prompt property Redo did not restore its authoritative edit"
		);
		auto DuplicatedPrompt = Call("DuplicateInstance", {{"Object", CreatedPromptId}});
		Require(DuplicatedPrompt["Ok"], "generic Studio command path could not duplicate a ProximityPrompt");
		const auto DuplicatedPromptId = DuplicatedPrompt["Result"]["Object"];
		Require(Call("ReparentInstance", {
			{"Object", DuplicatedPromptId}, {"Parent", Collectible2["Id"]},
		})["Ok"], "generic Studio hierarchy command could not reparent a ProximityPrompt");
		Require(
			Call("DestroyInstance", {{"Object", CreatedPromptId}})["Ok"] &&
				Call("DestroyInstance", {{"Object", DuplicatedPromptId}})["Ok"],
			"generic Studio hierarchy command could not delete temporary prompt authoring probes"
		);

		auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		Require(Catalog["Ok"], "AssetService catalog could not be read");
		const auto &Assets = Catalog["Result"]["Assets"];
		for (const auto Kind : {"Mesh", "Material", "Image", "Font", "Audio"})
			Require(std::ranges::any_of(Assets, [&](const Json &Asset) {
				return Asset.value("Kind", "") == Kind && Asset.value("State", "") == "Ready";
			}), "a required imported asset is not Ready");
		Require(std::ranges::any_of(Assets, [](const Json &Asset) {
			return Asset.value("Kind", "") == "Mesh" && Asset.contains("Dependencies") && !Asset["Dependencies"].empty();
		}), "imported mesh dependency graph is missing");
		Require(std::ranges::any_of(Assets, [](const Json &Asset) {
			return Asset.value("Kind", "") == "Material" && Asset.contains("Dependencies") && !Asset["Dependencies"].empty();
		}), "imported material dependency graph is missing");

		auto Saved = Call("SaveProject");
		Require(Saved["Ok"] && !Saved["Result"].value("Dirty", true), "sample project did not save cleanly");
		Require(Call("OpenProject", {{"Root", Workspace.Root.generic_string()}})["Ok"], "saved sample did not reopen");
		auto Reopened = Call("GetSnapshot");
		Require(Reopened["Ok"], "reopened sample snapshot failed");
		const auto ReopenedObjects = Reopened["Result"]["Snapshot"]["Objects"];
		const auto ReopenedProjectState = Reopened["Result"]["ProjectState"];
		std::vector<std::string> ReopenedPromptLabels;
		bool FoundHoldPrompt = false;
		bool FoundLineOfSightPrompt = false;
		std::size_t ReopenedSoundCount = 0;
		for (const auto &Object : ReopenedObjects) {
			if (Object.value("ClassName", "") == "Sound") {
				++ReopenedSoundCount;
				const auto &Properties = Object["EditorProperties"];
				Require(Properties.contains("SoundId") &&
					Properties["SoundId"].value("Value", "") == "asset://a0d10f78c368437daac002a4e59fdd64",
					"SoundId did not survive sample save/reopen");
			}
			if (Object.value("ClassName", "") != "ProximityPrompt") continue;
			const auto &Properties = Object["EditorProperties"];
			Require(
				Properties.contains("Enabled") && Properties["Enabled"].value("Value", false) &&
				Properties.contains("ActionText") && Properties["ActionText"].value("Value", "") == "Collect" &&
				Properties.contains("ObjectText") && Properties.contains("MaxActivationDistance") &&
				Properties["MaxActivationDistance"].value("Value", 0.0) == 4.0 &&
				Properties.contains("HoldDuration") && Properties.contains("RequiresLineOfSight"),
				"ProximityPrompt authored properties did not survive save/reopen"
			);
			ReopenedPromptLabels.push_back(Properties["ObjectText"].value("Value", ""));
			FoundHoldPrompt = FoundHoldPrompt ||
				std::abs(Properties["HoldDuration"].value("Value", 0.0) - 0.4) < 1e-6;
			FoundLineOfSightPrompt = FoundLineOfSightPrompt ||
									 (Properties["ObjectText"].value("Value", "") == "Collectible2" &&
									  Properties["RequiresLineOfSight"].value("Value", false));
		}
		std::ranges::sort(ReopenedPromptLabels);
		Require(
			ReopenedPromptLabels == std::vector<std::string>{"Collectible1", "Collectible2", "Collectible3"} &&
				FoundHoldPrompt && FoundLineOfSightPrompt && ReopenedSoundCount == 4,
			"reopened prompt labels or hold semantics differ from the authored game"
		);
		for (const auto Name : {"Hud", "Badge", "Progress", "Hint", "WinPanel", "WinTitle", "WinDetail", "RestartButton"}) {
			const auto &Object = GetObject(ReopenedObjects, Name);
			Require(Object["EditorProperties"].contains("Position") && Object["EditorProperties"].contains("Size") &&
				Object["EditorProperties"]["Size"]["Value"].size() == 4,
				"GUI UDim2 layout did not survive save/reopen");
		}

		Require(Call("ConfigureViewport", {{"Width", 320u}, {"Height", 200u}})["Ok"],
			"initial runtime viewport configuration failed");
		auto EditViewportPick = Call("PickViewport", {{"X", 159.5}, {"Y", 99.5}});
		Require(EditViewportPick["Ok"],
			"edit viewport could not publish the sample's mesh, material, and texture resources");
		// This CTest target is deliberately headless. Runtime world/UI publication,
		// resize, completion, and pointer activation are exercised above through
		// PlaySession's HeadlessRenderer; the separate editor viewport smoke owns
		// the real SDL GPU capture contract.
		Json FirstCycleDiagnostics = Json::array();
		for (int Cycle = 0; Cycle < 10; ++Cycle) {
			auto Started = Call("StartPlaySession");
			if (!(Started["Ok"] && Started["Result"]["State"] == "Running"))
				std::cerr << "[Game:FirstCompleteGameTest] StartPlaySession response: " << Started.dump() << '\n';
			Require(Started["Ok"] && Started["Result"]["State"] == "Running", "Play did not start");
			const auto PlaySessionId = Started["Result"]["PlaySessionId"];
			Json CycleDiagnostics = Started["Result"]["Diagnostics"];
			Require(Call("SendPlayInput", {
				{"PlaySessionId", PlaySessionId}, {"Type", "Focus"}, {"Focused", true},
			})["Ok"], "runtime focus input failed");

			auto Polled = Call("PollPlayDiagnostics", {{"PlaySessionId", PlaySessionId}});
			Require(Polled["Ok"], "Play diagnostics could not be polled");
			AppendDiagnostics(CycleDiagnostics, Polled["Result"]["Diagnostics"]);
			auto Stopped = Call("StopPlaySession", {{"PlaySessionId", PlaySessionId}});
			Require(Stopped["Ok"] && Stopped["Result"]["State"] == "Stopped", "Stop did not complete");
			AppendDiagnostics(CycleDiagnostics, Stopped["Result"]["Diagnostics"]);
			Require(!std::ranges::any_of(CycleDiagnostics, [](const Json &Diagnostic) {
				return Diagnostic.value("Severity", "") == "Error";
			}), "runtime emitted an error diagnostic");
			if (Cycle == 0) FirstCycleDiagnostics = std::move(CycleDiagnostics);
		}

		Require(HasDiagnostic(FirstCycleDiagnostics, "[Game:FirstCompleteGame] Ready\t3"),
			"game manager did not start or register three collectibles");
		Require(HasDiagnostic(HeadlessRuntimeEvidence.Diagnostics, "[Game:FirstCompleteGame] Round complete"),
			"deterministic completion logic did not run");
		Require(HasDiagnostic(HeadlessRuntimeEvidence.Diagnostics, "[Game:FirstCompleteGame] Round reset"),
			"GUI Restart activation did not reset the round");

		auto AfterPlay = Call("GetSnapshot");
		Require(AfterPlay["Ok"] && AfterPlay["Result"]["Snapshot"]["Objects"] == ReopenedObjects &&
			AfterPlay["Result"]["ProjectState"] == ReopenedProjectState,
			"ten Play/Stop cycles changed authoring state");

		const auto GuiCount = std::ranges::count_if(ReopenedObjects, [](const Json &Object) {
			const auto ClassName = Object.value("ClassName", "");
			return ClassName == "ScreenGui" || ClassName == "Frame" || ClassName == "ImageLabel" ||
				ClassName == "TextLabel" || ClassName == "TextButton";
		});
		std::cout << "[Game:FirstCompleteGameTest] objects=" << ReopenedObjects.size()
			<< " gui=" << GuiCount << " importedAssets=" << Assets.size()
			<< " representativeFrameMs=" << HeadlessRuntimeEvidence.RepresentativeFrameMilliseconds
			<< " playStopCycles=10\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Game:FirstCompleteGameTest] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

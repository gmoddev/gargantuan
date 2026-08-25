#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <ranges>
#include <string>
#include <thread>

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
		auto InitialPublication = Session.TakeRenderPublication();
		Require(InitialPublication && InitialPublication->FullResync &&
			InitialPublication->Frame.ViewportWidth == 320 && InitialPublication->Frame.ViewportHeight == 200,
			"headless runtime did not publish its initial complete frame");
		Require(InitialPublication->UiChanged && !InitialPublication->GetUi().Batches.empty(),
			"headless runtime publication omitted the authored GUI");
		auto RuntimeWorld = Session.GetWorld();
		auto PlayersValue = RuntimeWorld ? std::dynamic_pointer_cast<Players>(RuntimeWorld->GetService("Players")) : nullptr;
		auto WorkspaceValue = RuntimeWorld ? std::dynamic_pointer_cast<Workspace>(RuntimeWorld->GetService("Workspace")) : nullptr;
		Require(PlayersValue && PlayersValue->GetLocalPlayer(), "default Players runtime did not create LocalPlayer");
		auto LocalPlayer = *PlayersValue->GetLocalPlayer();
		Require(LocalPlayer->GetCharacter().has_value(), "default player runtime did not assemble a character");
		auto Character = *LocalPlayer->GetCharacter();
		Require(std::dynamic_pointer_cast<KinematicCharacter>(Character) && WorkspaceValue && WorkspaceValue->GetCurrentCamera(),
			"default kinematic character or camera did not initialize");
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
		auto ResizedPublication = Session.TakeRenderPublication();
		Require(ResizedPublication && ResizedPublication->Frame.ViewportWidth == 640 &&
			ResizedPublication->Frame.ViewportHeight == 360 &&
			ResizedPublication->GetUi().ViewportWidth == 640 && ResizedPublication->GetUi().ViewportHeight == 360,
			"headless runtime publication did not follow viewport resize");

		const auto CompleteDown = KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Pressed};
		const auto CompleteUp = KeyEvent{{1}, PhysicalKey::K, LogicalKey::K, KeyModifier::None, ButtonState::Released};
		(void)Session.ProcessEvent(CompleteDown);
		(void)Session.ProcessEvent(CompleteUp);
		Session.Step();
		Require(Session.TakeRenderPublication() != nullptr,
			"completion did not produce a headless runtime publication");
		(void)Session.ProcessEvent(PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Pressed, {320.0f, 241.0f}});
		(void)Session.ProcessEvent(PointerButtonEvent{{1}, PointerButton::Left, ButtonState::Released, {320.0f, 241.0f}});
		Session.Step();
		Require(Session.TakeRenderPublication() != nullptr,
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
		auto Snapshot = Call("GetSnapshot");
		Require(Snapshot["Ok"], "sample project snapshot failed");
		const auto ObjectsBeforePlay = Snapshot["Result"]["Snapshot"]["Objects"];

		const std::array<std::pair<std::string_view, std::string_view>, 15> ExpectedObjects{{
			{"CollectionCourse", "Folder"}, {"Ground", "Part"}, {"MovingObstacle", "Part"},
			{"ImportedBeacon", "MeshPart"}, {"Collectibles", "Folder"}, {"Collectible1", "Part"},
			{"Collectible2", "Part"}, {"Collectible3", "Part"}, {"CollectionGui", "ScreenGui"},
			{"Hud", "Frame"}, {"Badge", "ImageLabel"}, {"Progress", "TextLabel"},
			{"WinPanel", "Frame"}, {"RestartButton", "TextButton"}, {"RoundManager", "Script"},
		}};
		for (const auto &Expected : ExpectedObjects)
			Require(HasObject(ObjectsBeforePlay, Expected.first, Expected.second), "authored hierarchy is incomplete");

		const auto &RoundManager = GetObject(ObjectsBeforePlay, "RoundManager");
		auto Source = Call("GetScriptSource", {{"Object", RoundManager["Id"]}});
		Require(Source["Ok"], "RoundManager source could not be read");
		const auto &SourceText = Source["Result"]["Source"].get_ref<const std::string &>();
		for (const auto RequiredText : {"ActionMap", "Players", "RunService", "RestartButton.Activated", "CompleteRound"})
			Require(SourceText.find(RequiredText) != std::string::npos, "RoundManager omits a required public gameplay API");

		auto Catalog = Call("GetAssetCatalog", {{"IncludeBuiltIns", false}});
		Require(Catalog["Ok"], "AssetService catalog could not be read");
		const auto &Assets = Catalog["Result"]["Assets"];
		for (const auto Kind : {"Mesh", "Material", "Image", "Font"})
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

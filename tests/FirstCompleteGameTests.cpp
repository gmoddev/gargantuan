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

	void ProveDefaultPlayerLoop(const std::filesystem::path &Root) {
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
		Session.Stop();
		RuntimeWorld.reset();
		AuthoringWorld->Destroy();
	}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestWorkspace Workspace;
		ProveDefaultPlayerLoop(Workspace.Root);
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
		Json FirstCycleDiagnostics = Json::array();
		double RepresentativeFrameMilliseconds = 0.0;
		for (int Cycle = 0; Cycle < 10; ++Cycle) {
			auto Started = Call("StartPlaySession");
			Require(Started["Ok"] && Started["Result"]["State"] == "Running", "Play did not start");
			const auto PlaySessionId = Started["Result"]["PlaySessionId"];
			Json CycleDiagnostics = Started["Result"]["Diagnostics"];
			Require(Call("SendPlayInput", {
				{"PlaySessionId", PlaySessionId}, {"Type", "Focus"}, {"Focused", true},
			})["Ok"], "runtime focus input failed");

			const auto FrameStart = std::chrono::steady_clock::now();
			auto Frame = Call("CaptureViewport");
			RepresentativeFrameMilliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - FrameStart
			).count();
			if (!Frame["Ok"] || Frame["Result"]["Mode"] != "Play")
				throw std::runtime_error("runtime viewport frame failed: " + Frame.dump());

			if (Cycle == 0) {
				Require(Call("SendPlayInput", {
					{"PlaySessionId", PlaySessionId}, {"Type", "Key"},
					{"Physical", static_cast<std::uint16_t>(PhysicalKey::W)},
					{"Logical", static_cast<std::uint16_t>(LogicalKey::W)}, {"State", "Pressed"},
				})["Ok"], "default movement input was rejected");
				Require(Call("CaptureViewport")["Ok"], "movement frame failed");
				Require(Call("SendPlayInput", {
					{"PlaySessionId", PlaySessionId}, {"Type", "Key"},
					{"Physical", static_cast<std::uint16_t>(PhysicalKey::W)},
					{"Logical", static_cast<std::uint16_t>(LogicalKey::W)}, {"State", "Released"},
				})["Ok"], "default movement release was rejected");
				Require(Call("ConfigureViewport", {{"Width", 640u}, {"Height", 360u}})["Ok"],
					"runtime viewport resize failed");
				Require(Call("SendPlayInput", {
					{"PlaySessionId", PlaySessionId}, {"Type", "Key"},
					{"Physical", static_cast<std::uint16_t>(PhysicalKey::K)},
					{"Logical", static_cast<std::uint16_t>(LogicalKey::K)}, {"State", "Pressed"},
				})["Ok"], "deterministic completion action was rejected");
				Require(Call("CaptureViewport")["Ok"], "completion frame failed");
				Require(Call("SendPlayInput", {
					{"PlaySessionId", PlaySessionId}, {"Type", "PointerButton"},
					{"Button", "Left"}, {"State", "Pressed"}, {"X", 320.0}, {"Y", 241.0},
				})["Ok"], "restart button press was rejected");
				Require(Call("SendPlayInput", {
					{"PlaySessionId", PlaySessionId}, {"Type", "PointerButton"},
					{"Button", "Left"}, {"State", "Released"}, {"X", 320.0}, {"Y", 241.0},
				})["Ok"], "restart button release was rejected");
			}

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
		Require(HasDiagnostic(FirstCycleDiagnostics, "[Game:FirstCompleteGame] Round complete"),
			"deterministic completion logic did not run");
		Require(HasDiagnostic(FirstCycleDiagnostics, "[Game:FirstCompleteGame] Round reset"),
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
			<< " representativeFrameMs=" << RepresentativeFrameMilliseconds << " playStopCycles=10\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Game:FirstCompleteGameTest] FAIL: " << Error.what() << '\n';
		return 1;
	}
}

#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/InputBinding.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/CharacterControlService.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	struct ActionMapTestAccess {
		static void Attach(ActionMap &Map, const std::shared_ptr<UserInputService> &Service) {
			Map.AttachInputService(Service);
		}
		static bool Process(ActionMap &Map, const HostEvent &Event) {
			return Map.ProcessEvent(Event);
		}
		static void EndFrame(ActionMap &Map) {
			Map.EndFrame();
		}
		static void Reset(ActionMap &Map) {
			Map.Reset();
		}
		static std::shared_ptr<InputBinding> Binding(ActionMap &Map, int Id) {
			const auto Found = Map.Bindings.find(Id);
			return Found == Map.Bindings.end() ? nullptr : Found->second.lock();
		}
	};
}

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Near(float Left, float Right, float Epsilon = 0.01f) {
		return std::abs(Left - Right) <= Epsilon;
	}

	gargantuan::HostEvent Key(gargantuan::LogicalKey Logical, gargantuan::ButtonState State) {
		using namespace gargantuan;
		return KeyEvent{
			.Device = {1},
			.Physical = Logical == LogicalKey::W	   ? PhysicalKey::W
						: Logical == LogicalKey::D	   ? PhysicalKey::D
						: Logical == LogicalKey::Space ? PhysicalKey::Space
						: Logical == LogicalKey::LeftShift ? PhysicalKey::LeftShift
													   : PhysicalKey::Unknown,
			.Logical = Logical,
			.State = State,
		};
	}

	void TestPlayerGuiLifecycle() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto PlayersService = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		auto Starter = World->GetService("StarterGui");
		Starter->SetArchivable(true);
		auto KeepTemplate = std::make_shared<ScreenGui>();
		KeepTemplate->SetName("Keep");
		KeepTemplate->SetArchivable(true);
		KeepTemplate->SetParent(Starter);
		auto ResetTemplate = std::make_shared<ScreenGui>();
		ResetTemplate->SetName("Reset");
		ResetTemplate->SetResetOnSpawn(true);
		ResetTemplate->SetParent(Starter);
		auto TemplatePart = std::make_shared<Part>();
		TemplatePart->SetName("PresentationPart");
		TemplatePart->SetParent(ResetTemplate);
		auto TemplateReference = std::make_shared<WeldConstraint>();
		TemplateReference->SetName("ReferenceFixture");
		TemplateReference->SetParent(ResetTemplate);
		TemplateReference->SetPart0(TemplatePart);
		ResetTemplate->MarkPersistenceSubtreeArchivable();
		bool ObservedProvisionedGui = false;
		auto Added = PlayersService->PlayerAdded->Connect([&](const auto &Player) {
			const auto Gui = Player->GetPlayerGui().value_or(nullptr);
			ObservedProvisionedGui = Gui && Gui->GetChildren().size() == 2 && !Player->GetCharacter();
		});
		auto First = PlayersService->CreateSessionPlayer({"test", "starter-gui-one"});
		const auto Gui = First->GetPlayerGui().value();
		const auto Keep = Gui->FindFirstChild("Keep", false);
		const auto OldReset = Gui->FindFirstChild("Reset", false);
		Check(ObservedProvisionedGui && Keep && OldReset && OldReset != ResetTemplate && Keep != KeepTemplate,
			"join provisions fresh PlayerGui roots before PlayerAdded without a Character or controller");
		if (!Keep || !OldReset) return;
		const auto ClonedReference = std::dynamic_pointer_cast<WeldConstraint>(OldReset->FindFirstChild("ReferenceFixture", false));
		Check(ClonedReference && ClonedReference->GetPart0().value_or(nullptr) ==
			OldReset->FindFirstChild("PresentationPart", false) &&
			ClonedReference->GetPart0().value_or(nullptr) != TemplatePart,
			"typed references inside a ScreenGui resolve to the matching cloned objects");
		Added->Disconnect();
		ResetTemplate->SetEnabled(false);
		Keep->SetName("PlayerState");
		auto Later = PlayersService->CreateSessionPlayer({"test", "starter-gui-later"});
		const auto LaterReset = std::dynamic_pointer_cast<ScreenGui>(
			Later->GetPlayerGui().value()->FindFirstChild("Reset", false));
		Check(std::static_pointer_cast<ScreenGui>(OldReset)->GetEnabled() && LaterReset && !LaterReset->GetEnabled(),
			"template property edits affect later provisioning and preserve existing clones");

		auto WorkspaceValue = World->GetService("Workspace");
		auto Spawn = [&] {
			auto Character = std::make_shared<KinematicCharacter>();
			Character->SetParent(WorkspaceValue);
			First->SetCharacter(Character);
			return Character;
		};
		auto ServerReset = std::make_shared<ScreenGui>();
		ServerReset->SetResetOnSpawn(true);
		ServerReset->SetParent(Gui);
		Spawn();
		Check(!OldReset->GetDestroyed() && !ServerReset->GetDestroyed(), "first Character binding keeps initially provisioned GUI");
		First->RemoveCharacter();
		Spawn();
		const auto NewReset = std::dynamic_pointer_cast<ScreenGui>(Gui->FindFirstChild("Reset", false));
		Check(NewReset && NewReset != OldReset && !NewReset->GetEnabled() && OldReset->GetDestroyed() &&
			ServerReset->GetDestroyed() && Gui->FindFirstChild("PlayerState", false) == Keep,
			"respawn recreates current reset templates, removes ad hoc reset roots, and preserves persistent runtime state");
		if (!NewReset) return;
		std::vector<std::shared_ptr<Character>> AddedCharacters;
		bool LatestGuiObserved = false;
		auto CharacterAdded = First->CharacterAdded->Connect([&](const auto &Value) {
			AddedCharacters.push_back(Value);
			auto Root = std::dynamic_pointer_cast<ScreenGui>(Gui->FindFirstChild("Reset", false));
			LatestGuiObserved = Root && Root->GetEnabled();
		});
		std::shared_ptr<KinematicCharacter> Nested;
		NewReset->Destroying->Once([&](std::monostate) {
			ResetTemplate->SetEnabled(true);
			Nested = Spawn();
		});
		const auto Displaced = Spawn();
		Check(Nested && First->GetCharacter().value_or(nullptr) == Nested && Displaced->GetDestroyed() &&
			AddedCharacters == std::vector<std::shared_ptr<Character>>{Nested} && LatestGuiObserved,
			"a respawn requested from GUI Destroying coalesces to the live Character and its current template");

		AddedCharacters.clear();
		std::shared_ptr<ScreenGui> Stale;
		Gui->ChildAdded->Once([&](const auto &Child) {
			Stale = std::dynamic_pointer_cast<ScreenGui>(Child);
			if (Stale) Stale->SetResetOnSpawn(false);
			Nested = Spawn();
		});
		const auto ReplacedDuringAttachment = Spawn();
		const auto LatestReset = std::dynamic_pointer_cast<ScreenGui>(Gui->FindFirstChild("Reset", false));
		Check(Nested && First->GetCharacter().value_or(nullptr) == Nested && ReplacedDuringAttachment->GetDestroyed() &&
			Stale && Stale->GetDestroyed() && LatestReset && LatestReset != Stale && Gui->GetChildren().size() == 2 &&
			AddedCharacters == std::vector<std::shared_ptr<Character>>{Nested},
			"GUI ChildAdded reentry retires stale attached clones even if callback changes their reset policy");
		CharacterAdded->Disconnect();

		First->SetArchivable(true);
		Gui->MarkPersistenceSubtreeArchivable();
		const std::array<std::shared_ptr<Instance>, 1> PlayerDocument{First};
		const auto GasState = InstanceSerialization::CaptureGasDocument(PlayerDocument, Gas::DocumentKind::Model);
		std::shared_ptr<Instance> PersistedPlayer = First;
		Check(GasState && GasState->Nodes.size() == 1 &&
			InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, PersistedPlayer).find("PlayerGui") ==
				std::string::npos,
			"JSON and GAS exclude PlayerGui even inside an explicitly archivable Player graph");
		const auto StarterJson = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Starter);
		std::istringstream Input(StarterJson);
		const auto StarterRoundtrip = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		Check(StarterRoundtrip.Ok && StarterRoundtrip.Instance && StarterRoundtrip.Instance->FindFirstChild("Reset", false),
			"authored StarterGui templates roundtrip through JSON");
		bool Blocked = false;
		{
			ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
			try { Gui->Destroy(); } catch (const std::runtime_error &) { Blocked = true; }
		}
		Check(Blocked && !Gui->GetDestroyed(), "Client domain cannot destroy the lifecycle-owned GUI container");

		if (LatestReset) {
			auto Removed = LatestReset->Destroying->Once([&](std::monostate) { First->Destroy(); });
			Spawn();
			Check(First->GetDestroyed() && Gui->GetDestroyed() && !Gui->GetParent(),
				"player retirement from a GUI removal callback aborts replacement provisioning");
		}
		auto AbortProvision = PlayersService->ChildAdded->Once([](const auto &Child) { Child->Destroy(); });
		Blocked = false;
		try { (void)PlayersService->CreateSessionPlayer({"test", "retired-during-provision"}); }
		catch (const std::runtime_error &) { Blocked = true; }
		Check(Blocked && PlayersService->GetPlayers() == std::vector{Later},
			"session player destroyed during initial attachment is never returned as a live join");
		Check(PlayersService->RemoveSessionPlayer(Later) && LaterReset && LaterReset->GetDestroyed(),
			"disconnect destroys this session player's runtime GUI");
		auto OtherTemplatePart = std::make_shared<Part>();
		OtherTemplatePart->SetArchivable(true);
		OtherTemplatePart->SetParent(KeepTemplate);
		TemplateReference->SetPart0(OtherTemplatePart);
		auto InvalidTemplates = PlayersService->CreateSessionPlayer({"test", "cross-screen-gui-reference"});
		Check(InvalidTemplates->GetPlayerGui().value()->GetChildren().empty(),
			"cross-ScreenGui template references reject provisioning before any template becomes private runtime state");
		World->Destroy();
	}

	void TestPlayerGuiScriptWaitRetirement() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto PlayersService = std::dynamic_pointer_cast<Players>(World->GetService("Players"));
		PlayersService->SetDefaultControllerEnabled(false);
		PlayersService->SetDefaultCameraEnabled(false);
		PlayersService->SetCharacterAutoLoads(false);
		auto Root = std::make_shared<ScreenGui>();
		Root->SetResetOnSpawn(true);
		Root->SetParent(World->GetService("StarterGui"));
		auto Template = std::make_shared<Script>();
		Template->SetName("ResetWait");
		Template->SetRunContext(Enums::RunContext::Client);
		Template->SetSource(R"(
			task.spawn(function()
				task.wait(0)
				game:SetAttribute("IndependentGuiTaskContinued", true)
			end)
			task.wait(0)
			game:SetAttribute("GuiMainContinued", true)
		)");
		Template->SetParent(Root);
		Root->MarkPersistenceSubtreeArchivable();
		HeadlessRenderer Renderer(Vector2(32, 32));
		Engine Runtime(World, &Renderer, nullptr, EngineProviderConfiguration{.AudioEnabled = false});
		const auto Owner = PlayersService->GetLocalPlayer().value();
		const auto Gui = Owner->GetPlayerGui().value();
		auto BindCharacter = [&] {
			auto Value = std::make_shared<KinematicCharacter>();
			Value->SetParent(Runtime.Workspace);
			Owner->SetCharacter(Value);
		};
		BindCharacter();
		const auto OldScript = std::dynamic_pointer_cast<Script>(Gui->FindFirstChild("ResetWait", true));
		Runtime.Script->Step();
		Check(OldScript && OldScript->Status == ScriptStatus::Yielded && OldScript->Thread,
			"the original Client Script has a pending main coroutine when respawn begins");
		BindCharacter();
		const auto NewScript = std::dynamic_pointer_cast<Script>(Gui->FindFirstChild("ResetWait", true));
		Runtime.Script->Step();
		Check(OldScript && OldScript->GetDestroyed() && !OldScript->Thread && NewScript &&
			NewScript != OldScript && NewScript->Status == ScriptStatus::Yielded &&
			!World->GetAttributeValue("GuiMainContinued") &&
			World->GetAttributeValue("IndependentGuiTaskContinued") == std::optional<WireValue>(true),
			"respawn cancels the destroyed main coroutine and preserves its independently spawned task");
		if (!NewScript) {
			Runtime.Destroy();
			return;
		}
		Check(World->ApplyAttributeMutation("IndependentGuiTaskContinued", std::nullopt) == MutationStatus::Success,
			"the replacement script uses a fresh independent-task observation");
		NewScript->SetEnabled(false);
		Runtime.Script->Threads.Step();
		Check(NewScript->Status == ScriptStatus::Disabled && !NewScript->Thread &&
			!World->GetAttributeValue("GuiMainContinued") &&
			World->GetAttributeValue("IndependentGuiTaskContinued") == std::optional<WireValue>(true),
			"disabling a suspended GUI Script retires its main wait while its separate task still runs");
		NewScript->SetEnabled(true);
		Runtime.Script->RunBootstrapScript(NewScript);
		Runtime.Script->Threads.Step();
		Check(World->GetAttributeValue("GuiMainContinued") == std::optional<WireValue>(true),
			"an explicitly restarted GUI Script can finish its new main wait");

		auto SelfRestart = std::make_shared<Script>();
		SelfRestart->SetName("SelfRestart");
		SelfRestart->SetRunContext(Enums::RunContext::Client);
		SelfRestart->SetSource(R"(
			if game:GetAttribute("GuiSelfRestartStarted") then
				game:SetAttribute("GuiSelfRestartFinished", true)
				return
			end
			game:SetAttribute("GuiSelfRestartStarted", true)
			task.spawn(function()
				task.wait(0)
				game:SetAttribute("GuiSelfRestartIndependent", true)
			end)
			script.Enabled = false
			script.Enabled = true
			task.wait(0)
			game:SetAttribute("GuiSelfRestartOldMain", true)
		)");
		SelfRestart->SetParent(NewScript->GetParent());
		Runtime.Script->Step();
		Check(SelfRestart->Status == ScriptStatus::Idle && !SelfRestart->Thread,
			"self-disable and re-enable preserve the existing Idle restart state");
		Runtime.Script->Step();
		Check(SelfRestart->Status == ScriptStatus::Finished &&
			World->GetAttributeValue("GuiSelfRestartFinished") == std::optional<WireValue>(true) &&
			World->GetAttributeValue("GuiSelfRestartIndependent") == std::optional<WireValue>(true) &&
			!World->GetAttributeValue("GuiSelfRestartOldMain"),
			"self-restart starts a fresh main coroutine and retires the old wait without discarding separate work");
		Runtime.Destroy();
	}

	void TestActionMapContract() {
		using namespace gargantuan;
		auto Input = std::make_shared<UserInputService>();
		auto Map = std::make_shared<ActionMap>();
		ActionMapTestAccess::Attach(*Map, Input);
		int Began = 0;
		int Ended = 0;
		int Changed = 0;
		std::vector<std::string> BeganOrder;
		auto BeganConnection = Map->ActionBegan->Connect([&](std::string Name) {
			BeganOrder.push_back(Name);
			if (Name == "MoveForward") ++Began;
		});
		auto EndedConnection = Map->ActionEnded->Connect([&](std::string Name) {
			if (Name == "MoveForward") ++Ended;
		});
		auto ChangedConnection = Map->ActionChanged->Connect([&](std::tuple<std::string, Vector2> Value) {
			if (std::get<0>(Value) == "MoveForward") ++Changed;
		});

		const auto ForwardBinding = Map->BindKey("MoveForward", Enums::KeyCode::W, 1.0f, 10, false);
		const auto ConsumingBinding = Map->BindKey("MenuAccept", Enums::KeyCode::W, 1.0f, 100, true);
		const auto Down = Key(LogicalKey::W, ButtonState::Pressed);
		(void)Input->ProcessEvent(Down);
		Check(ActionMapTestAccess::Process(*Map, Down), "consuming semantic binding reports handled input");
		Check(
			Map->IsDown("MoveForward") && Near(Map->GetValue("MoveForward"), 1.0f),
			"ActionMap publishes semantic digital state above UserInputService"
		);
		Check(Began == 1 && Changed == 1, "ActionMap fires one begin and one value change for a physical transition");
		Check(
			BeganOrder == std::vector<std::string>({"MenuAccept", "MoveForward"}),
			"higher-priority bindings publish their semantic transition first"
		);
		const auto AlternateForwardBinding = Map->BindKey("MoveForward", Enums::KeyCode::D, 0.5f, 5, false);
		const auto AlternateDown = Key(LogicalKey::D, ButtonState::Pressed);
		(void)Input->ProcessEvent(AlternateDown);
		(void)ActionMapTestAccess::Process(*Map, AlternateDown);
		const auto ForwardUp = Key(LogicalKey::W, ButtonState::Released);
		(void)Input->ProcessEvent(ForwardUp);
		(void)ActionMapTestAccess::Process(*Map, ForwardUp);
		Check(
			Map->IsDown("MoveForward") && Near(Map->GetValue("MoveForward"), 0.5f) && Ended == 0,
			"multiple physical bindings aggregate without ending an action while another source remains active"
		);
		(void)Input->ProcessEvent(Down);
		(void)ActionMapTestAccess::Process(*Map, Down);

		const auto LookBinding = Map->BindPointerDelta("Look", Vector2(2.0f, -1.0f), 0, false);
		HostEvent Motion = PointerMoveEvent{{1}, {20.0f, 30.0f}, {3.0f, 4.0f}};
		(void)Input->ProcessEvent(Motion);
		Check(!ActionMapTestAccess::Process(*Map, Motion), "non-consuming Look binding preserves lower-level routing");
		auto Look = Map->GetVector("Look");
		Check(
			Near(Look.GetX(), 6.0f) && Near(Look.GetY(), -4.0f),
			"semantic Look preserves scaled true relative pointer delta"
		);
		ActionMapTestAccess::EndFrame(*Map);
		Check(Map->GetVector("Look") == Vector2(), "transient semantic Look resets at the frame boundary");

		(void)Input->ProcessEvent(FocusEvent{false});
		(void)ActionMapTestAccess::Process(*Map, FocusEvent{false});
		Check(
			!Map->IsDown("MoveForward") && Map->GetValue("MoveForward") == 0.0f && Ended == 1,
			"focus loss clears semantic state and ends active actions"
		);
		Check(
			Map->Unbind(ForwardBinding) && Map->Unbind(AlternateForwardBinding) && Map->Unbind(ConsumingBinding) &&
				Map->Unbind(LookBinding),
			"bindings can be removed independently by identity"
		);
		Check(Map->GetBindingCount() == 0, "ActionMap releases all binding state after unbinding");
		(void)BeganConnection;
		(void)EndedConnection;
		(void)ChangedConnection;

		auto BoundedMap = std::make_shared<ActionMap>();
		ActionMapTestAccess::Attach(*BoundedMap, Input);
		for (std::size_t Index = 0; Index < ActionMap::MaximumBindings; ++Index)
			(void)BoundedMap->BindKey("Bounded", Enums::KeyCode::W, 1.0f, 0, false);
		bool Rejected = false;
		try {
			(void)BoundedMap->BindKey("Bounded", Enums::KeyCode::W, 1.0f, 0, false);
		} catch (const std::length_error &) {
			Rejected = true;
		}
		Check(Rejected, "ActionMap rejects bindings beyond its explicit native resource bound");
		BoundedMap->UnbindAction("Bounded");
		Check(BoundedMap->GetBindingCount() == 0, "bounded binding stress releases without retained action state");
		auto StoppingMap = std::make_shared<ActionMap>();
		ActionMapTestAccess::Attach(*StoppingMap, Input);
		Check(StoppingMap->BindKey("StoppingProbe", Enums::KeyCode::W, 1.0f, 0, false) > 0,
			"ActionMap accepts a legacy binding before native stop");
		ActionMapTestAccess::Reset(*StoppingMap);
		ActionMapTestAccess::Reset(*StoppingMap);
		Check(StoppingMap->GetBindingCount() == 0 &&
			StoppingMap->BindKey("LateKey", Enums::KeyCode::W, 1.0f, 0, false) == 0 &&
			StoppingMap->BindMouseButton("LateButton", Enums::UserInputType::MouseButton1, 1.0f, 0, false) == 0 &&
			StoppingMap->BindPointerDelta("LateLook", Vector2(1.0f, 1.0f), 0, false) == 0,
			"repeated ActionMap reset remains closed to every facade bind entry");
		ActionMapTestAccess::Attach(*StoppingMap, {});
		const auto ReopenedId = StoppingMap->BindKey("Reopened", Enums::KeyCode::W, 1.0f, 0, false);
		Check(ReopenedId > 1, "a completed reset can reopen through private attach without reusing a binding ID");
		ActionMapTestAccess::Reset(*StoppingMap);

		auto InterruptedMap = std::make_shared<ActionMap>();
		ActionMapTestAccess::Attach(*InterruptedMap, Input);
		const auto FirstId = InterruptedMap->BindKey("First", Enums::KeyCode::W, 1.0f, 0, false);
		const auto SecondId = InterruptedMap->BindKey("Second", Enums::KeyCode::D, 1.0f, 0, false);
		auto FirstBinding = ActionMapTestAccess::Binding(*InterruptedMap, FirstId);
		auto ThrowingConnection = FirstBinding ? FirstBinding->Destroying->Connect([](auto) {
			throw std::runtime_error("[Input:ActionMap] injected native teardown interruption");
		}) : nullptr;
		bool TeardownInterrupted = false;
		try {
			ActionMapTestAccess::Reset(*InterruptedMap);
		} catch (const std::runtime_error &) {
			TeardownInterrupted = true;
		}
		bool AttachRejected = false;
		try {
			ActionMapTestAccess::Attach(*InterruptedMap, Input);
		} catch (const std::logic_error &) {
			AttachRejected = true;
		}
		Check(TeardownInterrupted && AttachRejected && InterruptedMap->GetBindingCount() == 1 &&
			ActionMapTestAccess::Binding(*InterruptedMap, SecondId) &&
			InterruptedMap->BindKey("AfterInterruptedStop", Enums::KeyCode::W, 1.0f, 0, false) == 0,
			"interrupted native reset keeps facade admission closed and rejects reattachment with live bindings");
		if (ThrowingConnection) ThrowingConnection->Disconnect();
	}

	std::shared_ptr<gargantuan::Part> MakePart(
		const std::shared_ptr<gargantuan::Workspace> &Workspace, std::string Name, glm::vec3 Position, glm::vec3 Size
	) {
		using namespace gargantuan;
		auto Value = std::make_shared<Part>();
		Value->SetName(std::move(Name));
		Value->SetAnchored(true);
		Value->SetPosition(Position);
		Value->SetSize(Size);
		Value->SetParent(Workspace);
		return Value;
	}

	void TestDefaultPlayerRuntime() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		Check(
			PlayersValue && Game->GetService("Players") == PlayersValue,
			"Players uses canonical DataModel-scoped service access"
		);
		Check(!PlayersValue->GetLocalPlayer(), "authoring-style DataModel service access does not invent LocalPlayer");
		auto Floor = MakePart(WorkspaceValue, "Floor", {0.0f, -1.0f, 0.0f}, {60.0f, 2.0f, 60.0f});
		auto Wall = MakePart(WorkspaceValue, "Wall", {0.0f, 2.0f, -8.0f}, {20.0f, 4.0f, 2.0f});
		auto Step = MakePart(WorkspaceValue, "Step", {4.0f, 0.5f, 0.0f}, {4.0f, 1.0f, 6.0f});
		auto BootstrapObserver = std::make_shared<Script>();
		BootstrapObserver->SetName("CharacterBootstrapObserver");
		BootstrapObserver->SetSource(R"(
local Players = game:GetService("Players")
if Players.LocalPlayer == nil or Players.LocalPlayer.Character == nil then
	error("default Character runtime did not precede project scripts")
end
local Witness = Instance.new("Folder")
Witness.Name = "CharacterBootstrapObserved"
Witness.Parent = game
)" );
		BootstrapObserver->SetParent(Game);
		(void)Floor;
		(void)Wall;
		(void)Step;

		std::vector<std::pair<std::string, std::string>> Diagnostics;
		HeadlessRenderer Renderer(Vector2(640.0f, 480.0f));
		Engine Runtime(Game, &Renderer, [&](std::string Severity, std::string Message) {
			Diagnostics.emplace_back(std::move(Severity), std::move(Message));
		});
		Check(
			Runtime.Players == PlayersValue && Runtime.Players->GetLocalPlayer().has_value(),
			"runtime startup creates exactly one LocalPlayer on the canonical Players service"
		);
		auto LocalPlayer = *Runtime.Players->GetLocalPlayer();
		Check(
			LocalPlayer->GetPlayerId() == 1 &&
				Runtime.Players->GetPlayers() == std::vector<std::shared_ptr<Player>>{LocalPlayer},
			"local Player has a nonzero transport-independent identity and deterministic membership"
		);

		Runtime.Script->Step();
		Runtime.Script->Step();
		const bool DefaultError = std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
			return Diagnostic.first == "Error" && Diagnostic.second.find("DefaultPlayerRuntime") != std::string::npos;
		});
		Check(!DefaultError, "engine-shipped default Luau modules start without runtime errors");
		Check(
			Game->FindFirstChild("CharacterBootstrapObserved", std::nullopt) != nullptr,
			"engine Character bootstrap runs exactly once before ordinary project scripts"
		);
		Check(
			Runtime.ActionMap->GetBindingCount() == 10,
			"engine-shipped Character modules initialize once without duplicating semantic bindings"
		);
		Check(LocalPlayer->GetCharacter().has_value(), "default Luau assembly responds to the Player spawn contract");
		if (!LocalPlayer->GetCharacter()) {
			Runtime.Destroy();
			return;
		}
		auto Character = std::dynamic_pointer_cast<KinematicCharacter>(*LocalPlayer->GetCharacter());
		Check(
			Character && Character->IsA("Character") && !Character->IsA("Humanoid") &&
				Character->GetRootPart().has_value() && (*Character->GetRootPart())->GetParent()->get() == Character.get(),
			"default character assembly owns a visible root under KinematicCharacter state"
		);
		if (!Character) {
			Runtime.Destroy();
			return;
		}
		auto NpcCharacter = std::make_shared<KinematicCharacter>();
		NpcCharacter->SetName("UnownedNpcCharacter");
		NpcCharacter->SetParent(Runtime.Workspace);
		Check(
			NpcCharacter->IsA("Character") && *LocalPlayer->GetCharacter() != NpcCharacter,
			"canonical Character semantics support an NPC with no synthetic Player relationship"
		);
		NpcCharacter->Destroy();

		for (int Frame = 0; Frame < 120; ++Frame)
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(
			Character->GetGrounded() && Near(Character->GetPosition().y, 2.0f, 0.1f),
			"default controller lands on actual floor geometry"
		);

		const auto BeforeSpeedCheck = Character->GetCFrame();
		(void)Character->ApplyAttributeMutation("WalkSpeed", WireValue(8.0));
		(void)Character->ApplyAttributeMutation("RunSpeed", WireValue(30.0));
		(void)Runtime.ProcessEvent(Key(LogicalKey::D, ButtonState::Pressed));
		Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		const auto Walk = Character->GetVelocity();
		(void)Runtime.ProcessEvent(Key(LogicalKey::LeftShift, ButtonState::Pressed));
		Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		const auto Run = Character->GetVelocity();
		(void)Runtime.ProcessEvent(Key(LogicalKey::LeftShift, ButtonState::Released));
		(void)Runtime.ProcessEvent(Key(LogicalKey::D, ButtonState::Released));
		Check(Near(glm::length(glm::vec2(Walk.x, Walk.z)), 8.0f) &&
				Near(glm::length(glm::vec2(Run.x, Run.z)), 30.0f),
			"stock movement selects configured walk and run speeds through the semantic action map");
		(void)Character->ApplyAttributeMutation("WalkSpeed", std::nullopt);
		(void)Character->ApplyAttributeMutation("RunSpeed", std::nullopt);
		Character->SetCFrame(BeforeSpeedCheck);

		(void)Runtime.ProcessEvent(Key(LogicalKey::D, ButtonState::Pressed));
		float MaximumStepHeight = Character->GetPosition().y;
		for (int Frame = 0; Frame < 32; ++Frame) {
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
			MaximumStepHeight = std::max(MaximumStepHeight, Character->GetPosition().y);
		}
		(void)Runtime.ProcessEvent(Key(LogicalKey::D, ButtonState::Released));
		Check(
			MaximumStepHeight > 2.75f && Character->GetPosition().x > 5.0f,
			"Luau step policy traverses useful low obstacles using bounded native capsule queries"
		);

		HostEvent RightDown = PointerButtonEvent{{1}, PointerButton::Right, ButtonState::Pressed, {0.0f, 0.0f}};
		auto Capture = Runtime.ProcessEvent(RightDown);
		Check(
			Capture.Consumed && Capture.Command && std::get<SetRelativePointerMode>(*Capture.Command).Enabled,
			"semantic camera orbit consumes the legacy camera route and requests relative capture synchronously"
		);
		const auto InitialYaw = Runtime.Workspace->GetCurrentCamera()->GetYaw();
		(void)Runtime.ProcessEvent(PointerMoveEvent{{1}, {0.0f, 0.0f}, {25.0f, -10.0f}});
		Runtime.RunService->PreRender->Fire(1.0f / 60.0f);
		Check(
			Runtime.Workspace->GetCurrentCamera()->GetYaw() < InitialYaw &&
				Runtime.Workspace->GetCurrentCamera()->GetPitch() > 15.0f,
			"default camera consumes semantic Look for unrestricted yaw and pitch"
		);
		(void)Runtime.ProcessEvent(PointerMoveEvent{{1}, {0.0f, 0.0f}, {4000.0f, -1000.0f}});
		Runtime.RunService->PreRender->Fire(1.0f / 60.0f);
		Check(
			Runtime.Workspace->GetCurrentCamera()->GetYaw() < -360.0f &&
				Near(Runtime.Workspace->GetCurrentCamera()->GetPitch(), 85.0f),
			"default camera leaves yaw unwrapped and clamps pitch"
		);
		ActionMapTestAccess::EndFrame(*Runtime.ActionMap);

		auto Release = Runtime.ProcessEvent(
			PointerButtonEvent{{1}, PointerButton::Right, ButtonState::Released, {0.0f, 0.0f}}
		);
		Check(
			Release.Command && !std::get<SetRelativePointerMode>(*Release.Command).Enabled,
			"orbit release returns pointer control to the host"
		);
		(void)Runtime.ProcessEvent(RightDown);
		auto FocusRelease = Runtime.ProcessEvent(FocusEvent{false});
		Check(
			FocusRelease.Command && !std::get<SetRelativePointerMode>(*FocusRelease.Command).Enabled &&
				!Runtime.ActionMap->IsDown("CameraOrbit"),
			"focus loss clears ActionMap state and relative capture together"
		);

		auto Camera = Runtime.Workspace->GetCurrentCamera();
		Camera->SetYaw(0.0f);
		Camera->SetPitch(15.0f);
		Runtime.RunService->PreRender->Fire(1.0f / 60.0f);
		const auto StartPosition = Character->GetPosition();
		const auto ForwardDown = Key(LogicalKey::W, ButtonState::Pressed);
		(void)Runtime.ProcessEvent(ForwardDown);
		for (int Frame = 0; Frame < 90; ++Frame)
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		(void)Runtime.ProcessEvent(Key(LogicalKey::W, ButtonState::Released));
		Check(Character->GetPosition().z < StartPosition.z - 1.0f, "ActionMap drives camera-relative default movement");
		Check(Character->GetPosition().z > -7.0f, "default horizontal movement is bounded by obstacle geometry");

		const auto JumpDown = Key(LogicalKey::Space, ButtonState::Pressed);
		(void)Character->ApplyAttributeMutation("JumpSpeed", WireValue(35.0));
		(void)Runtime.ProcessEvent(JumpDown);
		Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(
			!Character->GetGrounded() && Near(Character->GetVelocity().y, 35.0f, 0.5f),
			"grounded Jump action launches the default character at the configured speed"
		);
		(void)Character->ApplyAttributeMutation("JumpSpeed", std::nullopt);
		(void)Runtime.ProcessEvent(Key(LogicalKey::Space, ButtonState::Released));
		for (int Frame = 0; Frame < 5; ++Frame)
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		const auto VelocityBeforeAirJump = Character->GetVelocity().y;
		(void)Runtime.ProcessEvent(JumpDown);
		Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(Character->GetVelocity().y < VelocityBeforeAirJump, "default controller rejects a repeated midair jump");
		(void)Runtime.ProcessEvent(Key(LogicalKey::Space, ButtonState::Released));
		for (int Frame = 0; Frame < 180 && !Character->GetGrounded(); ++Frame)
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(Character->GetGrounded(), "jump arc lands again through real world grounding");

		Character->Destroy();
		Check(
			!LocalPlayer->GetCharacter(),
			"externally destroyed character clears the Player relationship deterministically"
		);
		LocalPlayer->LoadCharacter();
		Check(
			LocalPlayer->GetCharacter().has_value(),
			"character spawn remains reusable after external character destruction"
		);
		Character = std::dynamic_pointer_cast<KinematicCharacter>(*LocalPlayer->GetCharacter());
		Check(Character != nullptr, "reloaded default Character retains the kinematic movement specialization");
		if (!Character) {
			Runtime.Destroy();
			return;
		}

		auto PreviousCharacter = Character;
		LocalPlayer->ResetCharacter();
		Check(
			PreviousCharacter->GetDestroyed() && LocalPlayer->GetCharacter().has_value() &&
				*LocalPlayer->GetCharacter() != PreviousCharacter,
			"Player reset destroys the old character and synchronously assembles a replacement"
		);
		auto ReplacementCharacter = *LocalPlayer->GetCharacter();
		const auto ShutdownProbe = Runtime.ActionMap->BindKey(
			"ShutdownProbe", Enums::KeyCode::W, 1.0f, 0, false);
		auto ProbeBinding = Game->FindFirstDescendant("Binding" + std::to_string(ShutdownProbe));
		int DestroyingAttempts = 0, EndedAttempts = 0;
		int DestroyingHandle = -1, EndedHandle = -1;
		auto DestroyingConnection = ProbeBinding ? ProbeBinding->Destroying->Connect([&](auto) {
			++DestroyingAttempts;
			DestroyingHandle = Runtime.ActionMap->BindKey(
				"LateDestroyingBinding", Enums::KeyCode::W, 1.0f, 0, false);
		}) : nullptr;
		auto EndedConnection = Runtime.ActionMap->ActionEnded->Connect([&](std::string Name) {
			if (Name != "ShutdownProbe") return;
			++EndedAttempts;
			EndedHandle = Runtime.ActionMap->BindPointerDelta(
				"LateEndedBinding", Vector2(1.0f, 1.0f), 0, false);
		});
		(void)Runtime.ProcessEvent(Key(LogicalKey::W, ButtonState::Pressed));
		Check(ShutdownProbe > 0 && Runtime.ActionMap->GetBindingCount() > 0 &&
			ProbeBinding && Runtime.ActionMap->IsDown("ShutdownProbe") &&
			Runtime.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
			"runtime has live facade bindings and default modules before native shutdown");
		Runtime.Destroy();
		Check(
			ReplacementCharacter->GetDestroyed() && !Runtime.Players->GetLocalPlayer() &&
				Runtime.Players->GetPlayers().empty(),
			"runtime shutdown deterministically removes character and LocalPlayer lifetimes"
		);
		Check(Runtime.ActionMap->GetBindingCount() == 0,
			"native runtime shutdown releases default and extra ActionMap facade bindings after Lua stops");
		Check(DestroyingAttempts == 1 && EndedAttempts == 1 &&
			DestroyingHandle == 0 && EndedHandle == 0 &&
			!Game->FindFirstDescendant("LateDestroyingBinding") &&
			!Game->FindFirstDescendant("LateEndedBinding"),
			"native teardown observers cannot admit new ActionMap bindings after shutdown starts");
		Check(!Runtime.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
			"default module shutdown releases runtime Instances after Lua stops");
		Runtime.Destroy();
		Check(Runtime.ActionMap->GetBindingCount() == 0 &&
			!Runtime.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
			"repeated runtime shutdown leaves bindings and modules retired");
	}

	void TestEngineReattachAfterShutdown() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		std::shared_ptr<ActionMap> CanonicalMap;
		int FirstId = 0;
		{
			HeadlessRenderer FirstRenderer(Vector2(320.0f, 240.0f));
			Engine First(Game, &FirstRenderer);
			CanonicalMap = First.ActionMap;
			First.ProcessService->Alive = true;
			First.Step();
			First.Step();
			FirstId = First.ActionMap->BindKey("FirstRuntimeProbe", Enums::KeyCode::W, 1.0f, 0, false);
			Check(First.Players->GetLocalPlayer().has_value() && First.ActionMap->GetBindingCount() > 1 &&
				FirstId > 0, "first Engine admits default runtime and facade bindings on the shared DataModel");
			First.Destroy();
			Check(First.ActionMap->GetBindingCount() == 0 &&
				!First.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
				"first Engine shutdown retires adapter-owned bindings and default modules");
		}
		{
			HeadlessRenderer SecondRenderer(Vector2(320.0f, 240.0f));
			Engine Second(Game, &SecondRenderer);
			Second.ProcessService->Alive = true;
			Second.Step();
			Second.Step();
			const auto SecondId = Second.ActionMap->BindKey("SecondRuntimeProbe", Enums::KeyCode::D, 1.0f, 0, false);
			Check(Second.ActionMap == CanonicalMap && Second.Players->GetLocalPlayer().has_value() &&
				Second.ActionMap->GetBindingCount() > 1 && SecondId > FirstId &&
				Second.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
				"a new Engine on the same live DataModel restarts default bindings with monotonic facade IDs");
			Second.Destroy();
			Check(Second.ActionMap->GetBindingCount() == 0,
				"second Engine shutdown retires reopened facade bindings");
		}
	}

	void TestCustomControllerPath() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		HeadlessRenderer Renderer(Vector2(320.0f, 240.0f));
		std::vector<std::pair<std::string, std::string>> Diagnostics;
		Engine Runtime(Game, &Renderer, [&](std::string Severity, std::string Message) {
			Diagnostics.emplace_back(std::move(Severity), std::move(Message));
		});
		auto CustomRuntime = std::make_shared<Script>();
		CustomRuntime->SetName("CustomCharacterRuntime");
		CustomRuntime->SetSource(R"(
local Players = game:GetService("Players")
local Workspace = game:GetService("Workspace")
local Player = Players.LocalPlayer
local Character = Instance.new("KinematicCharacter")
Character.Name = "CustomCharacter"
Character.Position = Vector3.new(100, 6, 0)
Character.Parent = Workspace
local Root = Instance.new("Part")
Root.Name = "CustomRoot"
Root.Size = Vector3.new(2, 4, 2)
Root.Parent = Character
Character.RootPart = Root
Player.Character = Character
Character:Move(Vector3.new(1, 0, 0))
)" );
		CustomRuntime->SetParent(Game);
		Runtime.Script->Step();
		auto CustomCharacter = std::dynamic_pointer_cast<KinematicCharacter>(
			Game->FindFirstDescendant("CustomCharacter"));
		const bool CustomError = std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
			return Diagnostic.first == "Error" && Diagnostic.second.find("CustomCharacterRuntime") != std::string::npos;
		});
		Check(
			Runtime.ActionMap->GetBindingCount() == 2 && CustomCharacter &&
				(*Runtime.Players->GetLocalPlayer())->GetCharacter() == CustomCharacter &&
				Near(CustomCharacter->GetPosition().x, 101.0f) && !CustomError &&
				!Runtime.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
			"a game-provided Luau Character controller replaces disabled defaults and uses only native movement admission"
		);
		const auto PositionAfterCustomPolicy = CustomCharacter ? CustomCharacter->GetPosition() : glm::vec3(0.0f);
		for (int Frame = 0; Frame < 10; ++Frame)
			Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(
			CustomCharacter && Near(CustomCharacter->GetPosition().x, PositionAfterCustomPolicy.x),
			"native Character has no hidden default locomotion policy after the Luau defaults are disabled"
		);
		auto Capture = Runtime.ProcessEvent(
			PointerButtonEvent{{1}, PointerButton::Right, ButtonState::Pressed, {0.0f, 0.0f}}
		);
		Check(
			Capture.Command && std::get<SetRelativePointerMode>(*Capture.Command).Enabled,
			"legacy low-level Camera host routing remains usable for a custom controller"
		);
		(void)Runtime.ProcessEvent(Key(LogicalKey::W, ButtonState::Pressed));
		Check(
			Runtime.UserInputService->IsKeyDown(Enums::KeyCode::W),
			"custom controllers retain direct UserInputService physical state"
		);
		Runtime.Destroy();
	}

	void TestCharacterSchema() {
		using namespace gargantuan;
		const auto *CharacterDefinition = InstanceClassRegistry::GetDefinitionByName("Character");
		const auto *KinematicDefinition = InstanceClassRegistry::GetDefinitionByName("KinematicCharacter");
		auto FindCharacterProperty = [CharacterDefinition](std::string_view Name) {
			if (!CharacterDefinition) return static_cast<const InstanceProperty *>(nullptr);
			auto Found = CharacterDefinition->AllProperties.find(std::string(Name));
			return Found == CharacterDefinition->AllProperties.end() ? nullptr : Found->second;
		};
		auto CFrameProperty = FindCharacterProperty("CFrame");
		auto PositionProperty = FindCharacterProperty("Position");
		auto RootPartProperty = FindCharacterProperty("RootPart");
		Check(
			CharacterDefinition && CharacterDefinition->ClassName == "Character" &&
				CharacterDefinition->Superclass == std::optional<std::string>("Folder") &&
				KinematicDefinition && KinematicDefinition->Superclass == std::optional<std::string>("Character") &&
				CFrameProperty && PositionProperty && RootPartProperty,
			"runtime schema exposes Character as the canonical Folder-based semantic above KinematicCharacter"
		);
		Check(
			CFrameProperty && CFrameProperty->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated &&
				CFrameProperty->PersistencePolicy == InstanceProperty::Persistence::Saved &&
				RootPartProperty && RootPartProperty->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated &&
				RootPartProperty->PersistencePolicy == InstanceProperty::Persistence::Saved &&
				!InstanceClassRegistry::GetDefinitionByName("Humanoid"),
			"Character persists and future-replicates authority state without introducing a Humanoid aggregate"
		);
	}

	void TestMovementPolicyResults() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		auto CharacterValue = std::make_shared<KinematicCharacter>();
		CharacterValue->SetParent(WorkspaceValue);
		HeadlessRenderer Renderer(Vector2(320.0f, 240.0f));
		Engine Runtime(Game, &Renderer, nullptr,
			{.AudioEnabled = false, .Mode = RuntimeMode::NetworkServer});
		auto Policy = std::make_shared<Script>();
		Policy->SetRunContext(Enums::RunContext::Server);
		Policy->SetSource(R"(
local Calls = 0
game:GetService("CharacterControlService"):SetMovementPolicy(function()
	Calls += 1
	if Calls == 1 then
		return nil, Vector3.zero, 0
	elseif Calls == 2 then
		return Vector3.zero, {}, 0
	elseif Calls == 3 then
		return Vector3.zero, Vector3.zero
	end
	return Vector3.xAxis, Vector3.zero, 0.25
end)
)");
		Policy->SetParent(Game);
		Runtime.Script->Step();
		auto *Main = Runtime.Script->L;
		const auto StackBase = lua_gettop(Main);
		const network::CharacterInputCommand Command{};
		int Rejected = 0;
		for (int Call = 0; Call < 3; ++Call) {
			try {
				(void)Runtime.CharacterControl->EvaluateMovement(Command, *CharacterValue);
			} catch (const std::runtime_error &) {
				++Rejected;
			}
			Check(lua_gettop(Main) == StackBase, "malformed movement policy results leave the main VM stack intact");
		}
		Check(Rejected == 3, "malformed movement policy results produce a native policy error");
		const auto Motion = Runtime.CharacterControl->EvaluateMovement(Command, *CharacterValue);
		Check(Motion.Translation == glm::vec3(1.0f, 0.0f, 0.0f) && Motion.Velocity == glm::vec3(0.0f) &&
				Motion.YawRadians == 0.25f && lua_gettop(Main) == StackBase,
			"a valid movement policy result is admitted after malformed results");
		Runtime.Destroy();
	}

	void TestCharacterRemovalReservations() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		auto Owner = PlayersValue->CreateSessionPlayer({"test", "owner"});
		auto Other = PlayersValue->CreateSessionPlayer({"test", "other"});
		const auto CreateCharacter = [&] {
			auto Actor = std::make_shared<KinematicCharacter>();
			Actor->SetParent(WorkspaceValue);
			return Actor;
		};
		auto Previous = CreateCharacter();
		auto Replacement = CreateCharacter();
		Owner->SetCharacter(Previous);
		auto Aborting = Owner->CharacterRemoving->Once([](const auto &) {
			throw std::runtime_error("abort removal callback");
		});
		bool Aborted = false;
		try {
			Owner->SetCharacter(Replacement);
		} catch (const std::runtime_error &) {
			Aborted = true;
		}
		Check(Aborted && Owner->GetCharacter() == Previous && !Previous->GetDestroyed(),
			"an interrupted removal leaves the previous Character owned and live");
		Other->SetPossession(Replacement);
		Check(Other->GetPossessedEntity() == Replacement,
			"an interrupted removal releases the pending Character reservation");
		Other->SetPossession(std::nullopt);

		bool ObservedPrevious = false;
		int Rejected = 0;
		auto Removing = Owner->CharacterRemoving->Once([&](const auto &Value) {
			ObservedPrevious = Value == Previous && Owner->GetCharacter() == Previous && !Previous->GetDestroyed();
			try { Other->SetCharacter(Replacement); } catch (const std::invalid_argument &) { ++Rejected; }
			try { Other->SetPossession(Replacement); } catch (const std::invalid_argument &) { ++Rejected; }
			try { Owner->ResetCharacter(); } catch (const std::runtime_error &) { ++Rejected; }
		});
		auto Final = CreateCharacter();
		bool AddedReplacement = false;
		auto Added = Owner->CharacterAdded->Once([&](const auto &Value) {
			AddedReplacement = Value == Replacement && Owner->GetCharacter() == Replacement;
			Owner->SetCharacter(Final);
		});
		Owner->SetCharacter(Replacement);
		Check(ObservedPrevious && Rejected == 3 && !Other->GetCharacter() && !Other->GetPossessedEntity(),
			"pre-removal observers see the previous Character and cannot reassign the reserved replacement");
		Check(AddedReplacement && Owner->GetCharacter() == Final && Previous->GetDestroyed() && Replacement->GetDestroyed(),
			"post-commit CharacterAdded can still install a custom template");
		Owner->RemoveCharacter();
		Check(Final->GetDestroyed() && !Owner->GetCharacter(), "final teardown retires the single owned Character");
		Game->Destroy();
	}

	void TestCharacterCommitObservers() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		auto Owner = PlayersValue->CreateSessionPlayer({"test", "owner"});
		auto Other = PlayersValue->CreateSessionPlayer({"test", "other"});
		const auto CreateCharacter = [&] {
			auto Actor = std::make_shared<KinematicCharacter>();
			Actor->SetParent(WorkspaceValue);
			return Actor;
		};

		auto Previous = CreateCharacter();
		auto Intermediate = CreateCharacter();
		auto Final = CreateCharacter();
		Owner->SetCharacter(Previous);
		std::vector<std::shared_ptr<Character>> AddedCharacters;
		auto Added = Owner->CharacterAdded->Connect([&](const auto &Value) { AddedCharacters.push_back(Value); });
		auto Changed = Owner->GetPropertyChangedSignal("Character");
		int RetiringRejections = 0;
		auto ReplaceOnCommit = Changed->Once([&](std::monostate) {
			Check(Owner->GetCharacter() == Intermediate, "property observer sees the committed Character");
			try { Owner->SetCharacter(Previous); } catch (const std::invalid_argument &) { ++RetiringRejections; }
			try { Other->SetCharacter(Previous); } catch (const std::invalid_argument &) { ++RetiringRejections; }
			try { Other->SetPossession(Previous); } catch (const std::invalid_argument &) { ++RetiringRejections; }
			Owner->SetCharacter(Final);
		});
		Owner->SetCharacter(Intermediate);
		Check(RetiringRejections == 3 && !Other->GetCharacter() && !Other->GetPossessedEntity(),
			"the previous Character stays reserved for retirement during property callbacks");
		Check(Owner->GetCharacter() == Final && Previous->GetDestroyed() && Intermediate->GetDestroyed() &&
				AddedCharacters.size() == 1 && AddedCharacters.front() == Final,
			"reentrant replacement publishes only the live addition and retires previous Characters");
		Final->Destroy();
		Check(!Owner->GetCharacter(), "reentrant replacement keeps the final Character destruction connection");

		auto DestroyedOnCommit = CreateCharacter();
		auto DestroyOnCommit = Changed->Once([&](std::monostate) { DestroyedOnCommit->Destroy(); });
		Owner->SetCharacter(DestroyedOnCommit);
		Check(!Owner->GetCharacter() && DestroyedOnCommit->GetDestroyed() && AddedCharacters.size() == 1,
			"destroying the committed Character in a property callback clears ownership and suppresses the stale addition");

		auto DestroyingPrevious = CreateCharacter();
		auto Displaced = CreateCharacter();
		auto ReplacesDisplaced = CreateCharacter();
		Owner->SetCharacter(DestroyingPrevious);
		auto ReplaceOnRetirement = DestroyingPrevious->Destroying->Once([&](std::monostate) {
			Owner->SetCharacter(ReplacesDisplaced);
		});
		Owner->SetCharacter(Displaced);
		Check(Owner->GetCharacter() == ReplacesDisplaced && Displaced->GetDestroyed() &&
				AddedCharacters.size() == 3 && AddedCharacters.back() == ReplacesDisplaced,
			"old Character destruction can supersede the replacement without publishing a stale addition");

		auto AfterFailure = CreateCharacter();
		auto ThrowOnCommit = Changed->Once([](std::monostate) { throw std::runtime_error("native observer failure"); });
		bool ReportedFailure = false;
		try {
			Owner->SetCharacter(AfterFailure);
		} catch (const std::runtime_error &Error) {
			ReportedFailure = std::string_view(Error.what()) == "native observer failure";
		}
		Check(ReportedFailure && ReplacesDisplaced->GetDestroyed() && Owner->GetCharacter() == AfterFailure,
			"a failed native property observer leaves the committed replacement tracked and retires the previous Character");
		AfterFailure->Destroy();
		Check(!Owner->GetCharacter(), "the committed Character retains its destruction connection after a callback fails");

		auto ExternalRetirement = CreateCharacter();
		auto RetiredChild = std::make_shared<Folder>();
		RetiredChild->SetParent(ExternalRetirement);
		Owner->SetCharacter(ExternalRetirement);
		auto ThrowOnExternalRemoval = Owner->CharacterRemoving->Once([](const auto &) {
			throw std::runtime_error("native removal observer failure");
		});
		ExternalRetirement->Destroy();
		Check(!Owner->GetCharacter() && ExternalRetirement->GetDestroyed() && !ExternalRetirement->GetParent() &&
				RetiredChild->GetDestroyed() && !RetiredChild->GetParent(),
			"external Character destruction retires its full hierarchy after a native removal observer fails");

		auto PlayerRetirement = CreateCharacter();
		Owner->SetCharacter(PlayerRetirement);
		auto ThrowOnPlayerRemoval = Owner->CharacterRemoving->Once([](const auto &) {
			throw std::runtime_error("native Player teardown observer failure");
		});
		Owner->Destroy();
		Check(!Owner->GetCharacter() && !Owner->GetParent() && PlayerRetirement->GetDestroyed() &&
				!PlayerRetirement->GetParent(),
			"Player destruction retires its Character and detaches both after a native observer fails");
		Game->Destroy();
	}

	void TestGenericPossession() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		auto Vehicle = MakePart(WorkspaceValue, "Vehicle", {0.0f, 3.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
		auto Group = std::make_shared<Folder>();
		Group->SetParent(WorkspaceValue);
		HeadlessRenderer Renderer(Vector2(320.0f, 240.0f));
		Engine Runtime(Game, &Renderer);
		auto Local = PlayersValue->GetLocalPlayer().value();
		auto Second = PlayersValue->CreateSessionPlayer({"test", "second"});
		auto ScriptValue = std::make_shared<Script>();
		ScriptValue->SetRunContext(Enums::RunContext::Server);
		ScriptValue->SetSource(R"(
local Player = game:GetService("Players").LocalPlayer
local Vehicle = game:GetService("Workspace"):FindFirstChild("Vehicle")
Player:SetPossession(Vehicle)
assert(Player.PossessedEntity == Vehicle and Player.Character == nil)
)");
		ScriptValue->SetParent(Game);
		Runtime.Script->Step();
		Check(Local->GetPossessedEntity() == Vehicle, "ordinary server Luau possesses a BasePart");
		auto Client = std::make_shared<Script>();
		Client->SetRunContext(Enums::RunContext::Client);
		Client->SetSource(R"(
local Player = game:GetService("Players").LocalPlayer
local Vehicle = game:GetService("Workspace"):FindFirstChild("Vehicle")
assert(Player.PossessedEntity == Vehicle)
local MethodAllowed = pcall(function() Player:SetPossession(nil) end)
local PropertyAllowed = pcall(function() Player.PossessedEntity = nil end)
assert(not MethodAllowed and not PropertyAllowed)
Vehicle:SetAttribute("DeniedClientPossession", true)
)");
		Client->SetParent(Game);
		Runtime.Script->Step();
		Check(Vehicle->GetAttributeValue("DeniedClientPossession") == std::optional<WireValue>(true) &&
				Local->GetPossessedEntity() == Vehicle,
			"Luau clients can inspect possession and cannot release it or directly assign its property");

		bool DuplicateRejected = false;
		try {
			Second->SetPossession(Vehicle);
		} catch (const std::invalid_argument &) {
			DuplicateRejected = true;
		}
		auto ForeignGame = std::make_shared<DataModel>();
		auto ForeignWorld = std::dynamic_pointer_cast<Workspace>(ForeignGame->GetService("Workspace"));
		auto Foreign = MakePart(ForeignWorld, "Foreign", {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
		bool ForeignRejected = false;
		try {
			Local->SetPossession(Foreign);
		} catch (const std::invalid_argument &) {
			ForeignRejected = true;
		}
		Check(DuplicateRejected && ForeignRejected && !Second->GetPossessedEntity() &&
				Local->GetPossessedEntity() == Vehicle,
			"duplicate and foreign targets are rejected without changing current possession");

		auto StockCharacter = std::make_shared<KinematicCharacter>();
		StockCharacter->SetParent(WorkspaceValue);
		Second->SetCharacter(StockCharacter);
		auto StockDuplicateScript = std::make_shared<Script>();
		StockDuplicateScript->SetRunContext(Enums::RunContext::Server);
		StockDuplicateScript->SetSource(R"(
local Players = game:GetService("Players")
local LocalPlayer = Players.LocalPlayer
local AssignedCharacter
for _, Player in Players:GetPlayers() do
	if Player ~= LocalPlayer and Player.Character then
		AssignedCharacter = Player.Character
		break
	end
end
assert(AssignedCharacter and AssignedCharacter ~= LocalPlayer.Character)
local Assigned = pcall(function() LocalPlayer.Character = AssignedCharacter end)
LocalPlayer:RemoveCharacter()
LocalPlayer:SetAttribute("RejectedDuplicateCharacter", not Assigned)
)");
		StockDuplicateScript->SetParent(Game);
		Runtime.Script->Step();
		Check(Local->GetAttributeValue("RejectedDuplicateCharacter") == std::optional<WireValue>(true) &&
				!Local->GetCharacter() && Second->GetCharacter() == StockCharacter && !StockCharacter->GetDestroyed(),
			"ordinary server Luau cannot take ownership of another Player's Character or destroy it through removal");
		bool StockTargetRejected = false;
		try {
			Local->SetPossession(StockCharacter);
		} catch (const std::invalid_argument &) {
			StockTargetRejected = true;
		}
		Second->RemoveCharacter();
		Check(StockCharacter->GetDestroyed() && !Second->GetCharacter(),
			"the owning Player still retires its Character");
		auto CustomCharacter = std::make_shared<KinematicCharacter>();
		CustomCharacter->SetParent(WorkspaceValue);
		Second->SetPossession(CustomCharacter);
		bool ExplicitTargetRejected = false;
		try {
			Local->SetCharacter(CustomCharacter);
		} catch (const std::invalid_argument &) {
			ExplicitTargetRejected = true;
		}
		Check(StockTargetRejected && ExplicitTargetRejected && !Local->GetCharacter() &&
				Local->GetPossessedEntity() == Vehicle && Second->GetPossessedEntity() == CustomCharacter,
			"stock and explicit possession reject a target currently owned by another Player");
		Second->SetPossession(std::nullopt);
		Local->SetCharacter(CustomCharacter);
		Check(Local->GetCharacter() == CustomCharacter && !CustomCharacter->GetDestroyed(),
			"a Character remains assignable after its other Player releases explicit possession");
		Local->RemoveCharacter();

		bool ReentrantRejected = false;
		auto Changed = Local->GetPropertyChangedSignal("PossessedEntity")->Connect([&](auto) {
			try {
				Local->SetPossession(Vehicle);
			} catch (const std::runtime_error &) {
				ReentrantRejected = true;
			}
		});
		const auto InitialRevision = Local->GetPossessionRevision();
		Local->SetPossession(std::nullopt);
		Check(ReentrantRejected && !Local->GetPossessedEntity() && !Vehicle->GetDestroyed(),
			"release preserves the entity and rejects synchronous reassignment from its notification");
		Changed->Disconnect();
		Local->SetPossession(Vehicle);
		Check(Local->GetPossessionRevision() == InitialRevision + 2,
			"same-entity reacquisition has a fresh possession revision");
		Vehicle->SetParent(Group);
		Group->SetParent(std::nullopt);
		Check(!Local->GetPossessedEntity(), "moving an ancestor out of Workspace revokes possession");
		Group->SetParent(WorkspaceValue);
		Second->SetPossession(Vehicle);
		const auto RetiredId = Vehicle->GetObjectId();
		Vehicle->Destroy();
		auto Replacement = MakePart(WorkspaceValue, "Replacement", {0.0f, 3.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
		Check(!Second->GetPossessedEntity() && !ObjectRegistry::Get().Lookup(RetiredId) &&
				Replacement->GetObjectId() != RetiredId,
			"destroyed entity identity cannot resolve or possess a replacement");

		Second->SetPossession(Replacement);
		bool RemovingObservedRelease = false;
		auto Removing = PlayersValue->PlayerRemoving->Connect([&](const auto &PlayerValue) {
			if (PlayerValue == Second) RemovingObservedRelease = !Second->GetPossessedEntity();
		});
		Check(PlayersValue->RemoveSessionPlayer(Second) && RemovingObservedRelease &&
				!Replacement->GetDestroyed(),
			"disconnect revokes possession before PlayerRemoving and preserves the controlled entity");
		Removing->Disconnect();
		Runtime.Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Exception) {
		std::cerr << "Runtime schema bootstrap failed: " << Exception.what() << '\n';
		return 1;
	}
	TestActionMapContract();
	TestPlayerGuiLifecycle();
	TestPlayerGuiScriptWaitRetirement();
	TestCharacterSchema();
	TestDefaultPlayerRuntime();
	TestEngineReattachAfterShutdown();
	TestCustomControllerPath();
	TestMovementPolicyResults();
	TestCharacterRemovalReservations();
	TestCharacterCommitObservers();
	TestGenericPossession();
	if (Failures == 0) std::cout << "All player runtime tests passed\n";
	return Failures == 0 ? 0 : 1;
}

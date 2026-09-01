#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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
													   : PhysicalKey::Unknown,
			.Logical = Logical,
			.State = State,
		};
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
			Runtime.ActionMap->GetBindingCount() == 9,
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
		(void)Runtime.ProcessEvent(JumpDown);
		Runtime.RunService->PreSimulation->Fire(1.0f / 60.0f);
		Check(
			!Character->GetGrounded() && Character->GetVelocity().y > 0.0f,
			"grounded Jump action launches the default character"
		);
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
		Runtime.Destroy();
		Check(
			ReplacementCharacter->GetDestroyed() && !Runtime.Players->GetLocalPlayer() &&
				Runtime.Players->GetPlayers().empty(),
			"runtime shutdown deterministically removes character and LocalPlayer lifetimes"
		);
		Check(
			Runtime.ActionMap->GetBindingCount() == 0 &&
				!Runtime.Players->FindFirstChild("PlayerRuntimeModules", std::nullopt),
			"default module shutdown releases bindings, signals, and runtime Instances"
		);
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
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Exception) {
		std::cerr << "Runtime schema bootstrap failed: " << Exception.what() << '\n';
		return 1;
	}
	TestActionMapContract();
	TestCharacterSchema();
	TestDefaultPlayerRuntime();
	TestCustomControllerPath();
	if (Failures == 0) std::cout << "All player runtime tests passed\n";
	return Failures == 0 ? 0 : 1;
}

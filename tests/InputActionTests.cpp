#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/InputAction.hpp"
#include "gargantuan/classes/InputBinding.hpp"
#include "gargantuan/classes/InputContext.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/ActionMap.hpp"
#include "gargantuan/services/InputService.hpp"
#include "gargantuan/services/UserInputService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;
	int Failures = 0;

	void Check(bool Condition, std::string_view Message) {
		if (Condition) return;
		++Failures;
		std::cerr << "[Input:FoundationTest] " << Message << '\n';
	}

	struct Fixture {
		std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
		std::shared_ptr<InputService> Input = std::dynamic_pointer_cast<InputService>(
			World->GetService("InputService")
		);
		std::shared_ptr<UserInputService> Physical = std::dynamic_pointer_cast<UserInputService>(
			World->GetService("UserInputService")
		);
		Fixture() {
			World->SetArchivable(true);
			Input->SetArchivable(true);
		}

		std::shared_ptr<InputContext> Context(std::string Name = "Gameplay", int Priority = 100, bool Consume = true) {
			auto Value = std::make_shared<InputContext>();
			Value->SetArchivable(true);
			Value->SetName(std::move(Name));
			Value->SetPriority(Priority);
			Value->SetConsume(Consume);
			Value->SetParent(Input);
			return Value;
		}

		std::shared_ptr<InputAction> Action(const std::shared_ptr<InputContext> &Parent, std::string Name = "Save") {
			auto Value = std::make_shared<InputAction>();
			Value->SetArchivable(true);
			Value->SetName(std::move(Name));
			Value->SetParent(Parent);
			return Value;
		}

		std::shared_ptr<InputBinding> Binding(const std::shared_ptr<InputAction> &Parent, Enums::KeyCode Code) {
			auto Value = std::make_shared<InputBinding>();
			Value->SetArchivable(true);
			Value->SetKeyCode(Code);
			Value->SetParent(Parent);
			return Value;
		}

		bool Key(LogicalKey Code, ButtonState State, bool GuiConsumed = false, bool Repeat = false) {
			const HostEvent Event = KeyEvent{.Device = {1}, .Logical = Code, .State = State, .Repeat = Repeat};
			(void)Physical->ProcessEvent(Event);
			return Input->ProcessEvent(Event, GuiConsumed);
		}
	};

	void TestPersistentHierarchyAndAuthority() {
		auto NewDocument = std::make_shared<DataModel>();
		NewDocument->InitializeLoadedProjectRevision();
		auto InputRoot = NewDocument->FindFirstChildOfClass("InputService", false);
		Check(
			InputRoot && InputRoot->GetArchivable() &&
				NewDocument->GetAuthoritativeRevision() == DataModel::InitialProjectRevision,
			"new authoring documents expose a persistent input root before their initial revision"
		);
		Fixture Test;
		auto Gameplay = Test.Context();
		auto Save = Test.Action(Gameplay);
		auto Binding = Test.Binding(Save, Enums::KeyCode::S);
		Binding->SetModifier1(Enums::KeyCode::LeftControl);
		Binding->SetModifier2(Enums::KeyCode::LeftShift);
		Binding->SetExactModifiers(true);
		Check(Test.World->GetService("InputService") == Test.Input, "InputService is the canonical singleton");
		for (const auto Name : {"InputContext", "InputAction", "InputBinding"}) {
			auto InstanceValue = InstanceClassRegistry::ConstructByName(Name);
			Check(
				InstanceValue && InstanceValue->IsA(Name), "input definitions are constructible schema-backed Instances"
			);
		}
		std::shared_ptr<Instance> Root = Test.World;
		const auto Bytes = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		std::istringstream Stream(Bytes);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Stream);
		Check(Loaded.Ok, "canonical input hierarchy saves and reopens with the current project schema");
		auto LoadedWorld = std::dynamic_pointer_cast<DataModel>(Loaded.Instance);
		auto LoadedInput = LoadedWorld ? LoadedWorld->GetService("InputService") : nullptr;
		auto LoadedContext = LoadedInput ? std::dynamic_pointer_cast<InputContext>(
											   LoadedInput->FindFirstChild("Gameplay", false)
										   )
										 : nullptr;
		auto LoadedAction = LoadedContext
								? std::dynamic_pointer_cast<InputAction>(LoadedContext->FindFirstChild("Save", false))
								: nullptr;
		auto LoadedBinding = LoadedAction ? std::dynamic_pointer_cast<InputBinding>(
												LoadedAction->FindFirstChildOfClass("InputBinding", false)
											)
										  : nullptr;
		Check(
			LoadedContext && LoadedContext->GetPriority() == 100 && LoadedAction && LoadedBinding &&
				LoadedBinding->GetKeyCode() == Enums::KeyCode::S &&
				LoadedBinding->GetModifier1() == Enums::KeyCode::LeftControl &&
				LoadedBinding->GetModifier2() == Enums::KeyCode::LeftShift && LoadedBinding->GetExactModifiers(),
			"reopened definitions retain class, parenting, priority and full chord configuration"
		);
		Check(
			LoadedBinding && LoadedBinding->GetObjectId() != Binding->GetObjectId(),
			"reopen creates a fresh runtime identity instead of reusing a prior ObjectId"
		);

		MutationGateway Gateway;
		const auto Authority = MutationAuthorityContext::Studio(
			ScriptSecurityContext::StudioCoreUi(), Test.World->GetObjectId()
		);
		Check(
			Gateway.Apply(UpdatePropertyCommand{Gameplay->GetObjectId(), "Priority", 250}, Authority).Succeeded(),
			"Studio authoring authority edits an input property through the mutation gateway"
		);
		Check(
			Gameplay->GetPriority() == 250 &&
				Gateway.Undo(*Test.World, ScriptSecurityContext::StudioCoreUi()).Succeeded() &&
				Gameplay->GetPriority() == 100 &&
				Gateway.Redo(*Test.World, ScriptSecurityContext::StudioCoreUi()).Succeeded() &&
				Gameplay->GetPriority() == 250,
			"input authoring participates in existing Undo/Redo history"
		);
		Check(
			!Gateway.Apply(
						UpdatePropertyCommand{Gameplay->GetObjectId(), "Enabled", false},
						ScriptSecurityContext{ScriptExecutionDomain::Client, {ScriptCapability::ReadDataModel}}
			).Succeeded() &&
				Gameplay->GetEnabled(),
			"read-only gameplay capability cannot change authored context state"
		);
		const auto Stale = Binding->GetObjectId();
		Binding->Destroy();
		Check(
			!ObjectRegistry::Get().Lookup(Stale) &&
				!Gateway.Apply(UpdatePropertyCommand{Stale, "Enabled", true}).Succeeded(),
			"destroyed binding identity cannot mutate a replacement generation"
		);
	}

	void TestChordRepeatAndRelease() {
		Fixture Test;
		auto Context = Test.Context();
		auto Save = Test.Action(Context);
		auto Binding = Test.Binding(Save, Enums::KeyCode::S);
		Binding->SetModifier1(Enums::KeyCode::LeftControl);
		Binding->SetModifier2(Enums::KeyCode::LeftShift);
		int Pressed = 0;
		int Released = 0;
		auto PressConnection = Save->Pressed->Connect([&](std::monostate) { ++Pressed; });
		auto ReleaseConnection = Save->Released->Connect([&](std::monostate) { ++Released; });
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::LeftControl, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::LeftShift, ButtonState::Pressed);
		Check(
			!Save->IsDown() && Pressed == 0,
			"pressing modifiers after the primary does not retroactively fire a shortcut"
		);
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(
			Save->IsDown() && Save->GetValue() == 1 && Pressed == 1,
			"Ctrl+Shift+S fires once after its fresh primary press"
		);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed, false, true);
		Check(Pressed == 1, "key repeat cannot emit another Pressed transition");
		(void)Test.Key(LogicalKey::LeftShift, ButtonState::Released, true);
		Check(!Save->IsDown() && Released == 1, "GUI-consumed required modifier release cannot leave a shortcut stuck");
		(void)Test.Key(LogicalKey::LeftShift, ButtonState::Pressed);
		Check(!Save->IsDown(), "repressing a modifier while primary is held does not reactivate the action");
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		(void)Test.Key(LogicalKey::RightAlt, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(Save->IsDown(), "additional held modifiers are allowed by the default chord matcher");
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		Binding->SetExactModifiers(true);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(!Save->IsDown(), "exact chord matching rejects extra side-specific modifiers");
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		(void)Test.Key(LogicalKey::RightAlt, ButtonState::Released);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed, true);
		Check(
			!Save->IsDown() && Test.Physical->IsKeyDown(Enums::KeyCode::S),
			"GUI-consumed shortcut presses retain physical state and cannot activate gameplay"
		);
		(void)PressConnection;
		(void)ReleaseConnection;
	}

	void TestPriorityLifecycleAndRebinding() {
		Fixture Test;
		auto Gameplay = Test.Context("Gameplay", 100, true);
		auto Inventory = Test.Context("Inventory", 200, true);
		auto Move = Test.Action(Gameplay, "Move");
		auto Close = Test.Action(Inventory, "Close");
		auto MoveBinding = Test.Binding(Move, Enums::KeyCode::W);
		(void)Test.Binding(Close, Enums::KeyCode::W);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Close->IsDown() && !Move->IsDown(), "highest-priority consuming context owns conflicting input");
		Inventory->SetEnabled(false);
		Test.Input->EnsureCompiled();
		Check(
			!Close->IsDown() && !Move->IsDown(),
			"context disable cancels winner without activating held input underneath"
		);
		(void)Test.Key(LogicalKey::W, ButtonState::Released);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Move->IsDown(), "new press after context switch reaches enabled gameplay context");
		MoveBinding->SetKeyCode(Enums::KeyCode::D);
		Test.Input->EnsureCompiled();
		Check(!Move->IsDown(), "rebinding an active binding cancels its previous action state");
		(void)Test.Key(LogicalKey::D, ButtonState::Pressed);
		Check(Move->IsDown(), "new physical key activates the rebound canonical binding");
		auto Other = Test.Binding(Move, Enums::KeyCode::A);
		(void)Test.Key(LogicalKey::A, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::D, ButtonState::Released);
		Check(Move->IsDown(), "multiple bindings keep an action active until every source releases");
		Other->Destroy();
		Test.Input->EnsureCompiled();
		Check(!Move->IsDown(), "destroying the final active source retires semantic state");
		(void)Test.Key(LogicalKey::D, ButtonState::Pressed);
		Check(Move->IsDown(), "surviving binding remains functional after sibling destruction");
		const HostEvent LostFocus = FocusEvent{false};
		(void)Test.Physical->ProcessEvent(LostFocus);
		(void)Test.Input->ProcessEvent(LostFocus);
		Check(
			!Move->IsDown() && !Test.Physical->IsKeyDown(Enums::KeyCode::D),
			"focus loss clears both physical and action state"
		);
		(void)Test.Input->ProcessEvent(FocusEvent{true});
		Check(!Move->IsDown(), "focus regain cannot recreate an old held action");
		auto Detached = std::make_shared<Folder>();
		Detached->SetParent(Test.World);
		Move->SetParent(Detached);
		Test.Input->EnsureCompiled();
		(void)Test.Key(LogicalKey::D, ButtonState::Pressed);
		Check(!Move->IsDown(), "noncanonical hierarchy is excluded from input evaluation");
	}

	void TestContextTakeoverWhileHeld() {
		Fixture Test;
		auto Gameplay = Test.Context("Gameplay", 100, false);
		auto Move = Test.Action(Gameplay, "Move");
		(void)Test.Binding(Move, Enums::KeyCode::W);
		auto Map = std::dynamic_pointer_cast<ActionMap>(Test.World->GetService("ActionMap"));
		(void)Map->BindKey("LegacyMove", Enums::KeyCode::W, 1, 0, false);
		auto Inventory = Test.Context("Inventory", 200, true);
		Inventory->SetEnabled(false);
		auto Close = Test.Action(Inventory, "Close");
		(void)Test.Binding(Close, Enums::KeyCode::W);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(
			Move->IsDown() && Map->IsDown("LegacyMove"), "non-consuming gameplay and legacy contexts can share input"
		);
		Inventory->SetEnabled(true);
		Test.Input->EnsureCompiled();
		Check(
			!Move->IsDown() && !Map->IsDown("LegacyMove") && !Close->IsDown(),
			"higher context claims held input and cancels lower sources without inventing its own press"
		);
		Inventory->SetEnabled(false);
		Test.Input->EnsureCompiled();
		Check(
			!Move->IsDown() && !Map->IsDown("LegacyMove"),
			"suppressed canonical and legacy sources cannot resume while held"
		);
		(void)Test.Key(LogicalKey::W, ButtonState::Released);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Move->IsDown() && Map->IsDown("LegacyMove"), "fresh input restores lower contexts after takeover ends");
	}

	void TestFourModifiersAndHeldDefinitionChanges() {
		Fixture Test;
		auto Context = Test.Context();
		auto Action = Test.Action(Context);
		std::string Cancellation;
		auto CanceledConnection = Action->Canceled->Connect([&](std::string Reason) {
			Cancellation = std::move(Reason);
		});
		auto Binding = Test.Binding(Action, Enums::KeyCode::S);
		Binding->SetModifier1(Enums::KeyCode::LeftControl);
		Binding->SetModifier2(Enums::KeyCode::LeftShift);
		Binding->SetModifier3(Enums::KeyCode::LeftAlt);
		Binding->SetModifier4(Enums::KeyCode::LeftMeta);
		(void)Test.Key(LogicalKey::RightControl, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::LeftShift, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::LeftAlt, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::LeftMeta, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(!Action->IsDown(), "right and left modifier requirements remain distinct");
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		(void)Test.Key(LogicalKey::LeftControl, ButtonState::Pressed);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(Action->IsDown(), "a four-modifier chord is supported at the documented maximum");
		auto Unrelated = Test.Action(Context, "Unrelated");
		(void)Test.Binding(Unrelated, Enums::KeyCode::Z);
		Test.Input->EnsureCompiled();
		Check(Action->IsDown(), "incremental compilation preserves unaffected held bindings");
		Action->SetEnabled(false);
		Test.Input->EnsureCompiled();
		Check(
			!Action->IsDown() && Cancellation == "Disabled",
			"disabling an action cancels its held state with an inspectable reason"
		);
		Action->SetEnabled(true);
		Test.Input->EnsureCompiled();
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed, false, true);
		Check(!Action->IsDown(), "enabling an action while primary is held requires release and a fresh press");
		(void)Test.Key(LogicalKey::S, ButtonState::Released);
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(Action->IsDown(), "re-enabled action resumes on the next complete primary transition");
		Binding->SetEnabled(false);
		Test.Input->EnsureCompiled();
		Check(!Action->IsDown(), "disabling an active binding cancels its action");
		(void)Test.Key(LogicalKey::D, ButtonState::Pressed);
		Binding->SetKeyCode(Enums::KeyCode::D);
		Binding->SetEnabled(true);
		Test.Input->EnsureCompiled();
		Check(!Action->IsDown(), "rebinding to an already-held key cannot fire an action");
		(void)Test.Key(LogicalKey::D, ButtonState::Released);
		(void)Test.Key(LogicalKey::D, ButtonState::Pressed);
		Check(Action->IsDown(), "a released and repressed replacement key activates the rebound action");
		(void)CanceledConnection;
	}

	void TestInspectionValidationAndPublication() {
		Fixture Test;
		auto Low = Test.Context("Low", 10, true);
		auto High = Test.Context("High", 20, true);
		auto LowAction = Test.Action(Low, "LowAction");
		auto HighAction = Test.Action(High, "HighAction");
		auto LowBinding = Test.Binding(LowAction, Enums::KeyCode::S);
		(void)Test.Binding(HighAction, Enums::KeyCode::S);
		const auto Cursor = ChangeJournal::Get().CreateCursor(Test.World->GetObjectId());
		const auto Revision = Test.World->GetAuthoritativeRevision();
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		const auto Snapshot = Test.Input->Inspect();
		auto Suppressed = std::ranges::find_if(Snapshot.Bindings, [&](const auto &Value) {
			return Value.Definition.Id == LowBinding->GetObjectId();
		});
		Check(
			Snapshot.Contexts.size() == 2 && Snapshot.Actions.size() == 2 && Snapshot.Bindings.size() == 2 &&
				std::ranges::contains(Snapshot.PressedKeys, static_cast<std::uint16_t>(Enums::KeyCode::S)),
			"bounded inspection reports canonical definitions and current physical key state"
		);
		Check(
			Suppressed != Snapshot.Bindings.end() && Suppressed->Matched && Suppressed->Suppressed &&
				!Suppressed->Active && Suppressed->WinningContext == High->GetObjectId(),
			"inspection identifies a matching suppressed binding and exact winning context generation"
		);
		Check(
			ChangeJournal::Get().Read(Cursor).Records.empty() && Test.World->GetAuthoritativeRevision() == Revision,
			"input transitions and read-only inspection do not mutate the authoring journal or project revision"
		);
		const auto CompileRevision = Snapshot.Stats.Revision;
		Check(
			Test.Input->Inspect().Stats.Revision == CompileRevision,
			"repeated inspection does not rebuild unchanged definitions"
		);
		LowBinding->SetModifier1(Enums::KeyCode::LeftControl);
		LowBinding->SetModifier2(Enums::KeyCode::LeftControl);
		Test.Input->EnsureCompiled();
		Check(
			!Test.Input->GetValidationErrors().empty() && !LowAction->IsDown(),
			"invalid manually authored duplicate modifiers remain inspectable but cannot evaluate"
		);
		LowBinding->SetModifier2(Enums::KeyCode::None);
		Test.Input->EnsureCompiled();
		Check(Test.Input->GetValidationErrors().empty(), "correcting canonical configuration clears compiler errors");

		Fixture Reentrant;
		auto Context = Reentrant.Context("Shared", 0, false);
		auto First = Reentrant.Action(Context, "First");
		auto Second = Reentrant.Action(Context, "Second");
		(void)Reentrant.Binding(First, Enums::KeyCode::W);
		(void)Reentrant.Binding(Second, Enums::KeyCode::W);
		bool SawCommittedPeer = false;
		auto Connection = First->Pressed->Connect([&](std::monostate) {
			SawCommittedPeer = Second->IsDown();
			Context->SetEnabled(false);
		});
		(void)Reentrant.Key(LogicalKey::W, ButtonState::Pressed);
		Check(SawCommittedPeer, "callbacks observe every peer action value already committed for the event");
		Reentrant.Input->EnsureCompiled();
		Check(
			!First->IsDown() && !Second->IsDown(),
			"callback-authored changes cancel safely at the next compilation boundary"
		);
		(void)Connection;
	}

	void TestCompatibilityValueShapes() {
		Fixture Test;
		auto Context = Test.Context("Compatibility", 0, false);
		auto Scalar = Test.Action(Context, "Scalar");
		Scalar->SetType(Enums::InputActionType::Scalar);
		auto Positive = Test.Binding(Scalar, Enums::KeyCode::W);
		Positive->SetScale(0.5f);
		auto Negative = Test.Binding(Scalar, Enums::KeyCode::S);
		Negative->SetScale(-1.0f);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Scalar->IsDown() && Scalar->GetValue() == 0.5f, "scalar action retains the legacy finite scale contract");
		(void)Test.Key(LogicalKey::S, ButtonState::Pressed);
		Check(
			Scalar->IsDown() && Scalar->GetValue() == -0.5f,
			"multiple scalar binding values aggregate deterministically"
		);
		Test.Input->EndFrame();
		Check(Scalar->GetValue() == -0.5f, "frame-transient reset cannot clear held scalar values");
		auto Look = Test.Action(Context, "Look");
		Look->SetType(Enums::InputActionType::Vector2);
		auto Pointer = std::make_shared<InputBinding>();
		Pointer->SetType(Enums::InputBindingType::PointerDelta);
		Pointer->SetVectorScale(Vector2(2, -1));
		Pointer->SetParent(Look);
		(void)Test.Input->ProcessEvent(PointerMoveEvent{{1}, {20, 30}, {3, 4}});
		Check(Look->GetVector() == Vector2(6, -4), "canonical Vector2 action preserves scaled relative-pointer deltas");
		Test.Input->EndFrame();
		Check(Look->GetVector() == Vector2(), "canonical runtime inspection observes the frame-boundary Look reset");
		Pointer->SetParent(Scalar);
		Test.Input->EnsureCompiled();
		Check(
			!Test.Input->GetValidationErrors().empty(),
			"incompatible manual binding/action value types are diagnostics rather than escaping exceptions"
		);
	}

	void TestGraphBoundsAndTransactionalValidation() {
		using G = CompiledInputGraph;
		G Graph;
		std::vector<G::ContextDefinition> Contexts{{.Id = {1, 1}}};
		std::vector<G::ActionDefinition> Actions{{.Id = {2, 1}, .Context = {1, 1}}};
		std::vector<G::BindingDefinition> Bindings{
			{.Id = {3, 1}, .Action = {2, 1}, .Code = static_cast<std::uint16_t>(Enums::KeyCode::W)}
		};
		Graph.Compile(Contexts, Actions, Bindings);
		(void)Graph.ProcessEvent(KeyEvent{.Logical = LogicalKey::W, .State = ButtonState::Pressed});
		Check(
			Graph.GetState({2, 1}).Down && !Graph.GetState({2, 2}).Down,
			"compiled action identity includes the full object generation"
		);
		const auto Revision = Graph.GetStatistics().Revision;
		auto Reject = [&](std::string_view Message) {
			bool Rejected = false;
			try {
				Graph.Compile(Contexts, Actions, Bindings);
			} catch (const std::exception &) {
				Rejected = true;
			}
			Check(Rejected && Graph.GetState({2, 1}).Down && Graph.GetStatistics().Revision == Revision, Message);
		};
		Bindings[0].Action = {9, 1};
		Reject("unknown action reference rejects compilation without changing the live projection");
		Bindings[0].Action = {2, 1};
		Bindings[0].Id = {2, 1};
		Reject("duplicate cross-kind identity rejects compilation transactionally");
		Bindings[0].Id = {3, 1};
		Bindings[0].Code = 4095;
		Reject("undefined numeric key values cannot become hidden bindable inputs");
		Bindings[0].Code = static_cast<std::uint16_t>(Enums::KeyCode::W);
		Bindings[0].Modifiers = 0x1f;
		Reject("the native compiler independently rejects more than four normalized modifiers");
		Bindings[0].Modifiers = 0;
		Contexts.resize(G::MaximumContexts + 1);
		Reject("native graph capacity failure preserves the last coherent projection");

		Fixture Test;
		auto Context = Test.Context();
		auto Action = Test.Action(Context);
		(void)Test.Binding(Action, Enums::KeyCode::W);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Action->IsDown(), "capacity cancellation fixture begins active");
		std::shared_ptr<InputContext> Last;
		for (std::size_t Index = 0; Index < G::MaximumContexts; ++Index)
			Last = Test.Context("Extra" + std::to_string(Index), 0, false);
		const auto Snapshot = Test.Input->Inspect();
		Check(
			!Action->IsDown() && Snapshot.Contexts.empty() && Snapshot.Actions.empty() && Snapshot.Bindings.empty() &&
				!Test.Input->GetValidationErrors().empty(),
			"over-capacity canonical definitions fail closed with bounded diagnostics instead of evaluating a prefix"
		);
		Last->Destroy();
		Test.Input->EnsureCompiled();
		Check(
			Test.Input->GetValidationErrors().empty() && !Action->IsDown(),
			"removing excess definitions recovers compilation without firing a held key"
		);
		(void)Test.Key(LogicalKey::W, ButtonState::Released);
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(Action->IsDown(), "corrected graph resumes at the next fresh input transition");
	}

	void TestLegacyFacadeOrderingAndMixedValues() {
		Fixture Test;
		auto Map = std::dynamic_pointer_cast<ActionMap>(Test.World->GetService("ActionMap"));
		for (const auto Invalid : {Enums::KeyCode::None, static_cast<Enums::KeyCode>(4095)}) {
			bool Rejected = false;
			try {
				(void)Map->BindKey("Invalid", Invalid, 1, 0, false);
			} catch (const std::exception &) {
				Rejected = true;
			}
			Check(
				Rejected && Map->GetBindingCount() == 0,
				"legacy key binding rejects missing or undefined primary keys without publishing a binding"
			);
		}
		(void)Map->BindKey("A", Enums::KeyCode::W, 1, std::numeric_limits<int>::min(), false);
		(void)Map->BindKey("A", Enums::KeyCode::D, 1, 100, false);
		(void)Map->BindKey("B", Enums::KeyCode::W, 1, 50, false);
		std::vector<std::string> Order;
		auto Connection = Map->ActionBegan->Connect([&](std::string Name) { Order.push_back(std::move(Name)); });
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(
			Order == std::vector<std::string>{"B", "A"},
			"legacy publication priority uses bindings affected by this key, not an unrelated binding's priority"
		);
		(void)Test.Key(LogicalKey::W, ButtonState::Released);
		Map->UnbindAction("A");
		Map->UnbindAction("B");
		(void)Map->BindKey("Mixed", Enums::KeyCode::W, 0.5f, 0, false);
		(void)Map->BindPointerDelta("Mixed", Vector2(2, -1), 0, false);
		Vector2 ChangedValue;
		auto Changed = Map->ActionChanged->Connect([&](std::tuple<std::string, Vector2> Value) {
			if (std::get<0>(Value) == "Mixed") ChangedValue = std::get<1>(Value);
		});
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(ChangedValue == Vector2(0.5f, 0), "mixed legacy digital changes publish scalar payloads");
		(void)Test.Input->ProcessEvent(PointerMoveEvent{{1}, {20, 30}, {3, 4}});
		Check(ChangedValue == Vector2(6, -4), "mixed legacy pointer changes publish vector payloads");
		Check(
			Map->IsDown("Mixed") && Map->GetValue("Mixed") == 0.5f && Map->GetVector("Mixed") == Vector2(6, -4),
			"legacy mixed-source action exposes independent held scalar and frame pointer values"
		);
		Test.Input->EndFrame();
		Check(
			Map->GetValue("Mixed") == 0.5f && Map->GetVector("Mixed") == Vector2(),
			"legacy mixed-source frame reset preserves scalar input while clearing pointer delta"
		);
		(void)Test.Key(LogicalKey::W, ButtonState::Released);
		Check(
			!Map->IsDown("Mixed") && Map->GetValue("Mixed") == 0,
			"legacy mixed-source key release ends only its held scalar state"
		);
		(void)Connection;
		(void)Changed;
	}

	void TestReplicatedDefinitionsRemainLocalState() {
		Fixture Source;
		auto Context = Source.Context("Replicated", 123, true);
		auto Action = Source.Action(Context, "ReplicatedAction");
		Action->SetType(Enums::InputActionType::Scalar);
		auto Binding = Source.Binding(Action, Enums::KeyCode::S);
		Binding->SetModifier1(Enums::KeyCode::LeftControl);
		Binding->SetModifier2(Enums::KeyCode::LeftShift);
		Binding->SetModifier3(Enums::KeyCode::LeftAlt);
		Binding->SetModifier4(Enums::KeyCode::LeftMeta);
		Binding->SetScale(0.5f);
		for (const auto Key :
			 {LogicalKey::LeftControl, LogicalKey::LeftShift, LogicalKey::LeftAlt, LogicalKey::LeftMeta, LogicalKey::S})
			(void)Source.Key(Key, ButtonState::Pressed);
		Check(Action->IsDown() && Action->GetValue() == 0.5f, "replication fixture has active local chord state");
		const auto SnapshotValue = CaptureSnapshot(Source.World);
		const auto SavedAction = std::ranges::find_if(SnapshotValue.Objects, [&](const auto &Object) {
			return Object.Id == WireObjectId::FromObjectId(Action->GetObjectId());
		});
		Check(
			SavedAction != SnapshotValue.Objects.end() && SavedAction->Properties.contains("Enabled") &&
				SavedAction->Properties.contains("Type") && !SavedAction->Properties.contains("Down") &&
				!SavedAction->Properties.contains("RuntimeValue") && !SavedAction->Properties.contains("Value"),
			"canonical structural snapshot carries authored configuration and excludes transient action values"
		);
		const auto Started = InProcessReplicationSession::Start(Source.World);
		Check(Started.Succeeded(), "input hierarchy materializes through the unchanged loopback snapshot protocol");
		if (!Started.Succeeded()) {
			for (const auto &Error : Started.Errors)
				std::cerr << "[Input:ReplicationTest] " << Error << '\n';
			return;
		}
		const auto &Session = Started.Session;
		auto ReceiverContext = std::dynamic_pointer_cast<InputContext>(
			Session->ResolveReceiver(WireObjectId::FromObjectId(Context->GetObjectId()))
		);
		auto ReceiverAction = std::dynamic_pointer_cast<InputAction>(
			Session->ResolveReceiver(WireObjectId::FromObjectId(Action->GetObjectId()))
		);
		auto ReceiverBinding = std::dynamic_pointer_cast<InputBinding>(
			Session->ResolveReceiver(WireObjectId::FromObjectId(Binding->GetObjectId()))
		);
		Check(
			ReceiverContext && ReceiverAction && ReceiverBinding && ReceiverContext->GetPriority() == 123 &&
				ReceiverContext->GetEnabled() && ReceiverContext->GetConsume() &&
				ReceiverAction->GetType() == Enums::InputActionType::Scalar && ReceiverAction->GetEnabled() &&
				ReceiverBinding->GetKeyCode() == Enums::KeyCode::S &&
				ReceiverBinding->GetModifier1() == Enums::KeyCode::LeftControl &&
				ReceiverBinding->GetModifier2() == Enums::KeyCode::LeftShift &&
				ReceiverBinding->GetModifier3() == Enums::KeyCode::LeftAlt &&
				ReceiverBinding->GetModifier4() == Enums::KeyCode::LeftMeta,
			"snapshot materialization preserves all four modifier slots and context/action configuration"
		);
		Check(
			ReceiverAction && !ReceiverAction->IsDown() && ReceiverAction->GetValue() == 0 &&
				ReceiverAction->GetVector() == Vector2(),
			"sender pressed keys and action state never become receiver input authority"
		);
		if (!ReceiverContext || !ReceiverAction || !ReceiverBinding) return;
		Context->SetPriority(321);
		Context->SetEnabled(false);
		Context->SetConsume(false);
		Action->SetEnabled(false);
		Action->SetType(Enums::InputActionType::Vector2);
		Binding->SetEnabled(false);
		Binding->SetKeyCode(Enums::KeyCode::D);
		Binding->SetModifier1(Enums::KeyCode::RightControl);
		Binding->SetModifier2(Enums::KeyCode::RightShift);
		Binding->SetModifier3(Enums::KeyCode::RightAlt);
		Binding->SetModifier4(Enums::KeyCode::RightMeta);
		Binding->SetExactModifiers(true);
		Binding->SetScale(-0.25f);
		Binding->SetVectorScale(Vector2(3, -2));
		Binding->SetPriority(-5);
		Binding->SetConsume(true);
		const auto Applied = Session->ApplyAvailable();
		Check(Applied.Succeeded(), "input definition changes apply through ordinary ordered replication deltas");
		if (!Applied.Succeeded()) std::cerr << "[Input:ReplicationTest] " << Applied.Message << '\n';
		Check(
			ReceiverContext->GetPriority() == 321 && !ReceiverContext->GetEnabled() && !ReceiverContext->GetConsume() &&
				!ReceiverAction->GetEnabled() && ReceiverAction->GetType() == Enums::InputActionType::Vector2 &&
				!ReceiverBinding->GetEnabled() && ReceiverBinding->GetKeyCode() == Enums::KeyCode::D &&
				ReceiverBinding->GetModifier1() == Enums::KeyCode::RightControl &&
				ReceiverBinding->GetModifier2() == Enums::KeyCode::RightShift &&
				ReceiverBinding->GetModifier3() == Enums::KeyCode::RightAlt &&
				ReceiverBinding->GetModifier4() == Enums::KeyCode::RightMeta && ReceiverBinding->GetExactModifiers() &&
				ReceiverBinding->GetScale() == -0.25f && ReceiverBinding->GetVectorScale() == Vector2(3, -2) &&
				ReceiverBinding->GetPriority() == -5 && ReceiverBinding->GetConsume(),
			"replication metadata preserves every authored binding field without a new wire representation"
		);
		Source.Input->EnsureCompiled();
		(void)Source.Input->ProcessEvent(FocusEvent{false});
		Check(
			Session->ApplyAvailable().Status == ReplicationApplyStatus::NoChanges && !ReceiverAction->IsDown(),
			"local compilation and focus cancellation create no replicated runtime-state mutations"
		);
		const auto BindingWireId = WireObjectId::FromObjectId(Binding->GetObjectId());
		Binding->Destroy();
		Check(
			Session->ApplyAvailable().Succeeded() && !Session->ResolveReceiver(BindingWireId),
			"normal binding destruction retires the receiver identity through existing structural replication"
		);
	}

	void TestLegacyNamesFollowInstanceLifecycle() {
		Fixture Test;
		auto Context = Test.Context("Authored", 100, false);
		auto Action = Test.Action(Context, "Before");
		(void)Test.Binding(Action, Enums::KeyCode::W);
		auto Map = std::dynamic_pointer_cast<ActionMap>(Test.World->GetService("ActionMap"));
		(void)Map->IsDown("Before");
		std::vector<std::string> Events;
		auto Began = Map->ActionBegan->Connect([&](std::string Name) { Events.push_back("Begin:" + Name); });
		auto Ended = Map->ActionEnded->Connect([&](std::string Name) { Events.push_back("End:" + Name); });
		(void)Test.Key(LogicalKey::W, ButtonState::Pressed);
		Check(
			Map->IsDown("Before") && Events == std::vector<std::string>{"Begin:Before"},
			"legacy facade observes an authored action through the canonical evaluator"
		);
		Events.clear();
		Action->SetName("After");
		Check(
			!Map->IsDown("Before") && Map->IsDown("After") &&
				Events == std::vector<std::string>{"End:Before", "Begin:After"},
			"name-cache reconciliation ends the previous name and exposes the renamed held action once"
		);
		Events.clear();
		const auto Stale = Action->GetObjectId();
		Action->Destroy();
		Check(
			!Map->IsDown("After") && Events == std::vector<std::string>{"End:After"} &&
				!ObjectRegistry::Get().Lookup(Stale),
			"name-cache reconciliation retires destroyed authored action identity without retaining a held semantic "
			"name"
		);
		(void)Began;
		(void)Ended;
	}

	void TestDefinitionObserversRemainBounded() {
		Fixture Test;
		auto Context = Test.Context();
		auto Action = Test.Action(Context);
		auto Binding = Test.Binding(Action, Enums::KeyCode::W);
		auto KeyChanged = Binding->GetPropertyChangedSignal("KeyCode");
		int UserNotifications = 0;
		auto UserConnection = KeyChanged->Connect([&](std::monostate) { ++UserNotifications; });
		const auto UserCount = KeyChanged->Connections.size();
		Test.Input->EnsureCompiled();
		const auto ObservedCount = KeyChanged->Connections.size();
		Check(ObservedCount == UserCount + 1, "compilation installs one owned observer per definition property");
		for (int Index = 0; Index < 128; ++Index) {
			Context->SetPriority(Index);
			Test.Input->EnsureCompiled();
		}
		Check(
			KeyChanged->Connections.size() == ObservedCount,
			"repeated incremental compilation cannot accumulate disconnected property observers"
		);
		for (int Index = 0; Index < 16; ++Index) {
			Binding->SetParent(Test.World);
			Test.Input->EnsureCompiled();
			Check(
				KeyChanged->Connections.size() == UserCount && UserConnection->Connected,
				"leaving the canonical graph removes only the service-owned definition observer"
			);
			Binding->SetParent(Action);
			Test.Input->EnsureCompiled();
			Check(
				KeyChanged->Connections.size() == ObservedCount,
				"re-entering the canonical graph installs one observer without stale duplicates"
			);
		}
		Binding->SetKeyCode(Enums::KeyCode::D);
		Test.Input->EnsureCompiled();
		Check(
			UserNotifications == 1 && KeyChanged->Connections.size() == ObservedCount,
			"observer retirement preserves unrelated author callbacks and their exact notification count"
		);
	}

	int Run(ScriptEngine &Engine, std::string_view Source) {
		size_t Size = 0;
		auto Options = Engine.CompileOptions;
		Options.debugLevel = 1;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Options, &Size);
		if (!Bytecode) return LUA_ERRSYNTAX;
		const auto Loaded = luau_load(Engine.L, "input-foundation-test", Bytecode, Size, 0);
		std::free(Bytecode);
		return Loaded == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : Loaded;
	}

	void TestLuauSurface() {
		Fixture Test;
		ScriptEngine Engine(Test.World);
		const auto Status = Run(Engine, R"(
local Input = game:GetService("InputService")
local Gameplay = Input:AddContext("Gameplay", { Priority = 100 })
local Save = Gameplay:AddAction("Save", { Type = Enum.InputActionType.Bool })
assert(Save.Type == Enum.InputActionType.Bool and Enum.InputActionType.Bool ~= Enum.InputBindingType.Key)
assert(not pcall(function() Save.Type = Enum.InputBindingType.Key end))
local Chord = Save:AddBinding(Enum.KeyCode.S, {
    Modifiers = { Enum.KeyCode.LeftControl, Enum.KeyCode.LeftShift },
})
assert(Gameplay.Parent == Input and Save.Parent == Gameplay and Chord.Parent == Save)
assert(Chord.Modifier1 == Enum.KeyCode.LeftControl and Chord.Modifier2 == Enum.KeyCode.LeftShift)
assert(not pcall(function() Chord.KeyCode = Enum.InputActionType.Bool end))
local Manual = Instance.new("InputContext")
Manual.Name = "Inventory"
Manual.Priority = 200
Manual.Parent = Input
local Close = Instance.new("InputAction")
Close.Name = "Close"
Close.Parent = Manual
local Escape = Instance.new("InputBinding")
Escape.KeyCode = Enum.KeyCode.Escape
Escape.Parent = Close
assert(Input:FindFirstChild("Inventory") == Manual and Close:IsA("InputAction"))
assert(not pcall(function() Save:AddBinding(Enum.KeyCode.S, { Modifiers = {
    Enum.KeyCode.LeftControl, Enum.KeyCode.LeftControl,
} }) end))
assert(not pcall(function() Save:AddBinding(Enum.KeyCode.S, { Modifiers = {
    Enum.KeyCode.LeftControl, Enum.KeyCode.LeftShift, Enum.KeyCode.LeftAlt,
    Enum.KeyCode.LeftMeta, Enum.KeyCode.RightControl,
} }) end))
assert(not pcall(function() Save:AddBinding(Enum.KeyCode.S, { Modifiers = { Enum.KeyCode.S } }) end))
assert(not pcall(function() Save:AddBinding(Enum.KeyCode.None) end))
assert(not pcall(function() Save:AddBinding(Enum.KeyCode.S, { UnknownOption = true }) end))
assert(#Save:GetChildren() == 1)
local Snapshot = Input:GetRuntimeSnapshot()
assert(Snapshot.Version == 1 and #Snapshot.Contexts == 2)
Snapshot.Contexts[1].Priority = 99999
assert(Input:GetRuntimeSnapshot().Contexts[1].Priority == 100)
)");
		if (Status != LUA_OK) std::cerr << "[Input:LuauTest] " << lua_tostring(Engine.L, -1) << '\n';
		Check(
			Status == LUA_OK,
			"Luau constructors create canonical Instances and reject malformed bounded modifiers atomically"
		);
		lua_settop(Engine.L, 0);
	}

	void TestDefaultBindingsPreserveAuthoredDefinitions() {
		Fixture Test;
		auto Context = Test.Context("Authored", 500, false);
		auto Move = Test.Action(Context, "MoveForward");
		auto AuthoredBinding = Test.Binding(Move, Enums::KeyCode::D);
		auto Jump = Test.Action(Context, "Jump");
		Jump->SetEnabled(false);
		const auto ContextId = Context->GetObjectId();
		const auto BindingId = AuthoredBinding->GetObjectId();
		const auto Path = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets" / "runtime" /
						  "DefaultActionMap.luau";
		std::ifstream File(Path, std::ios::binary);
		if (!File) throw std::runtime_error("[Input:DefaultTest] DefaultActionMap resource is missing");
		const std::string DefaultSource{std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
		ScriptEngine Engine(Test.World);
		const auto Source = std::string("local Populate = (function()\n") + DefaultSource + R"(
end)()
local Input = game:GetService("InputService")
local Map = game:GetService("ActionMap")
local Cleanup = Populate(Map, true)
assert(Map:GetBindingCount() == 5)
local SecondCleanup = Populate(Map, true)
assert(Map:GetBindingCount() == 5)
SecondCleanup()
assert(Map:GetBindingCount() == 5)
Cleanup()
assert(Map:GetBindingCount() == 0)
local Context = Input:FindFirstChild("Authored")
assert(Context.Priority == 500 and #Context.MoveForward:GetChildren() == 1)
assert(Context.MoveForward:GetChildren()[1].KeyCode == Enum.KeyCode.D)
assert(Context.Jump.Enabled == false and #Context.Jump:GetChildren() == 0)
)";
		const auto Status = Run(Engine, Source);
		if (Status != LUA_OK) std::cerr << "[Input:DefaultTest] " << lua_tostring(Engine.L, -1) << '\n';
		Check(
			Status == LUA_OK && Context->GetObjectId() == ContextId && AuthoredBinding->GetObjectId() == BindingId,
			"shipped defaults fill only absent actions and cleanup preserves authored bindings, disabled actions and "
			"identities"
		);
		lua_settop(Engine.L, 0);
	}

	void TestEditorHostAuthoring() {
		using Json = nlohmann::ordered_json;
		const auto Root = std::filesystem::temp_directory_path() /
						  ("gargantuan-input-test-" +
						   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() {
				std::error_code Error;
				std::filesystem::remove_all(Root, Error);
			}
		} Lifetime{Root};
		std::filesystem::create_directories(Root / ".gargantuan");
		{
			Fixture FixtureValue;
			(void)FixtureValue.Context();
			std::shared_ptr<Instance> World = FixtureValue.World;
			std::ofstream File(Root / ".gargantuan" / "project.instance.json", std::ios::binary);
			File << InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, World);
			if (!File) throw std::runtime_error("[Input:EditorHostTest] Could not write owned fixture");
		}
		EditorHost Host("input-test");
		std::size_t Request = 0;
		auto Call = [&](std::string Method, Json Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(
				Json{
					{"Version", EditorHostProtocolVersion},
					{"RequestId", std::to_string(++Request)},
					{"SessionToken", "input-test"},
					{"Method", std::move(Method)},
					{"Params", std::move(Parameters)},
				}
					.dump()
			));
		};
		Check(
			Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"].get<bool>(),
			"EditorHost opens a project containing persistent input Instances"
		);
		const auto Schema = Call("GetSchema");
		for (const auto Name : {"InputContext", "InputAction", "InputBinding"}) {
			const auto Identity = SchemaId::FromNativeName("Engine", Name).ToString();
			Check(
				std::ranges::any_of(
					Schema["Result"]["Definitions"],
					[&](const Json &Value) { return Value.value("SchemaId", "") == Identity; }
				),
				"EditorHost publishes input classes through the ordinary immutable schema DTO"
			);
		}
		const auto Snapshot = Call("GetSnapshot");
		const auto &Objects = Snapshot["Result"]["Snapshot"]["Objects"];
		const auto Context = std::ranges::find_if(Objects, [](const Json &Value) {
			return Value.value("ClassName", "") == "InputContext";
		});
		Check(Context != Objects.end(), "Studio Explorer snapshot contains the authored context");
		if (Context == Objects.end()) return;
		auto Created = Call(
			"CreateInstance",
			{
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "InputAction").ToString()},
				{"DefinitionVersion", 1},
				{"Parent", (*Context)["Id"]},
				{"Name", "CloseInventory"},
			}
		);
		Check(Created["Ok"].get<bool>(), "EditorHost creates an InputAction under its canonical context");
		if (!Created["Ok"].get<bool>()) return;
		Check(
			Call(
				"SetProperty",
				{
					{"Object", (*Context)["Id"]},
					{"ClassSchemaId", SchemaId::FromNativeName("Engine", "InputContext").ToString()},
					{"ClassDefinitionVersion", 1},
					{"DeclaringClassSchemaId", SchemaId::FromNativeName("Engine", "InputContext").ToString()},
					{"DeclaringDefinitionVersion", 1},
					{"Property", "Priority"},
					{"Value", {{"Type", "Int"}, {"Value", 200}}},
				}
			)["Ok"]
				.get<bool>(),
			"Studio Properties edits canonical priority with exact schema identity"
		);
		Check(
			Call("Undo")["Ok"].get<bool>() && Call("Redo")["Ok"].get<bool>(),
			"EditorHost Undo/Redo supports persistent input definition changes"
		);
		Check(
			Call("SaveProject")["Ok"].get<bool>() &&
				Call("OpenProject", {{"Root", Root.generic_string()}})["Ok"].get<bool>(),
			"EditorHost saves and reopens canonical input definitions through project authority"
		);
		const auto Reopened = Call("GetSnapshot");
		Check(
			std::ranges::any_of(
				Reopened["Result"]["Snapshot"]["Objects"],
				[](const Json &Value) {
					return Value.value("Name", "") == "CloseInventory" && Value.value("ClassName", "") == "InputAction";
				}
			),
			"reopened Studio snapshot retains an action created through authoring authority"
		);
	}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestPersistentHierarchyAndAuthority();
		TestChordRepeatAndRelease();
		TestPriorityLifecycleAndRebinding();
		TestContextTakeoverWhileHeld();
		TestFourModifiersAndHeldDefinitionChanges();
		TestInspectionValidationAndPublication();
		TestCompatibilityValueShapes();
		TestGraphBoundsAndTransactionalValidation();
		TestLegacyFacadeOrderingAndMixedValues();
		TestReplicatedDefinitionsRemainLocalState();
		TestLegacyNamesFollowInstanceLifecycle();
		TestDefinitionObserversRemainBounded();
		TestLuauSurface();
		TestDefaultBindingsPreserveAuthoredDefinitions();
		TestEditorHostAuthoring();
	} catch (const std::exception &Error) {
		std::cerr << "[Input:FoundationTest] " << Error.what() << '\n';
		return 1;
	}
	return Failures == 0 ? 0 : 1;
}

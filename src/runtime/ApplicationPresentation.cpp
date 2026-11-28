// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/ApplicationPresentation.hpp"

#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/audio/AudioRuntime.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/RunService.hpp"
#include "gargantuan/services/InputService.hpp"
#include <algorithm>
#include <array>

namespace gargantuan {
	std::shared_ptr<DataModel> ApplicationPresentation::CreateGraph() {
		auto Graph = std::make_shared<DataModel>();
		Graph->ApplicationGraph = true;
		return Graph;
	}

	bool ApplicationPresentation::SupportsClass(std::string_view Name) {
		constexpr std::array Classes{"Folder", "Script", "ModuleScript", "ScreenGui", "Frame", "TextButton", "TextLabel",
			"TextBox", "ScrollingFrame", "UIListLayout", "ImageLabel", "ImageButton", "ViewportFrame", "Camera", "Part", "Sound"};
		return std::ranges::find(Classes, Name) != Classes.end();
	}

	ApplicationPresentation::ApplicationPresentation(std::shared_ptr<DataModel> Graph,
		const std::shared_ptr<Script> &Entry, std::filesystem::path Font,
		std::function<void(std::string, std::string)> Diagnostic, bool AudioEnabled)
		: Gui(Graph, std::move(Font), GuiRuntimeMode::Application),
		  VM(std::make_shared<ScriptEngine>(Graph, std::move(Diagnostic), RuntimeMode::NetworkClient)), EntryScript(Entry) {
		if (!Graph->ApplicationGraph)
			throw std::invalid_argument("[Lifecycle:Application] Expected a private presentation graph");
		const auto Folder = Entry ? Entry->GetParent().value_or(nullptr) : nullptr;
		if (!Folder || !Folder->IsA("Folder") || Folder->GetParent().value_or(nullptr) != Graph ||
			!Entry->GetEnabled() || Entry->Status != ScriptStatus::Idle || !Entry->HasExecutableCode() ||
			Entry->GetRunContext() != Enums::RunContext::Client)
			throw std::invalid_argument("[Lifecycle:Application] Expected an enabled client entry in the application folder");
		Run = std::dynamic_pointer_cast<RunService>(Graph->GetService("RunService"));
		Input = std::dynamic_pointer_cast<InputService>(Graph->GetService("InputService"));
		VM->Application = this;
		try {
			(void)Entry->GetObjectId();
			VM->RunBootstrapScript(Entry);
			if (Entry->Status != ScriptStatus::Finished)
				throw std::runtime_error("[Lifecycle:Application] Entry failed to complete its startup turn");
			CommitMenu();
			if (HasMainMenu()) {
				auto Assets = std::dynamic_pointer_cast<AssetService>(Graph->GetService("AssetService"));
				if (AudioEnabled) Output = CreateSdlAudioOutput();
				Audio = std::make_unique<AudioRuntime>(Assets, Output ? Output->OpenStream() : nullptr,
					[this](std::string Code, std::string Message) {
						VM->EmitRuntimeDiagnostic("Warning", "[Audio:Application] " + Code + " " + Message);
					});
				UnbindAudio = Graph->BindDescendants([this](const std::shared_ptr<Instance> &Object) {
					if (auto Voice = std::dynamic_pointer_cast<Sound>(Object)) Audio->RegisterSound(Voice);
					if (auto ScriptValue = std::dynamic_pointer_cast<Script>(Object); ScriptValue && ScriptValue != EntryScript)
						MenuScripts[ScriptValue->GetObjectId()] = ScriptValue;
				});
			}
		} catch (...) {
			VM->Application = nullptr;
			VM->ShutdownRuntime();
			throw;
		}
	}

	ApplicationPresentation::~ApplicationPresentation() {
		if (UnbindAudio) UnbindAudio();
		if (Audio) Audio->Shutdown();
		VM->Application = nullptr;
		VM->ShutdownRuntime();
	}

	bool ApplicationPresentation::IsCallerCurrent(const DataModel &Receiver) const {
		const auto &Caller = GetCurrentScriptSecurityContext();
		return VM->DataModel.get() == &Receiver && !VM->IsStopping() && Caller.Domain == ScriptExecutionDomain::Client &&
			IsOwnerCurrent(Caller.IssuingScript, Caller.ExecutionGeneration);
	}

	bool ApplicationPresentation::IsOwnerCurrent(ObjectId Owner, std::uint64_t Generation) const {
		if (Owner == EntryScript->GetObjectId())
			return EntryScript->IsCurrentExecution(Generation) && EntryScript->IsDescendantOf(VM->DataModel);
		const auto Found = Controllers.find(Owner);
		const auto Root = Gui.GetApplicationMenu();
		return Root && Found != Controllers.end() && Found->second->IsCurrentExecution(Generation) &&
			Found->second->IsDescendantOf(Root) && Root->IsDescendantOf(VM->DataModel);
	}

	void ApplicationPresentation::TrackConnection(const std::shared_ptr<SignalConnection> &Connection) {
		const auto &Caller = GetCurrentScriptSecurityContext();
		Connections.push_back({Caller.IssuingScript, Caller.ExecutionGeneration, Connection});
	}

	void ApplicationPresentation::SetGameplayInput(bool Enabled) {
		const auto &Caller = GetCurrentScriptSecurityContext();
		if (Enabled) InputHolds.erase(Caller.IssuingScript);
		else InputHolds[Caller.IssuingScript] = Caller.ExecutionGeneration;
		if (GameplayInputEnabled && !InputHolds.empty()) GameplayInputReleasePending = true;
		GameplayInputEnabled = InputHolds.empty();
	}

	void ApplicationPresentation::ReconcileOwners() {
		const auto Menu = Gui.GetApplicationMenu();
		const bool Replaced = ControllerRoot.lock() != Menu;
		std::erase_if(Controllers, [&](const auto &Item) {
			const auto &Controller = Item.second;
			if (!Replaced && Menu && Controller->GetEnabled() && Controller->Status != ScriptStatus::Idle && Controller->IsDescendantOf(Menu)) return false;
			VM->Threads.RetireOwner(Item.first, Controller->GetExecutionGeneration());
			Controller->RetireExecution();
			Controller->Cleanup();
			if (Controller->GetEnabled() && !Controller->GetDestroyed()) Controller->Status = ScriptStatus::Idle;
			return true;
		});
		std::erase_if(Connections, [&](const auto &Item) {
			auto Connection = Item.Connection.lock();
			if (!Connection || !Connection->Connected) return true;
			if (IsOwnerCurrent(Item.Owner, Item.Generation)) return false;
			Connection->Disconnect();
			return true;
		});
		std::erase_if(InputHolds, [&](const auto &Item) { return !IsOwnerCurrent(Item.first, Item.second); });
		GameplayInputEnabled = InputHolds.empty();
		ControllerRoot = Menu;
		std::erase_if(MenuScripts, [](const auto &Item) { return Item.second.expired(); });
		if (!Menu) return;
		std::vector<std::shared_ptr<Script>> Starts;
		for (const auto &[Id, Weak] : MenuScripts) {
			auto Controller = Weak.lock();
			if (!Controller || Controllers.contains(Id) || Controller->Status != ScriptStatus::Idle ||
				Controller->GetRunContext() != Enums::RunContext::Client || !Controller->IsDescendantOf(Menu)) continue;
			Starts.push_back(Controller);
			if (Starts.size() == 8) break;
		}
		for (const auto &Controller : Starts) {
			if (Controller->Status != ScriptStatus::Idle || !Controller->IsDescendantOf(Menu)) continue;
			Controllers.emplace(Controller->GetObjectId(), Controller);
			VM->RunBootstrapScript(Controller);
		}
	}

	bool ApplicationPresentation::HasMainMenu() const {
		const auto Menu = MenuDirty ? Selection.lock() : Gui.GetApplicationMenu();
		return Menu && !Menu->GetDestroyed() && !Menu->IsDestroying() && Menu->IsDescendantOf(VM->DataModel);
	}

	bool ApplicationPresentation::IsMainMenuVisible() const { return (MenuDirty ? RequestedVisible : CommittedVisible) && HasMainMenu(); }

	bool ApplicationPresentation::SetMainMenu(std::shared_ptr<ScreenGui> Menu) {
		if (!Gui.CanSelectApplicationMenu(Menu)) return false;
		if (Menu && Menu == (MenuDirty ? Selection.lock() : Gui.GetApplicationMenu())) return true;
		RequestedVisible = Menu && (!HasMainMenu() || IsMainMenuVisible());
		Selection = Menu;
		MenuDirty = true;
		MenuOwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		MenuOwner = GetCurrentScriptSecurityContext().IssuingScript;
		return true;
	}

	bool ApplicationPresentation::SetMainMenuVisible(bool Visible) {
		if (!HasMainMenu()) return false;
		if (!MenuDirty) Selection = Gui.GetApplicationMenu();
		RequestedVisible = Visible;
		MenuDirty = true;
		MenuOwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		MenuOwner = GetCurrentScriptSecurityContext().IssuingScript;
		return true;
	}

	void ApplicationPresentation::CommitMenu() {
		if (!MenuDirty) return;
		const auto Menu = Selection.lock();
		MenuDirty = false;
		Selection.reset();
		if (!IsOwnerCurrent(MenuOwner, MenuOwnerGeneration)) return;
		const auto Visible = RequestedVisible;
		if (Gui.SetApplicationMenu(Menu, Visible)) CommittedVisible = Visible;
		else
			VM->EmitRuntimeDiagnostic("Warning", "[Lifecycle:Application] Menu changed before presentation");
	}

	void ApplicationPresentation::Step(float DeltaTime) {
		ReconcileOwners();
		VM->Step();
		VM->StepTweens(DeltaTime);
		Run->PreRender->Fire(DeltaTime);
		if (Audio) Audio->Step(CFrame{});
		Input->EndFrame();
		FlushPresentation();
	}

	void ApplicationPresentation::ReturnToMenu() {
		MenuDirty = false;
		InputHolds.clear();
		GameplayInputEnabled = true;
		if (const auto Menu = Gui.GetApplicationMenu())
			CommittedVisible = Gui.SetApplicationMenu(Menu, true);
	}

	void ApplicationPresentation::FlushPresentation() {
		CommitMenu();
		(void)Gui.Reconcile();
	}

	bool ApplicationPresentation::ProcessEvent(const HostEvent &Event, GuiApplicationFrame Frame) {
		const bool GuiHandled = Gui.ProcessEvent(Event, Frame);
		const bool Handled = Input->ProcessEvent(Event, GuiHandled) || GuiHandled;
		CommitMenu();
		return Handled;
	}
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/ApplicationPresentation.hpp"

#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/RunService.hpp"

namespace gargantuan {
	ApplicationPresentation::ApplicationPresentation(std::shared_ptr<DataModel> Graph,
		const std::shared_ptr<Script> &Entry, std::filesystem::path Font,
		std::function<void(std::string, std::string)> Diagnostic)
		: Gui(Graph, std::move(Font), GuiRuntimeMode::Application),
		  VM(std::make_shared<ScriptEngine>(Graph, std::move(Diagnostic), RuntimeMode::NetworkClient)), EntryScript(Entry) {
		const auto Folder = Entry ? Entry->GetParent().value_or(nullptr) : nullptr;
		if (!Folder || !Folder->IsA("Folder") || Folder->GetParent().value_or(nullptr) != Graph ||
			!Entry->GetEnabled() || Entry->Status != ScriptStatus::Idle || !Entry->HasExecutableCode() ||
			Entry->GetRunContext() != Enums::RunContext::Client)
			throw std::invalid_argument("[Lifecycle:Application] Expected an enabled client entry in the application folder");
		Run = std::dynamic_pointer_cast<RunService>(Graph->GetService("RunService"));
		VM->Application = this;
		try {
			(void)Entry->GetObjectId();
			VM->RunBootstrapScript(Entry);
			if (Entry->Status != ScriptStatus::Finished)
				throw std::runtime_error("[Lifecycle:Application] Entry failed to complete its startup turn");
			CommitMenu();
		} catch (...) {
			VM->Application = nullptr;
			VM->ShutdownRuntime();
			throw;
		}
	}

	ApplicationPresentation::~ApplicationPresentation() {
		VM->Application = nullptr;
		VM->ShutdownRuntime();
	}

	bool ApplicationPresentation::IsCallerCurrent(const DataModel &Receiver) const {
		const auto &Caller = GetCurrentScriptSecurityContext();
		return VM->DataModel.get() == &Receiver && !VM->IsStopping() && Caller.Domain == ScriptExecutionDomain::Client &&
			Caller.IssuingScript == EntryScript->GetObjectId() && EntryScript->IsCurrentExecution(Caller.ExecutionGeneration) &&
			EntryScript->IsDescendantOf(VM->DataModel);
	}

	bool ApplicationPresentation::HasMainMenu() const {
		const auto Menu = MenuDirty ? Selection.lock() : Gui.GetApplicationMenu();
		return Menu && !Menu->GetDestroyed() && !Menu->IsDestroying() && Menu->IsDescendantOf(VM->DataModel);
	}

	bool ApplicationPresentation::IsMainMenuVisible() const { return RequestedVisible && HasMainMenu(); }

	bool ApplicationPresentation::SetMainMenu(std::shared_ptr<ScreenGui> Menu) {
		if (!Gui.CanSelectApplicationMenu(Menu)) return false;
		if (Menu && Menu == (MenuDirty ? Selection.lock() : Gui.GetApplicationMenu())) return true;
		RequestedVisible = Menu && (!HasMainMenu() || RequestedVisible);
		Selection = Menu;
		MenuDirty = true;
		return true;
	}

	bool ApplicationPresentation::SetMainMenuVisible(bool Visible) {
		if (!HasMainMenu()) return false;
		if (!MenuDirty) Selection = Gui.GetApplicationMenu();
		RequestedVisible = Visible;
		MenuDirty = true;
		return true;
	}

	void ApplicationPresentation::CommitMenu() {
		if (!MenuDirty) return;
		const auto Menu = Selection.lock();
		MenuDirty = false;
		Selection.reset();
		if (!Gui.SetApplicationMenu(Menu, RequestedVisible))
			VM->EmitRuntimeDiagnostic("Warning", "[Lifecycle:Application] Menu changed before presentation");
	}

	void ApplicationPresentation::Step(float DeltaTime) {
		VM->Step();
		VM->StepTweens(DeltaTime);
		Run->PreRender->Fire(DeltaTime);
		CommitMenu();
		(void)Gui.Reconcile();
	}
}

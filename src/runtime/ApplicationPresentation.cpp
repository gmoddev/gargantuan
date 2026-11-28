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
	namespace {
		class PresentationGraph final : public DataModel {
			const ServiceDefinitions &GetServiceDefinitions() const override {
				static const ServiceDefinitions Services{
					{"RunService", SchemaId::FromNativeName("Engine", "RunService")},
					{"SessionService", SchemaId::FromNativeName("Engine", "SessionService")},
					{"ApplicationService", SchemaId::FromNativeName("Engine", "ApplicationService")},
					{"TweenService", SchemaId::FromNativeName("Engine", "TweenService")},
					{"AssetService", SchemaId::FromNativeName("Engine", "AssetService")},
					{"InputService", SchemaId::FromNativeName("Engine", "InputService")},
				};
				return Services;
			}
		};
	}

	std::shared_ptr<DataModel> ApplicationPresentation::CreateGraph() { return std::make_shared<PresentationGraph>(); }

	bool ApplicationPresentation::SupportsClass(std::string_view Name) {
		constexpr std::array Classes{"Folder", "Script", "ModuleScript", "ScreenGui", "Frame", "TextButton", "TextLabel",
			"TextBox", "ScrollingFrame", "UIListLayout", "ImageLabel", "ImageButton", "ViewportFrame", "Camera", "Sound"};
		return std::ranges::find(Classes, Name) != Classes.end();
	}

	ApplicationPresentation::ApplicationPresentation(std::shared_ptr<DataModel> Graph,
		const std::shared_ptr<Script> &Entry, std::filesystem::path Font,
		std::function<void(std::string, std::string)> Diagnostic, bool AudioEnabled)
		: Gui(Graph, std::move(Font), GuiRuntimeMode::Application),
		  VM(std::make_shared<ScriptEngine>(Graph, std::move(Diagnostic), RuntimeMode::NetworkClient)), EntryScript(Entry) {
		if (!dynamic_cast<PresentationGraph *>(Graph.get()))
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
				Audio = std::make_unique<AudioRuntime>(Assets, AudioEnabled ? CreateSdlAudioBackend() : nullptr,
					[this](std::string Code, std::string Message) {
						VM->EmitRuntimeDiagnostic("Warning", "[Audio:Application] " + Code + " " + Message);
					});
				UnbindAudio = Graph->BindDescendants([this](const std::shared_ptr<Instance> &Object) {
					if (auto Voice = std::dynamic_pointer_cast<Sound>(Object)) Audio->RegisterSound(Voice);
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
			Caller.IssuingScript == EntryScript->GetObjectId() && IsEntryCurrent(Caller.ExecutionGeneration);
	}

	bool ApplicationPresentation::IsEntryCurrent(std::uint64_t Generation) const {
		return EntryScript->IsCurrentExecution(Generation) && EntryScript->IsDescendantOf(VM->DataModel);
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
		return true;
	}

	bool ApplicationPresentation::SetMainMenuVisible(bool Visible) {
		if (!HasMainMenu()) return false;
		if (!MenuDirty) Selection = Gui.GetApplicationMenu();
		RequestedVisible = Visible;
		MenuDirty = true;
		MenuOwnerGeneration = GetCurrentScriptSecurityContext().ExecutionGeneration;
		return true;
	}

	void ApplicationPresentation::CommitMenu() {
		if (!MenuDirty) return;
		const auto Menu = Selection.lock();
		MenuDirty = false;
		Selection.reset();
		if (!EntryScript->IsCurrentExecution(MenuOwnerGeneration) || !EntryScript->IsDescendantOf(VM->DataModel)) return;
		const auto Visible = RequestedVisible;
		if (Gui.SetApplicationMenu(Menu, Visible)) CommittedVisible = Visible;
		else
			VM->EmitRuntimeDiagnostic("Warning", "[Lifecycle:Application] Menu changed before presentation");
	}

	void ApplicationPresentation::Step(float DeltaTime) {
		VM->Step();
		VM->StepTweens(DeltaTime);
		Run->PreRender->Fire(DeltaTime);
		if (Audio) Audio->Step(CFrame{});
		Input->EndFrame();
		FlushPresentation();
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

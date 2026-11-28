// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/gui/GuiRuntime.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace gargantuan {
	class Script;
	class ScriptEngine;
	class RunService;
	class InputService;
	class AudioRuntime;

	// The host supplies a private, already admitted application graph. No
	// session, LocalPlayer, renderer or persistence provider is created here.
	class ApplicationPresentation final {
	  public:
		[[nodiscard]] static std::shared_ptr<DataModel> CreateGraph();
		[[nodiscard]] static bool SupportsClass(std::string_view Name);
		ApplicationPresentation(std::shared_ptr<DataModel> Graph, const std::shared_ptr<Script> &Entry,
			std::filesystem::path Font, std::function<void(std::string, std::string)> Diagnostic = {}, bool AudioEnabled = false);
		~ApplicationPresentation();
		ApplicationPresentation(const ApplicationPresentation &) = delete;
		ApplicationPresentation &operator=(const ApplicationPresentation &) = delete;

		[[nodiscard]] bool IsCallerCurrent(const DataModel &Receiver) const;
		[[nodiscard]] bool IsEntryCurrent(std::uint64_t Generation) const;
		ApplicationSession Sessions;
		bool QuitRequested = false;
		bool GameplayInputEnabled = true;
		[[nodiscard]] bool SetMainMenu(std::shared_ptr<ScreenGui> Menu);
		[[nodiscard]] bool SetMainMenuVisible(bool Visible);
		[[nodiscard]] bool HasMainMenu() const;
		[[nodiscard]] bool IsMainMenuVisible() const;
		void Step(float DeltaTime);
		void FlushPresentation();
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event, GuiApplicationFrame Frame);
		GuiRuntime Gui;

	  private:
		std::shared_ptr<ScriptEngine> VM;
		std::shared_ptr<Script> EntryScript;
		std::shared_ptr<RunService> Run;
		std::shared_ptr<InputService> Input;
		std::unique_ptr<AudioRuntime> Audio;
		std::function<void()> UnbindAudio;
		std::weak_ptr<ScreenGui> Selection;
		bool RequestedVisible = false;
		bool CommittedVisible = false;
		bool MenuDirty = false;
		std::uint64_t MenuOwnerGeneration = 0;
		void CommitMenu();
	};
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/gui/GuiRuntime.hpp"

#include <functional>
#include <string>

namespace gargantuan {
	class Script;
	class ScriptEngine;
	class RunService;

	// The host supplies a private, already admitted application graph. No
	// session, LocalPlayer, renderer or persistence provider is created here.
	class ApplicationPresentation final {
	  public:
		ApplicationPresentation(std::shared_ptr<DataModel> Graph, const std::shared_ptr<Script> &Entry,
			std::filesystem::path Font, std::function<void(std::string, std::string)> Diagnostic = {});
		~ApplicationPresentation();
		ApplicationPresentation(const ApplicationPresentation &) = delete;
		ApplicationPresentation &operator=(const ApplicationPresentation &) = delete;

		[[nodiscard]] bool IsCallerCurrent(const DataModel &Receiver) const;
		[[nodiscard]] bool SetMainMenu(std::shared_ptr<ScreenGui> Menu);
		[[nodiscard]] bool SetMainMenuVisible(bool Visible);
		[[nodiscard]] bool HasMainMenu() const;
		[[nodiscard]] bool IsMainMenuVisible() const;
		void Step(float DeltaTime);
		GuiRuntime Gui;

	  private:
		std::shared_ptr<ScriptEngine> VM;
		std::shared_ptr<Script> EntryScript;
		std::shared_ptr<RunService> Run;
		std::weak_ptr<ScreenGui> Selection;
		bool RequestedVisible = false;
		bool MenuDirty = false;
		void CommitMenu();
	};
}

// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/gui/GuiRuntime.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace gargantuan {
	class Script;
	class ScriptEngine;
	class RunService;
	class InputService;
	class AudioRuntime;
	class IAudioOutput;
	class SignalConnection;

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
		[[nodiscard]] bool IsOwnerCurrent(ObjectId Owner, std::uint64_t Generation) const;
		void TrackConnection(const std::shared_ptr<SignalConnection> &Connection);
		void SetGameplayInput(bool Enabled);
		void NotifyPause(bool Held);
		ApplicationSession Sessions;
		[[nodiscard]] std::shared_ptr<IAudioOutput> GetAudioOutput() const { return Output; }
		bool QuitRequested = false;
		bool GameplayInputEnabled = true;
		bool GameplayInputReleasePending = false;
		[[nodiscard]] bool SetMainMenu(std::shared_ptr<ScreenGui> Menu);
		[[nodiscard]] bool SetMainMenuVisible(bool Visible);
		[[nodiscard]] bool HasMainMenu() const;
		[[nodiscard]] bool IsMainMenuVisible() const;
		void Step(float DeltaTime);
		void FlushPresentation();
		void ReturnToMenu();
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event, GuiApplicationFrame Frame);
		GuiRuntime Gui;

	  private:
		std::shared_ptr<ScriptEngine> VM;
		std::shared_ptr<Script> EntryScript;
		std::shared_ptr<RunService> Run;
		std::shared_ptr<InputService> Input;
		std::unique_ptr<AudioRuntime> Audio;
		std::shared_ptr<IAudioOutput> Output;
		std::function<void()> UnbindAudio;
		std::weak_ptr<ScreenGui> Selection;
		bool RequestedVisible = false;
		bool CommittedVisible = false;
		bool MenuDirty = false;
		std::uint64_t MenuOwnerGeneration = 0;
		ObjectId MenuOwner;
		std::map<ObjectId, std::shared_ptr<Script>> Controllers;
		std::map<ObjectId, std::weak_ptr<Script>> MenuScripts;
		std::weak_ptr<ScreenGui> ControllerRoot;
		struct OwnedConnection {
			ObjectId Owner;
			std::uint64_t Generation;
			std::weak_ptr<SignalConnection> Connection;
		};
		std::vector<OwnedConnection> Connections;
		std::map<ObjectId, std::uint64_t> InputHolds;
		void ReconcileOwners();
		void CommitMenu();
	};
}

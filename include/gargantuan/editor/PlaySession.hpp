// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#pragma once

#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/platform/HostEvent.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	struct PlaySessionId { std::uint64_t Value = 0; };
	enum class PlaySessionState { Stopped, Starting, Running, Stopping, Failed };

	struct PlayDiagnostic {
		std::uint64_t Sequence = 0;
		std::uint64_t TimestampMilliseconds = 0;
		std::string Severity;
		std::string Category;
		std::string Message;
	};

	class PlaySession final {
	  public:
		static constexpr std::size_t MaximumDiagnostics = 256;
		static constexpr std::size_t MaximumDiagnosticBytes = 2048;

		PlaySession(
			PlaySessionId Id,
			std::string LaunchContents,
			InstanceSerialization::InstanceFormat Format,
			std::filesystem::path ProjectRoot,
			std::uint32_t Width,
			std::uint32_t Height,
			std::uint64_t AuthoringRevision,
			AssetProjectSnapshot Assets = {}
		);
		~PlaySession();

		PlaySession(const PlaySession &) = delete;
		PlaySession &operator=(const PlaySession &) = delete;

		void Step();
		void Resize(std::uint32_t Width, std::uint32_t Height);
		[[nodiscard]] HostEventResult ProcessEvent(const HostEvent &Event);
		void Stop();
		[[nodiscard]] std::vector<PlayDiagnostic> DrainDiagnostics();
		[[nodiscard]] std::shared_ptr<DataModel> GetWorld() const { return RuntimeWorld; }
		[[nodiscard]] PlaySessionId GetId() const { return Id; }
		[[nodiscard]] PlaySessionState GetState() const { return State; }
		[[nodiscard]] std::uint64_t GetAuthoringRevision() const { return LaunchAuthoringRevision; }

	  private:
		void AddDiagnostic(std::string Severity, std::string Category, std::string Message);

		PlaySessionId Id;
		PlaySessionState State = PlaySessionState::Stopped;
		std::uint64_t LaunchAuthoringRevision = 0;
		std::shared_ptr<DataModel> RuntimeWorld;
		std::unique_ptr<HeadlessRenderer> RuntimeRenderer;
		std::unique_ptr<Engine> RuntimeEngine;
		std::deque<PlayDiagnostic> Diagnostics;
		std::uint64_t NextDiagnosticSequence = 1;
	};

	[[nodiscard]] const char *GetPlaySessionStateName(PlaySessionState State);
}

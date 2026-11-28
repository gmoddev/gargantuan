// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#pragma once

#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/platform/HostEvent.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	class IPersistenceProvider;
	class SimulationTimeline;
	struct PlaySessionId { std::uint64_t Value = 0; };
	enum class PlaySessionState { Stopped, Starting, Running, Stopping, Failed };

	struct PlayScriptBinding {
		SchemaId ClassSchemaId;
		std::uint32_t ClassDefinitionVersion;
		int SourceVersion;
		ObjectId AuthoringObject;
	};

	struct PlayScriptSources {
		static constexpr std::size_t MaximumBindings = 4096;
		ProjectId Project;
		ObjectId Scope;
		std::unordered_map<std::string, PlayScriptBinding> Bindings;
		std::optional<std::string> DebugSourceId;
	};

	struct PlayScriptFailureLocation {
		std::string SourceId;
		PlayScriptBinding Binding;
		ProjectId Project;
		ObjectId Scope;
		std::uint64_t LaunchRevision;
		int Line;
	};

	struct PlayDiagnostic {
		std::uint64_t Sequence = 0;
		std::uint64_t TimestampMilliseconds = 0;
		std::string Severity;
		std::string Category;
		std::string Message;
		std::optional<PlayScriptFailureLocation> Source;
	};

	struct PlayRootDebugPause {
		std::uint32_t Serial;
		PlayScriptFailureLocation Source;
		bool CanResume;
	};

	struct PlayExecutionDiagnostics {
		struct Phase {
			std::string_view Name;
			std::uint64_t TotalNanoseconds = 0;
			std::uint64_t MaximumNanoseconds = 0;
		};
		static constexpr std::size_t MaximumSamples = 60;
		std::size_t Samples = 0;
		std::array<Phase, 5> Phases{};
	};

	struct PlayStreamingDiagnostics {
		StreamingDiagnostics Streaming;
		std::optional<PlayExecutionDiagnostics> Execution;
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
			AssetProjectSnapshot Assets = {},
			bool AudioEnabled = false,
			std::shared_ptr<IPersistenceProvider> Persistence = {},
			const InstanceSerialization::TerrainSceneAdmission &AdmitTerrain = {},
			PlayScriptSources Sources = {},
			const InstanceSerialization::DomainTerrainAdmission &AdmitDomains = {},
			const blockgrid::SegmentMap* GridSegments=nullptr
		);
		~PlaySession();

		PlaySession(const PlaySession &) = delete;
		PlaySession &operator=(const PlaySession &) = delete;

		void Step();
		void Resize(std::uint32_t Width, std::uint32_t Height);
		[[nodiscard]] HostEventResult ProcessEvent(const HostEvent &Event);
		void Stop();
		[[nodiscard]] std::vector<PlayDiagnostic> DrainDiagnostics();
		[[nodiscard]] std::vector<RenderPublicationPtr> TakeRenderPublications();
		void RequestRenderFullResync();
		void SetStreamingDiagnosticsEnabled(bool Enabled);
		[[nodiscard]] PlayStreamingDiagnostics GetStreamingDiagnostics(ResidencyDiagnosticQuery Query = {}) const;
		[[nodiscard]] std::shared_ptr<DataModel> GetWorld() const { return RuntimeWorld; }
		// Native inspection must use the evaluator actually owned by RuntimeEngine.
		// Never resolve a replacement service by class/name in the mutable graph.
		[[nodiscard]] std::shared_ptr<InputService> GetInputForInspection() const;
		[[nodiscard]] PlaySessionId GetId() const { return Id; }
		[[nodiscard]] PlaySessionState GetState() const { return State; }
		[[nodiscard]] std::uint64_t GetAuthoringRevision() const { return LaunchAuthoringRevision; }
		[[nodiscard]] bool IsRootDebugPaused() const;
		[[nodiscard]] std::optional<PlayRootDebugPause> GetRootDebugPause() const;
		bool ResumeRootDebug(std::uint32_t Serial, bool Step);

	  private:
		void AddDiagnostic(std::string Severity, std::string Category, std::string Message,
			std::optional<PlayScriptFailureLocation> Source = std::nullopt);

		PlaySessionId Id;
		PlaySessionState State = PlaySessionState::Stopped;
		std::uint64_t LaunchAuthoringRevision = 0;
		std::shared_ptr<DataModel> RuntimeWorld;
		std::unique_ptr<HeadlessRenderer> RuntimeRenderer;
		std::deque<PlayDiagnostic> Diagnostics;
		std::uint64_t NextDiagnosticSequence = 1;
		std::unique_ptr<Engine> RuntimeEngine;
		struct ExecutionProfile;
		std::unique_ptr<ExecutionProfile> Profile;
		std::shared_ptr<SimulationTimeline> DebugClock;
		std::optional<PlayScriptFailureLocation> DebugSource;
		std::optional<WindowResizeEvent> DebugPendingResize;
		std::uint32_t DebugPauseSerial = 0;
		std::deque<RenderPublicationPtr> PendingRenderPublications;
		bool AwaitingRenderFullResync = false;
		static constexpr std::size_t MaximumPendingRenderPublications = 32;
	};

	[[nodiscard]] const char *GetPlaySessionStateName(PlaySessionState State);
}

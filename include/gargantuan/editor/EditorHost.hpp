// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/editor/LatestFrameMailbox.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/editor/SharedFrameRing.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

#include <cstddef>
#include <chrono>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace gargantuan {
	inline constexpr std::uint32_t EditorHostProtocolVersion = 1;
	inline constexpr std::size_t EditorHostMaximumRequestBytes = 1024 * 1024;
	inline constexpr std::size_t EditorHostMaximumResponseBytes = 8 * 1024 * 1024;
	inline constexpr std::uint32_t EditorHostMaximumViewportWidth = 3840;
	inline constexpr std::uint32_t EditorHostMaximumViewportHeight = 2160;
	inline constexpr std::uint64_t EditorHostMaximumViewportPixels = 3840ull * 2160ull;
	inline constexpr std::string_view EditorHostResponsePrefix = "GARGANTUAN_EDITOR/1 ";

	class EditorHost {
	  public:
		explicit EditorHost(std::string sessionToken);
		EditorHost(std::string sessionToken, bool AudioEnabled);
		EditorHost(std::string sessionToken, ScriptSecurityContext studioSecurity);
		~EditorHost();

		[[nodiscard]] std::string HandleRequest(std::string_view request);
		int Run(std::istream &input, std::ostream &output, std::function<void()> ProcessObserver = {});
		void PumpPlaySessionForTesting();
		void PumpViewportPresentationForTesting();
		void SetPersistenceCheckpointForTesting(std::function<void()> checkpoint) {
			PersistenceCheckpointForTesting = std::move(checkpoint);
		}
		void SetViewportPresentationCheckpointForTesting(std::function<void(std::string_view)> Checkpoint) {
			ViewportPresentationCheckpointForTesting = std::move(Checkpoint);
		}
		void SetViewportCaptureForTesting(
			std::function<EditorViewportCaptureView(RenderPublicationPtr)> Capture
		) {
			ViewportCaptureForTesting = std::move(Capture);
		}

	  private:
		struct PackageJob;
		std::string SessionToken;
		static constexpr std::uint64_t TransactionOwner = 1;
		std::unique_ptr<DiskFilesystem> Filesystem;
		std::optional<Project> CurrentProject;
		std::shared_ptr<DataModel> World;
		std::uint64_t PersistedRevision = 0;
		std::function<void()> PersistenceCheckpointForTesting;
		std::function<void(std::string_view)> ViewportPresentationCheckpointForTesting;
		std::function<EditorViewportCaptureView(RenderPublicationPtr)> ViewportCaptureForTesting;
		std::optional<ChangeCursor> Cursor;
		MutationGateway Mutations;
		ScriptSecurityContext StudioSecurity = ScriptSecurityContext::StudioCoreUi();
		bool AudioEnabled = false;
		std::optional<RenderCameraInput> ViewportCamera;
		RenderPublisher ViewportPublisher;
		RenderPublicationPtr LastViewportPublication;
		RenderProjection ViewportProjection;
		std::unique_ptr<EditorViewportRenderer> ViewportRenderer;
		std::unique_ptr<SharedFrameRing> ViewportFrameRing;
		std::unique_ptr<LatestFrameMailbox> ViewportLatestFrameMailbox;
		std::uint32_t ViewportWidth = 0;
		std::uint32_t ViewportHeight = 0;
		std::uint64_t ViewportFrameNumber = 0;
		std::uint64_t ViewportGeneration = 1;
		std::uint64_t ViewportCameraRevision = 0;
		std::uint64_t ViewportProducedFrames = 0;
		std::uint64_t ViewportPresentationFailures = 0;
		std::uint64_t ViewportResizeCount = 0;
		std::uint64_t ViewportLastRenderSubmissionUs = 0;
		std::uint64_t ViewportLastGpuReadbackUs = 0;
		std::uint64_t ViewportLastCpuExtractionUs = 0;
		std::uint64_t ViewportLastPlaySessionId = 0;
		std::uint64_t ViewportLastCameraRevision = 0;
		std::uint64_t ViewportLastSourceRevision = 0;
		LatestFrameMailboxLayout::Mode ViewportLastMode = LatestFrameMailboxLayout::Mode::Edit;
		std::string ViewportLastFailure;
		std::string ViewportMostRecentFailure;
		bool ViewportPresentationActive = false;
		std::chrono::steady_clock::time_point NextViewportFrame{};
		std::chrono::steady_clock::time_point NextPlayStep{};
		std::chrono::microseconds ViewportFrameInterval{6944};
		std::unique_ptr<PlaySession> ActivePlaySession;
		PlaySessionId LastPlaySessionId;
		PlaySessionState LastPlaySessionState = PlaySessionState::Stopped;
		std::uint64_t NextPlaySessionId = 1;
		std::unique_ptr<PackageJob> ActivePackageJob;
		std::uint64_t NextPackageJobId = 1;

		void AdvanceViewportGeneration();
		void ConsumePlayRenderPublications();
		void PumpPlaySession();
		void PumpViewportPresentation();
	};
}

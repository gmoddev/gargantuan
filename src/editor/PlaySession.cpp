// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#include "gargantuan/editor/PlaySession.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace gargantuan {
	const char *GetPlaySessionStateName(PlaySessionState State) {
		switch (State) {
		case PlaySessionState::Stopped: return "Stopped";
		case PlaySessionState::Starting: return "Starting";
		case PlaySessionState::Running: return "Running";
		case PlaySessionState::Stopping: return "Stopping";
		case PlaySessionState::Failed: return "Failed";
		}
		return "Failed";
	}

	PlaySession::PlaySession(
		PlaySessionId SessionId,
		std::string LaunchContents,
		InstanceSerialization::InstanceFormat Format,
		std::filesystem::path ProjectRoot,
		std::uint32_t Width,
		std::uint32_t Height,
		std::uint64_t AuthoringRevision,
		AssetProjectSnapshot Assets
	) : Id(SessionId), State(PlaySessionState::Starting), LaunchAuthoringRevision(AuthoringRevision) {
		if (Id.Value == 0 || AuthoringRevision == 0) throw std::invalid_argument("Play session identity and revision must be nonzero");
		std::istringstream Input(std::move(LaunchContents));
		auto Deserialized = InstanceSerialization::Deserialize(Format, Input);
		if (!Deserialized.Ok || !Deserialized.Instance || !Deserialized.Instance->IsA("DataModel")) {
			State = PlaySessionState::Failed;
			throw std::runtime_error("Play launch snapshot could not construct a DataModel");
		}
		RuntimeWorld = std::dynamic_pointer_cast<DataModel>(Deserialized.Instance);
		if (!RuntimeWorld) {
			State = PlaySessionState::Failed;
			throw std::runtime_error("Play launch snapshot has inconsistent DataModel metadata");
		}
		RuntimeWorld->MarkPersistenceSubtreeArchivable();
		RuntimeWorld->Root = std::move(ProjectRoot);
		auto RuntimeAssets = std::dynamic_pointer_cast<AssetService>(RuntimeWorld->GetService("AssetService"));
		if (!RuntimeAssets) {
			State = PlaySessionState::Failed;
			throw std::runtime_error("Play launch snapshot has no canonical AssetService");
		}
		RuntimeAssets->LoadProjectAssetSnapshot(Assets);
		RuntimeRenderer = std::make_unique<HeadlessRenderer>(Vector2(Width, Height));
		RuntimeEngine = std::make_unique<Engine>(
			RuntimeWorld,
			RuntimeRenderer.get(),
			[this](std::string Severity, std::string Message) {
				AddDiagnostic(std::move(Severity), "Luau", std::move(Message));
			}
		);
		RuntimeEngine->ProcessService->Alive = true;
		State = PlaySessionState::Running;
		AddDiagnostic("Information", "Runtime", "Play session is running");
		Step();
	}

	PlaySession::~PlaySession() { Stop(); }

	void PlaySession::Step() {
		if (State != PlaySessionState::Running || !RuntimeEngine) return;
		RuntimeEngine->Step();
		if (auto Publication = RuntimeRenderer->TakeLastPublication()) {
			if (AwaitingRenderFullResync && !Publication->FullResync) {
				// The requested full publication has not reached the renderer yet.
			} else if (PendingRenderPublications.size() == MaximumPendingRenderPublications) {
				PendingRenderPublications.clear();
				AwaitingRenderFullResync = true;
				RuntimeEngine->RenderPublishing.RequestFullResync();
			} else {
				if (Publication->FullResync) AwaitingRenderFullResync = false;
				PendingRenderPublications.push_back(std::move(Publication));
			}
		}
		if (!RuntimeEngine->ProcessService->Alive) {
			AddDiagnostic("Information", "Runtime", "Runtime requested exit");
			Stop();
		}
	}

	HostEventResult PlaySession::ProcessEvent(const HostEvent &Event) {
		if (State != PlaySessionState::Running || !RuntimeEngine) return {};
		return RuntimeEngine->ProcessEvent(Event);
	}

	void PlaySession::Resize(std::uint32_t Width, std::uint32_t Height) {
		if (State != PlaySessionState::Running || !RuntimeRenderer) return;
		RuntimeRenderer->Resize(static_cast<int>(Width), static_cast<int>(Height));
		(void)ProcessEvent(WindowResizeEvent{Width, Height});
		PendingRenderPublications.clear();
		AwaitingRenderFullResync = true;
	}

	void PlaySession::Stop() {
		if (State == PlaySessionState::Stopped || State == PlaySessionState::Stopping) return;
		State = PlaySessionState::Stopping;
		if (RuntimeEngine) {
			RuntimeEngine->ProcessService->MarkExit(0);
			RuntimeEngine->Destroy();
			RuntimeEngine.reset();
		}
		RuntimeRenderer.reset();
		RuntimeWorld.reset();
		State = PlaySessionState::Stopped;
		AddDiagnostic("Information", "Runtime", "Play session stopped");
	}

	std::vector<PlayDiagnostic> PlaySession::DrainDiagnostics() {
		std::vector<PlayDiagnostic> Result;
		Result.reserve(Diagnostics.size());
		while (!Diagnostics.empty()) {
			Result.push_back(std::move(Diagnostics.front()));
			Diagnostics.pop_front();
		}
		return Result;
	}

	std::vector<RenderPublicationPtr> PlaySession::TakeRenderPublications() {
		std::vector<RenderPublicationPtr> Result;
		Result.reserve(PendingRenderPublications.size());
		while (!PendingRenderPublications.empty()) {
			Result.push_back(std::move(PendingRenderPublications.front()));
			PendingRenderPublications.pop_front();
		}
		return Result;
	}

	void PlaySession::RequestRenderFullResync() {
		PendingRenderPublications.clear();
		AwaitingRenderFullResync = true;
		if (RuntimeEngine) RuntimeEngine->RenderPublishing.RequestFullResync();
	}

	void PlaySession::AddDiagnostic(std::string Severity, std::string Category, std::string Message) {
		if (Message.size() > MaximumDiagnosticBytes) Message.resize(MaximumDiagnosticBytes);
		if (Diagnostics.size() == MaximumDiagnostics) Diagnostics.pop_front();
		const auto Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()
		).count();
		Diagnostics.push_back({
			NextDiagnosticSequence++, static_cast<std::uint64_t>(Timestamp),
			std::move(Severity), std::move(Category), std::move(Message)
		});
	}
}

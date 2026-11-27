// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#include "gargantuan/editor/PlaySession.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/InputService.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace gargantuan {
	struct PlaySession::ExecutionProfile {
		using WorkPhase = runtime_detail::WorkPhase;
		static constexpr std::array Phases{WorkPhase::EngineStep, WorkPhase::PreSimulation,
			WorkPhase::Physics, WorkPhase::Animation, WorkPhase::RenderPublication};
		std::array<std::array<std::uint64_t, Phases.size()>, PlayExecutionDiagnostics::MaximumSamples> Recent{};
		std::size_t Cursor = 0;
		std::size_t Count = 0;

		void Step(Engine &Runtime) {
			runtime_detail::WorkSample Sample{};
			{
				const runtime_detail::WorkCapture Capture(&Sample);
				Runtime.Step();
			}
			for (std::size_t Index = 0; Index < Phases.size(); ++Index)
				Recent[Cursor][Index] = Sample[static_cast<std::size_t>(Phases[Index])].Nanoseconds;
			Cursor = (Cursor + 1) % Recent.size();
			Count = std::min(Count + 1, Recent.size());
		}

		[[nodiscard]] PlayExecutionDiagnostics Snapshot() const {
			PlayExecutionDiagnostics Result{.Samples = Count};
			static_assert(Phases.size() == std::tuple_size_v<decltype(Result.Phases)>);
			for (std::size_t Index = 0; Index < Phases.size(); ++Index) {
				auto &Phase = Result.Phases[Index];
				Phase.Name = runtime_detail::WorkPhaseNames[static_cast<std::size_t>(Phases[Index])];
				for (std::size_t Sample = 0; Sample < Count; ++Sample) {
					const auto Duration = Recent[Sample][Index];
					runtime_detail::AddWorkCounter(Phase.TotalNanoseconds, Duration);
					Phase.MaximumNanoseconds = std::max(Phase.MaximumNanoseconds, Duration);
				}
			}
			return Result;
		}
	};

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
		AssetProjectSnapshot Assets,
		bool AudioEnabled,
		std::shared_ptr<IPersistenceProvider> Persistence,
		const InstanceSerialization::TerrainSceneAdmission &AdmitTerrain
	) : Id(SessionId), State(PlaySessionState::Starting), LaunchAuthoringRevision(AuthoringRevision) {
		if (Id.Value == 0 || AuthoringRevision == 0) throw std::invalid_argument("Play session identity and revision must be nonzero");
		std::istringstream Input(std::move(LaunchContents));
		auto Deserialized = InstanceSerialization::Deserialize(Format, Input, AdmitTerrain);
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
		// EditorHost owns the SDL device. This CPU-only renderer still produces
		// the complete visual publications its offscreen consumer needs.
		RuntimeRenderer = std::make_unique<HeadlessRenderer>(Vector2(Width, Height),
			HeadlessRenderer::PublicationMode::DeferredGpuPresentation);
		EngineProviderConfiguration Providers{.AudioEnabled = AudioEnabled};
		Providers.Persistence = std::move(Persistence);
		RuntimeEngine = std::make_unique<Engine>(
			RuntimeWorld,
			RuntimeRenderer.get(),
			[this](std::string Severity, std::string Message) {
				AddDiagnostic(std::move(Severity), "Luau", std::move(Message));
			},
			Providers
		);
		RuntimeEngine->ProcessService->Alive = true;
		State = PlaySessionState::Running;
		AddDiagnostic("Information", "Runtime", "Play session is running");
		Step();
	}

	PlaySession::~PlaySession() { Stop(); }

	void PlaySession::Step() {
		if (State != PlaySessionState::Running || !RuntimeEngine) return;
		if (Profile) Profile->Step(*RuntimeEngine);
		else RuntimeEngine->Step();
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

	void PlaySession::SetStreamingDiagnosticsEnabled(bool Enabled) {
		if (!RuntimeEngine || State != PlaySessionState::Running) return;
		auto NewProfile = Enabled && !Profile ? std::make_unique<ExecutionProfile>() : nullptr;
		RuntimeEngine->SetStreamingDiagnosticsEnabled(Enabled);
		if (!Enabled) Profile.reset();
		else if (NewProfile) Profile = std::move(NewProfile);
	}

	PlayStreamingDiagnostics PlaySession::GetStreamingDiagnostics(ResidencyDiagnosticQuery Query) const {
		PlayStreamingDiagnostics Result;
		if (RuntimeEngine) {
			Result.Streaming = RuntimeEngine->GetStreamingDiagnostics(Query);
			if (Result.Streaming.Enabled && Profile) Result.Execution = Profile->Snapshot();
		} else Result.Streaming.Status = StreamingDiagnosticStatus::Stopped;
		return Result;
	}

	std::shared_ptr<InputService> PlaySession::GetInputForInspection() const {
		if (State != PlaySessionState::Running || !RuntimeEngine || !RuntimeWorld) return {};
		const auto &Input = RuntimeEngine->Input;
		if (!Input || Input->GetDestroyed() || Input->GetReplicationScopeId() != RuntimeWorld->GetObjectId()) return {};
		return Input;
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
		Profile.reset();
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

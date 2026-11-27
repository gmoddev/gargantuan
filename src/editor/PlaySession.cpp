// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#include "gargantuan/editor/PlaySession.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/InputService.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace gargantuan {
	namespace {
		void PublishPausedGui(Engine &Runtime, std::uint32_t Width, std::uint32_t Height) {
			if (!Runtime.Gui) return;
			const auto Presentation = Runtime.Gui->GetCommittedPresentation();
			const auto &Frame = Presentation->Frame;
			const auto Source = Runtime.DataModel->GetObjectId();
			auto &Publisher = Runtime.RenderPublishing;
			if (Frame.ViewportWidth == 0 || Frame.ViewportHeight == 0 ||
				(Frame.ViewportWidth == Width && Frame.ViewportHeight == Height)) {
				if (!Publisher.HasUiFrame(Source, Presentation->Generation))
					Publisher.SetUiFrame(std::shared_ptr<const RenderUiFrame>(Presentation, &Frame),
						Source, Presentation->Generation);
				return;
			}
			// Resize the committed display list without changing the runtime GUI
			// layout, hit testing, or script-observable viewport during a pause.
			const glm::vec2 Scale{static_cast<float>(Width) / Frame.ViewportWidth,
				static_cast<float>(Height) / Frame.ViewportHeight};
			auto Fitted = Frame;
			Fitted.ViewportWidth = Width;
			Fitted.ViewportHeight = Height;
			for (auto &Batch : Fitted.Batches) {
				for (auto &Vertex : Batch.Vertices) Vertex.Position *= Scale;
				if (auto &Clip = Batch.Clip) {
					Clip->X *= Scale.x;
					Clip->Y *= Scale.y;
					Clip->Width *= Scale.x;
					Clip->Height *= Scale.y;
				}
			}
			// Do not identify this derived frame as the runtime GUI's generation.
			// The first live GUI publication reinstates ownership after Continue.
			Publisher.SetUiFrame(std::move(Fitted));
		}
	}

	struct PlaySession::DevelopmentTimeline {
		using Clock = std::chrono::steady_clock;
		std::optional<Clock::time_point> PausedAt;
		Clock::duration PausedTime{};

		[[nodiscard]] Clock::time_point Now() const {
			return (PausedAt ? *PausedAt : Clock::now()) - PausedTime;
		}

		void Resume() {
			PausedTime += Clock::now() - *PausedAt;
			PausedAt.reset();
		}
	};

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
		const InstanceSerialization::TerrainSceneAdmission &AdmitTerrain,
		PlayScriptSources Sources,
		const InstanceSerialization::DomainTerrainAdmission &AdmitDomains,const blockgrid::SegmentMap* GridSegments
	) : Id(SessionId), State(PlaySessionState::Starting), LaunchAuthoringRevision(AuthoringRevision) {
		if (Id.Value == 0 || AuthoringRevision == 0) throw std::invalid_argument("Play session identity and revision must be nonzero");
		std::istringstream Input(std::move(LaunchContents));
		auto Deserialized = InstanceSerialization::Deserialize(Format, Input, AdmitTerrain, AdmitDomains,GridSegments);
		if (!Deserialized.Ok || !Deserialized.Instance || !Deserialized.Instance->IsA("DataModel")) {
			State = PlaySessionState::Failed;
			throw std::runtime_error("Play launch snapshot could not construct a DataModel");
		}
		RuntimeWorld = std::dynamic_pointer_cast<DataModel>(Deserialized.Instance);
		if (!RuntimeWorld) {
			State = PlaySessionState::Failed;
			throw std::runtime_error("Play launch snapshot has inconsistent DataModel metadata");
		}
		struct CloneOrigin {
			std::string SourceId;
			PlayScriptBinding Binding;
			int RuntimeSourceVersion;
		};
		std::unordered_map<ObjectId, CloneOrigin> Origins;
		std::shared_ptr<Script> DebugTarget;
		if (!Sources.Bindings.empty()) {
			// Record the exact hydrated Script objects before Engine boot or any gameplay.
			for (const auto &Object : RuntimeWorld->GetDescendants()) {
				const auto ScriptValue = std::dynamic_pointer_cast<Script>(Object);
				if (!ScriptValue || !ScriptValue->WasSourceIdLoaded()) continue;
				auto SourceId = ScriptValue->GetSourceId();
				const auto Binding = Sources.Bindings.find(SourceId);
				if (Binding != Sources.Bindings.end()) {
					if (Sources.DebugSourceId && *Sources.DebugSourceId == SourceId) {
						DebugTarget = ScriptValue;
						DebugSource = PlayScriptFailureLocation{SourceId, Binding->second, Sources.Project,
							Sources.Scope, LaunchAuthoringRevision, 0};
					}
					Origins.emplace(ScriptValue->GetObjectId(),
						CloneOrigin{std::move(SourceId), Binding->second, ScriptValue->GetSourceVersion()});
				}
			}
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
		if (Sources.DebugSourceId) {
			if (!DebugTarget || !RuntimeEngine->Script->ArmRootScriptDebug(DebugTarget))
				throw std::invalid_argument("[Editor:PlayDebugger] Script cannot start as a managed local root");
			DebugClock = std::make_shared<DevelopmentTimeline>();
			RuntimeEngine->SetDevelopmentClock([Clock = DebugClock] { return Clock->Now(); });
		}
		if (!Origins.empty()) RuntimeEngine->Script->SetScriptFailureObserver(
			[this, Project = Sources.Project, Scope = Sources.Scope, Origins = std::move(Origins)](
				const Script &Failed, int Line, std::string Message
			) {
				std::optional<PlayScriptFailureLocation> Source;
				const auto Origin = Origins.find(Failed.GetObjectId());
				if (Origin != Origins.end() && Failed.GetSourceVersion() == Origin->second.RuntimeSourceVersion)
					Source = PlayScriptFailureLocation{Origin->second.SourceId, Origin->second.Binding,
						Project, Scope, LaunchAuthoringRevision, Line};
				AddDiagnostic("Error", "Luau", std::move(Message), std::move(Source));
			}
		);
		RuntimeEngine->ProcessService->Alive = true;
		State = PlaySessionState::Running;
		AddDiagnostic("Information", "Runtime", "Play session is running");
		Step();
		if (DebugClock && !DebugClock->PausedAt)
			AddDiagnostic("Warning", "Runtime", "The selected Script produced no executable root entry pause");
	}

	PlaySession::~PlaySession() { Stop(); }

	void PlaySession::Step() {
		if (State != PlaySessionState::Running || !RuntimeEngine || IsRootDebugPaused()) return;
		if (Profile) Profile->Step(*RuntimeEngine);
		else RuntimeEngine->Step();
		if (DebugClock && RuntimeEngine->Script && RuntimeEngine->Script->GetPausedRootLine()) {
			DebugClock->PausedAt = std::chrono::steady_clock::now();
			++DebugPauseSerial;
		}
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
		if (State != PlaySessionState::Running || !RuntimeEngine || IsRootDebugPaused()) return {};
		return RuntimeEngine->ProcessEvent(Event);
	}

	bool PlaySession::IsRootDebugPaused() const {
		return DebugClock && DebugClock->PausedAt.has_value();
	}

	std::optional<PlayRootDebugPause> PlaySession::GetRootDebugPause() const {
		if (!IsRootDebugPaused() || !DebugSource || !RuntimeEngine || !RuntimeEngine->Script) return std::nullopt;
		auto Source = *DebugSource;
		Source.Line = RuntimeEngine->Script->GetPausedRootLine();
		return PlayRootDebugPause{DebugPauseSerial, std::move(Source), RuntimeEngine->Script->HasRootDebugBudget()};
	}

	bool PlaySession::ResumeRootDebug(std::uint32_t Serial, bool Step) {
		const auto Pause = GetRootDebugPause();
		if (!Pause || Pause->Serial != Serial || !Pause->CanResume || DebugPauseSerial == UINT32_MAX) return false;
		try {
			RuntimeEngine->Script->ResumeRootScriptDebug(Step);
		} catch (...) {
			Stop();
			throw;
		}
		if (RuntimeEngine->Script && RuntimeEngine->Script->GetPausedRootLine()) {
			++DebugPauseSerial;
			return true;
		}
		DebugClock->Resume();
		DebugSource.reset();
		if (const auto Pending = std::exchange(DebugPendingResize, std::nullopt)) Resize(Pending->Width, Pending->Height);
		return true;
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
		if (IsRootDebugPaused()) {
			DebugPendingResize = WindowResizeEvent{Width, Height};
		} else (void)ProcessEvent(WindowResizeEvent{Width, Height});
		RequestRenderFullResync();
	}

	void PlaySession::Stop() {
		if (State == PlaySessionState::Stopped || State == PlaySessionState::Stopping) return;
		State = PlaySessionState::Stopping;
		DebugSource.reset();
		DebugPendingResize.reset();
		Profile.reset();
		if (RuntimeEngine) {
			RuntimeEngine->ProcessService->MarkExit(0);
			RuntimeEngine->Destroy();
			RuntimeEngine.reset();
		}
		DebugClock.reset();
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
		if (AwaitingRenderFullResync && IsRootDebugPaused()) {
			// A resized or replaced presentation sink needs a new full publication.
			// Keep the runtime clock, scripts, and resize event parked while extracting it.
			const auto [Width, Height] = RuntimeRenderer->GetViewportSize();
			const auto Camera = RuntimeEngine->Workspace->GetCurrentCamera();
			auto &Publisher = RuntimeEngine->RenderPublishing;
			PublishPausedGui(*RuntimeEngine, Width, Height);
			Publisher.RequestFullResync();
			auto Publication = Publisher.Publish(*RuntimeEngine->WorldRoot, MakeRenderCameraInput(*Camera), Width, Height);
			RuntimeRenderer->Draw(Publication);
			PendingRenderPublications.push_back(RuntimeRenderer->TakeLastPublication());
			AwaitingRenderFullResync = false;
		}
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

	void PlaySession::AddDiagnostic(std::string Severity, std::string Category, std::string Message,
		std::optional<PlayScriptFailureLocation> Source) {
		if (Message.size() > MaximumDiagnosticBytes) Message.resize(MaximumDiagnosticBytes);
		if (Diagnostics.size() == MaximumDiagnostics) Diagnostics.pop_front();
		const auto Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()
		).count();
		Diagnostics.push_back({
			NextDiagnosticSequence++, static_cast<std::uint64_t>(Timestamp),
			std::move(Severity), std::move(Category), std::move(Message), std::move(Source)
		});
	}
}

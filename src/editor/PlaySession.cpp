// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0.

#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/runtime/SimulationTimeline.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/render/ApplicationDisplay.hpp"
#include "gargantuan/services/WorldSaveService.hpp"
#include "runtime/WorldWriter.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "packaging/ApplicationTemplate.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
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

	struct PlaySession::PlayApplication {
		std::unique_ptr<ApplicationPresentation> Presentation;
		WorldFactory Open;
		WorldStorage Storage;
		std::weak_ptr<DataModel> Graph;
		std::unique_ptr<host::WorldWriter> Writer;
		std::shared_ptr<IPersistenceProvider> Selected;
		std::unique_ptr<PersistenceRuntime> Opening;
		std::shared_ptr<PersistenceHandle> Admission;
		std::unique_ptr<PlaySession> Child;
		RenderPublisher Publisher;
		ApplicationDisplay Display;
		std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now();
		std::uint32_t Width, Height;
		bool Preparing = false, GameplayResizePending = false;
		GuiApplicationFrame Published{}, Displayed{};
		RenderPublicationId LastPublication = InvalidRenderPublicationId;
	};

	const ApplicationPresentation *PlaySession::GetApplication() const {
		return Application ? Application->Presentation.get() : nullptr;
	}

	std::shared_ptr<DataModel> PlaySession::GetWorld() const {
		return Application && Application->Child ? Application->Child->GetWorld() : RuntimeWorld;
	}

	void PlaySession::StepApplication() {
		auto &App = *Application;
		auto *Menu = App.Presentation.get();
		if (Menu) {
			Menu->Gui.SetViewport({App.Width, App.Height, 1.0f, {}});
			const auto Now = std::chrono::steady_clock::now();
			if (!Menu->QuitRequested) Menu->Step(std::chrono::duration<float>(Now - App.Last).count());
			App.Last = Now;
			if (Menu->QuitRequested) {
				const auto Result = PrepareStop();
				if (Result == PersistenceStatus::Ok) Stop();
				else if (Result != PersistenceStatus::Pending) {
					Menu->QuitRequested = false;
					AddDiagnostic("Error", "Application", std::string("World save during Quit: ") + GetPersistenceStatusName(Result));
				}
				return;
			}
			Menu->Sessions.PumpCatalog(*Menu);
			Menu->Sessions.Commit(*Menu, [&](SessionCommand Command) {
				switch (Command) {
				case SessionCommand::Host: throw SessionAdmissionFailure::UnsupportedTopology;
				case SessionCommand::StartLocal: {
					if (Menu->Sessions.StartingPrivateServer()) throw SessionAdmissionFailure::UnsupportedTopology;
					if (!std::exchange(App.Preparing, true)) return false;
					const auto *Slot = Menu->Sessions.GetStartingSlot();
					if (Slot && !App.Selected) {
						if (!App.Storage.Directory.empty()) App.Writer = std::make_unique<host::WorldWriter>(
							App.Storage.Directory, *ProjectId::Parse(Slot->SaveId));
						App.Selected = App.Storage.Open(*Slot, false);
						if (!App.Selected) throw SessionAdmissionFailure::Unavailable;
						App.Opening = std::make_unique<PersistenceRuntime>(App.Selected, App.Graph);
						App.Admission = App.Opening->Submit({.Store = "WorldAdmission", .Key = "Profile"});
					}
					if (App.Admission) {
						App.Opening->Pump();
						const auto Status = App.Admission->Poll().Status;
						if (Status == PersistenceStatus::Pending) return false;
						if (Status != PersistenceStatus::Ok && Status != PersistenceStatus::Missing)
							throw SessionAdmissionFailure::Unavailable;
						App.Admission.reset();
						App.Opening.reset();
					}
					App.Child = App.Open(App.Width, App.Height, Menu->GetAudioOutput(), Slot, App.Selected);
					App.GameplayResizePending = false;
					App.Preparing = false;
					break;
				}
				case SessionCommand::Leave:
					if (auto Storage = App.Child->RuntimeEngine->Script->GetPersistenceRuntime()) {
						const auto Status = Storage->PollWriteDrain(App.Selected != nullptr);
						if (Status == PersistenceStatus::Pending) return false;
						if (Status == PersistenceStatus::UnknownOutcome) throw SessionAdmissionFailure::UnknownOutcome;
						if (Status != PersistenceStatus::Ok) throw SessionAdmissionFailure::SaveFailed;
					}
					App.Child.reset();
					App.Selected.reset();
					App.Writer.reset();
					Menu->ReturnToMenu();
					App.Publisher.RequestFullResync();
					break;
				case SessionCommand::Pause:
				case SessionCommand::Resume: {
					auto &Child = *App.Child;
					const bool Held = Command == SessionCommand::Pause;
					if (Held) (void)Child.ProcessEvent(FocusEvent{false});
					Child.DebugClock->SetHeld(SimulationTimeline::Hold::Gameplay, Held);
					Child.RuntimeEngine->Audio->SetPlaybackHeld(Held);
					break;
				}
				case SessionCommand::Join: throw SessionAdmissionFailure::UnsupportedTopology;
				}
				return true;
			}, [&] {
				auto Status = PersistenceStatus::Ok;
				if (App.Child) {
					if (const auto Storage = App.Child->RuntimeEngine->Script->GetPersistenceRuntime()) {
						Status = Storage->PollWriteDrain(App.Selected != nullptr);
						if (Status == PersistenceStatus::Pending) return Status;
					}
				}
				App.Preparing = false;
				App.Admission.reset();
				App.Opening.reset();
				App.Child.reset();
				App.Selected.reset();
				App.Writer.reset();
				return Status;
			});
			if (Menu->GameplayInputReleasePending) {
				if (App.Child && !Menu->Sessions.IsPaused() && !Menu->Sessions.IsLeaving()) (void)App.Child->ProcessEvent(FocusEvent{false});
				Menu->GameplayInputReleasePending = false;
			}
			if (Menu->QuitRequested) return;
		}
		if (Menu) Menu->FlushPresentation();
		bool PublishedGame = false;
		if (App.Child) {
			auto &Child = *App.Child;
			if (!Menu || (!Menu->Sessions.IsPaused() && !Menu->Sessions.IsLeaving())) {
				if (std::exchange(App.GameplayResizePending, false)) Child.Resize(App.Width, App.Height);
				Child.Step();
			}
			else if (auto Storage = Child.RuntimeEngine->Script->GetPersistenceRuntime()) Storage->Pump();
			for (auto &Diagnostic : Child.DrainDiagnostics())
				AddDiagnostic(std::move(Diagnostic.Severity), std::move(Diagnostic.Category),
					std::move(Diagnostic.Message), std::move(Diagnostic.Source));
			if (Child.GetState() != PlaySessionState::Running) { Stop(); return; }
			for (auto &Frame : Child.TakeRenderPublications()) {
				if (!Menu) QueuePublication(std::move(Frame));
				else {
					if (Frame->FullResync) App.Publisher.RequestFullResync();
					Menu->Gui.Publish(App.Publisher);
					QueuePublication(App.Display.Compose(Frame, *App.Publisher.PublishPresentation(App.Width, App.Height), true));
				}
				PublishedGame = true;
			}
		}
		if (!Menu) return;
		if (!PublishedGame) {
			Menu->Gui.Publish(App.Publisher);
			QueuePublication(App.Display.Compose({}, *App.Publisher.PublishPresentation(App.Width, App.Height), App.Child != nullptr));
		}
		if (!PendingRenderPublications.empty()) {
			App.LastPublication = PendingRenderPublications.back()->Id;
			App.Published = Menu->Gui.GetApplicationFrame();
		}
	}

	void PlaySession::AcknowledgePresentation(RenderPublicationId Publication) {
		if (auto *App = Application.get(); App && App->Presentation && App->LastPublication == Publication &&
			App->Presentation->Gui.AcknowledgeApplicationFrame(App->Published)) App->Displayed = App->Published;
	}

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

	PlaySession::PlaySession(PlaySessionId SessionId, std::uint64_t Revision, std::uint32_t Width, std::uint32_t Height,
		std::string Template, AssetRuntimeSnapshot Assets, bool AudioEnabled, WorldFactory Open, WorldStorage Storage)
		: Id(SessionId), State(PlaySessionState::Starting), LaunchAuthoringRevision(Revision) {
		auto Graph = application_template::Load(Template);
		auto Service = std::dynamic_pointer_cast<AssetService>(Graph.World->GetService("AssetService"));
		Service->LoadRuntimeAssetSnapshot(application_template::SelectAssets(Template, Assets));
		Application = std::make_unique<PlayApplication>();
		auto &App = *Application;
		App.Open = std::move(Open);
		App.Storage = std::move(Storage);
		App.Graph = Graph.World;
		App.Width = Width;
		App.Height = Height;
		App.Presentation = std::make_unique<ApplicationPresentation>(Graph.World, Graph.Entry,
			Paths::GetExecutableDirectory() / "runtime" / "GargantuanSans.ttf",
			[this](std::string Level, std::string Message) { AddDiagnostic(std::move(Level), "Application", std::move(Message)); },
			AudioEnabled, [&App, World = Graph.World](ApplicationSession &Sessions) {
				if (App.Storage.Open) Sessions.ConfigureCatalog(App.Storage.Catalog, World, App.Storage.Baseline, App.Storage.Open);
			});
		if (!App.Presentation->HasMainMenu()) {
			App.Presentation.reset();
			App.Child = App.Open(Width, Height, {}, nullptr, {});
		}
		State = PlaySessionState::Running;
		Step();
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
		const InstanceSerialization::DomainTerrainAdmission &AdmitDomains,const blockgrid::SegmentMap* GridSegments,
		std::shared_ptr<IAudioOutput> SharedAudio, bool ApplicationChild, const WorldSlot *Slot
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
		if (Slot) std::dynamic_pointer_cast<WorldSaveService>(RuntimeWorld->GetService("WorldSaveService"))->BindSlot(*Slot);
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
		Providers.AudioOutput = std::move(SharedAudio);
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
		}
		if (ApplicationChild) {
			if (auto Storage = RuntimeEngine->Script->GetPersistenceRuntime()) Storage->TrackSessionWrites();
		}
		if (ApplicationChild || Sources.DebugSourceId) {
			DebugClock = std::make_shared<SimulationTimeline>();
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
		if (Sources.DebugSourceId && !IsRootDebugPaused())
			AddDiagnostic("Warning", "Runtime", "The selected Script produced no executable root entry pause");
	}

	PlaySession::~PlaySession() { Stop(); }

	void PlaySession::Step() {
		if (Application && State == PlaySessionState::Running) { StepApplication(); return; }
		if (State != PlaySessionState::Running || !RuntimeEngine || IsRootDebugPaused()) return;
		if (Profile) Profile->Step(*RuntimeEngine);
		else RuntimeEngine->Step();
		if (DebugClock && RuntimeEngine->Script && RuntimeEngine->Script->GetPausedRootLine()) {
			DebugClock->SetHeld(SimulationTimeline::Hold::Debugger, true);
			++DebugPauseSerial;
		}
		QueuePublication(RuntimeRenderer->TakeLastPublication());
		if (!RuntimeEngine->ProcessService->Alive) {
			AddDiagnostic("Information", "Runtime", "Runtime requested exit");
			Stop();
		}
	}

	void PlaySession::QueuePublication(RenderPublicationPtr Publication) {
		if (Publication) {
			if (AwaitingRenderFullResync && !Publication->FullResync) {
				// The requested full publication has not reached the renderer yet.
			} else if (PendingRenderPublications.size() == MaximumPendingRenderPublications) {
				RequestRenderFullResync();
			} else {
				if (Publication->FullResync) AwaitingRenderFullResync = false;
				PendingRenderPublications.push_back(std::move(Publication));
			}
		}
	}

	HostEventResult PlaySession::ProcessEvent(const HostEvent &Event) {
		if (Application && State == PlaySessionState::Running) {
			auto &App = *Application;
			if (auto *Menu = App.Presentation.get()) {
				const bool Consumed = Menu->ProcessEvent(Event, App.Displayed);
				auto TextInput = Menu->Gui.SynchronizeTextInput();
				if (Consumed || TextInput || !Menu->GameplayInputEnabled ||
					Menu->Sessions.IsPaused() || Menu->Sessions.IsLeaving())
					return {Consumed || TextInput.has_value(), std::move(TextInput)};
			}
			return App.Child ? App.Child->ProcessEvent(Event) : HostEventResult{};
		}
		if (State != PlaySessionState::Running || !RuntimeEngine || IsRootDebugPaused()) return {};
		return RuntimeEngine->ProcessEvent(Event);
	}

	bool PlaySession::IsRootDebugPaused() const {
		if (Application && Application->Child) return Application->Child->IsRootDebugPaused();
		return DebugClock && DebugClock->IsHeld(SimulationTimeline::Hold::Debugger);
	}

	std::optional<PlayRootDebugPause> PlaySession::GetRootDebugPause() const {
		if (Application && Application->Child) return Application->Child->GetRootDebugPause();
		if (!IsRootDebugPaused() || !DebugSource || !RuntimeEngine || !RuntimeEngine->Script) return std::nullopt;
		auto Source = *DebugSource;
		Source.Line = RuntimeEngine->Script->GetPausedRootLine();
		return PlayRootDebugPause{DebugPauseSerial, std::move(Source), RuntimeEngine->Script->HasRootDebugBudget()};
	}

	bool PlaySession::ResumeRootDebug(std::uint32_t Serial, bool Step) {
		if (Application && Application->Child) return Application->Child->ResumeRootDebug(Serial, Step);
		const auto Pause = GetRootDebugPause();
		if (!Pause || Pause->Serial != Serial || !Pause->CanResume || DebugPauseSerial == UINT32_MAX) return false;
		const auto PreviousView = RuntimeEngine->Workspace->GetCurrentSpatialRegion().value_or(nullptr);
		const auto PreviousActivation = PreviousView ? PreviousView->GetSpatialActivation() : 0;
		try {
			RuntimeEngine->Script->ResumeRootScriptDebug(Step);
		} catch (...) {
			Stop();
			throw;
		}
		if (RuntimeEngine->Script && RuntimeEngine->Script->GetPausedRootLine()) {
			// A stepped line can switch charts while the simulation clock is held.
			const auto View = RuntimeEngine->Workspace->GetCurrentSpatialRegion().value_or(nullptr);
			if (View != PreviousView || (View && View->GetSpatialActivation() != PreviousActivation))
				RequestRenderFullResync();
			++DebugPauseSerial;
			return true;
		}
		DebugClock->SetHeld(SimulationTimeline::Hold::Debugger, false);
		DebugSource.reset();
		if (const auto Pending = std::exchange(DebugPendingResize, std::nullopt)) Resize(Pending->Width, Pending->Height);
		return true;
	}

	void PlaySession::SetStreamingDiagnosticsEnabled(bool Enabled) {
		if (Application && Application->Child) return Application->Child->SetStreamingDiagnosticsEnabled(Enabled);
		if (!RuntimeEngine || State != PlaySessionState::Running) return;
		auto NewProfile = Enabled && !Profile ? std::make_unique<ExecutionProfile>() : nullptr;
		RuntimeEngine->SetStreamingDiagnosticsEnabled(Enabled);
		if (!Enabled) Profile.reset();
		else if (NewProfile) Profile = std::move(NewProfile);
	}

	PlayStreamingDiagnostics PlaySession::GetStreamingDiagnostics(ResidencyDiagnosticQuery Query) const {
		if (Application && Application->Child) return Application->Child->GetStreamingDiagnostics(Query);
		PlayStreamingDiagnostics Result;
		if (RuntimeEngine) {
			Result.Streaming = RuntimeEngine->GetStreamingDiagnostics(Query);
			if (Result.Streaming.Enabled && Profile) Result.Execution = Profile->Snapshot();
		} else Result.Streaming.Status = StreamingDiagnosticStatus::Stopped;
		return Result;
	}

	std::shared_ptr<InputService> PlaySession::GetInputForInspection() const {
		if (Application && Application->Child) return Application->Child->GetInputForInspection();
		if (State != PlaySessionState::Running || !RuntimeEngine || !RuntimeWorld) return {};
		const auto &Input = RuntimeEngine->Input;
		if (!Input || Input->GetDestroyed() || Input->GetReplicationScopeId() != RuntimeWorld->GetObjectId()) return {};
		return Input;
	}

	void PlaySession::Resize(std::uint32_t Width, std::uint32_t Height) {
		if (Application) {
			Application->Width = Width;
			Application->Height = Height;
			if (Application->Child) {
				const auto &Menu = Application->Presentation;
				if (Menu && (Menu->Sessions.IsPaused() || Menu->Sessions.IsLeaving())) {
					Application->GameplayResizePending = true;
					Application->Child->RuntimeRenderer->Resize(static_cast<int>(Width), static_cast<int>(Height));
				}
				else Application->Child->Resize(Width, Height);
			}
			RequestRenderFullResync();
			return;
		}
		if (State != PlaySessionState::Running || !RuntimeRenderer) return;
		RuntimeRenderer->Resize(static_cast<int>(Width), static_cast<int>(Height));
		if (IsRootDebugPaused()) {
			DebugPendingResize = WindowResizeEvent{Width, Height};
		} else (void)ProcessEvent(WindowResizeEvent{Width, Height});
		RequestRenderFullResync();
	}

	PersistenceStatus PlaySession::PrepareStop() {
		if (!Application || !Application->Child || !Application->Child->RuntimeEngine) return PersistenceStatus::Ok;
		const auto Storage = Application->Child->RuntimeEngine->Script->GetPersistenceRuntime();
		return Storage ? Storage->PollWriteDrain(Application->Selected != nullptr) : PersistenceStatus::Ok;
	}

	void PlaySession::Stop() {
		if (State == PlaySessionState::Stopped || State == PlaySessionState::Stopping) return;
		State = PlaySessionState::Stopping;
		if (Application && Application->Presentation) {
			auto &Menu = *Application->Presentation;
			Menu.CommitClose();
			Menu.Sessions.FinishClose(Menu, PrepareStop());
		}
		Application.reset();
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
		if (AwaitingRenderFullResync && RuntimeEngine && DebugClock &&
			(IsRootDebugPaused() || DebugClock->IsHeld(SimulationTimeline::Hold::Gameplay))) {
			// Project the selected chart into a new sink or after a stepped chart change.
			// Keep the runtime clock, scripts, and resize event parked during extraction.
			const auto [Width, Height] = RuntimeRenderer->GetViewportSize();
			const auto Camera = RuntimeEngine->Workspace->GetCurrentCamera();
			const auto Selected = RuntimeEngine->Workspace->GetCurrentSpatialRegion();
			const std::shared_ptr<WorldRoot> View = Selected ? *Selected : RuntimeEngine->WorldRoot;
			auto &Publisher = RuntimeEngine->RenderPublishing;
			PublishPausedGui(*RuntimeEngine, Width, Height);
			Publisher.SetTerrainRenderState(View->GetTerrainRenderState());
			Publisher.RequestFullResync();
			auto Publication = Publisher.Publish(*View, MakeRenderCameraInput(*Camera), Width, Height);
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
		if (Application) {
			Application->Displayed = {};
			Application->Publisher.RequestFullResync();
			if (Application->Child) Application->Child->RequestRenderFullResync();
		}
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

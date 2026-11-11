#include "gargantuan/Engine.hpp"
#include "runtime/PublicationLatencyDiagnostics.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/FileLink.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/InputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <chrono>
#include <filesystem>
#include <glm/glm.hpp>
#include <limits>
#include <lua.h>
#include <memory>
#include <optional>
#include <thread>

namespace gargantuan {
	Engine::Engine(
		std::shared_ptr<gargantuan::DataModel> game,
		BaseRenderer *renderer,
		std::function<void(std::string, std::string)> RuntimeDiagnostic,
		EngineProviderConfiguration ProviderConfiguration
	)
		: DataModel(game), Renderer(renderer),
		  Script(std::make_shared<class ScriptEngine>(game, std::move(RuntimeDiagnostic), ProviderConfiguration.Mode)),
		  Workspace(GetService<gargantuan::Workspace>()),
		  WorldRoot(std::static_pointer_cast<gargantuan::WorldRoot>(Workspace)),
		  RunService(GetService<gargantuan::RunService>()), ProcessService(GetService<gargantuan::ProcessService>()),
		  UserInputService(GetService<gargantuan::UserInputService>()), ActionMap(GetService<gargantuan::ActionMap>()),
		  Input(GetService<gargantuan::InputService>()),
		  Assets(GetService<gargantuan::AssetService>()),
		  CharacterControl(GetService<gargantuan::CharacterControlService>()),
		  Animation(
			  std::make_unique<AnimationRuntime>(
				  Assets,
				  [](std::string Code, std::string Message) {
					  LOG_WARN(App, "[Animation:Runtime] %s: %s", Code.c_str(), Message.c_str());
				  },
				  AnimationRuntimeOptions{
					  .CpuSkinningFallback = Renderer && Renderer->GetCapabilities().Graphical &&
											 !Renderer->GetCapabilities().GpuSkinning,
				  }
			  )
		  ),
		  Spatial(
			  std::make_shared<SemanticSpatialResolver>(
				  Assets,
				  Animation.get(),
				  [](std::string Code, std::string Message) {
					  LOG_WARN(App, "[Spatial:Semantic] %s: %s", Code.c_str(), Message.c_str());
				  }
			  )
		  ),
		  Audio(
			  std::make_unique<AudioRuntime>(
				  Assets,
				  ProviderConfiguration.AudioEnabled ? CreateSdlAudioBackend() : nullptr,
				  [](std::string Code, std::string Message) {
					  LOG_WARN(App, "[Audio:Runtime] %s: %s", Code.c_str(), Message.c_str());
				  },
				  Spatial
			  )
		  ),
		  Entitlements(GetService<gargantuan::EntitlementService>()),
		  Interaction(GetService<gargantuan::InteractionService>()), Lighting(GetService<gargantuan::Lighting>()),
		  Players(GetService<gargantuan::Players>()) {
		// All members exist here, but ~Engine does not run if this body throws.
		// Unwind runtime bindings while their Engine/VM/renderer are still alive,
		// before automatic member destruction can leave callbacks in the world.
		struct ConstructionLifetime final {
			Engine &Runtime;
			bool Complete = false;
			~ConstructionLifetime() { if (!Complete) Runtime.Destroy(); }
		} Construction{*this};
		if (ProviderConfiguration.Entitlements &&
			!Entitlements->ConfigureProvider(std::move(ProviderConfiguration.Entitlements)))
			LOG_WARN(
				App, "[Backend:Entitlements] configured provider did not become ready; offline semantics remain active"
			);
		CharacterControl->ConfigureRuntime(ProviderConfiguration.Mode);
		const auto DefaultGuiFont = Paths::GetExecutableDirectory() / "runtime" / "GargantuanSans.ttf";
		ActionMap->AttachInputService(UserInputService);
		if (ProviderConfiguration.Mode == RuntimeMode::Offline)
			Players->InitializeLocalPlayer();
		else if (ProviderConfiguration.Mode == RuntimeMode::NetworkClient && !Players->GetLocalPlayer())
			throw std::runtime_error(
				"[Player:Session] Network client runtime requires a trusted LocalPlayer association"
			);
		Interaction->AttachRuntime(
			DataModel, Players, ActionMap, Spatial, Renderer && dynamic_cast<HeadlessRenderer *>(Renderer) == nullptr
		);
		Assets->ConfigureBuiltInFont(DefaultGuiFont);
		Gui = std::make_unique<GuiRuntime>(DataModel, DefaultGuiFont);
		if (Renderer) {
			const auto [Width, Height] = Renderer->GetViewportSize();
			Gui->SetViewport({Width, Height, 1.0f, {}});
		}
		if (DataModel->Filesystem) ProjectSources = std::make_unique<SourceMount>(*DataModel->Filesystem);

		UnbindDescendants = DataModel->BindDescendants([this](std::shared_ptr<Instance> inst) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::EngineActivation);
			if (auto script = std::dynamic_pointer_cast<gargantuan::Script>(inst)) {
				this->Script->ScriptQueue.insert(script);
			}
			if (auto SoundValue = std::dynamic_pointer_cast<gargantuan::Sound>(inst)) Audio->RegisterSound(SoundValue);
			if (auto AnimatorValue = std::dynamic_pointer_cast<gargantuan::Animator>(inst))
				Animation->RegisterAnimator(AnimatorValue);
			if (auto AttachmentValue = std::dynamic_pointer_cast<gargantuan::Attachment>(inst))
				Spatial->RegisterAttachment(AttachmentValue);

			if (auto link = std::dynamic_pointer_cast<gargantuan::FileLink>(inst)) {
				if (!ProjectSources) {
					LOG_WARN(
						App,
						"[Project:SourceMount] FileLink '%s' has no project filesystem mount",
						inst->GetFullName().c_str()
					);
					return;
				}
				auto Result = link->Synchronize(*ProjectSources);
				if (!Result)
					LOG_WARN(
						App,
						"[Project:SourceMount] FileLink '%s' failed: %s",
						inst->GetFullName().c_str(),
						Result.error().Format().c_str()
					);
			}
		});

		DescendantRemovedConnection = DataModel->DescendantRemoved->Connect([this](std::shared_ptr<Instance> inst) {
			if (auto script = std::dynamic_pointer_cast<gargantuan::Script>(inst);
				script && Script->ScriptQueue.contains(script)) {
				Script->ScriptQueue.erase(script);
			}
		});
		DataModelDestroyingConnection = DataModel->Destroying->Once([this](std::monostate) { Destroy(); });
		const bool PreloadAllContent = ProviderConfiguration.Content &&
			ProviderConfiguration.Content->Mode == ContentResidencyMode::FullyResident;
		StreamingDiagnosticsEnabled = ProviderConfiguration.EnableStreamingDiagnostics;
		ResidencyPolicyEnabled = ProviderConfiguration.Mode == RuntimeMode::NetworkServer &&
			ProviderConfiguration.EnableResidencyDemandPolicy && ProviderConfiguration.Content &&
			ProviderConfiguration.Content->Mode == ContentResidencyMode::OnDemand;
		const auto ContentStartupDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		if (ProviderConfiguration.Content) {
			Content = std::make_unique<ContentAvailabilityService>(
				DataModel,
				Workspace,
				std::move(*ProviderConfiguration.Content),
				[](std::string Code, std::string Message) {
					LOG_WARN(App, "[Content:Availability] %s: %s", Code.c_str(), Message.c_str());
				}
			);
			if (PreloadAllContent) {
				while (!Content->IsFullyResident()) {
					Content->Step();
					if (std::chrono::steady_clock::now() >= ContentStartupDeadline ||
						Content->GetMetrics().Failures != 0)
						throw std::runtime_error("[Content:Availability] fully resident package bootstrap failed");
					std::this_thread::yield();
				}
			}
		}

		Script->RunBootstrapScript(Players->StartDefaultRuntime(ProviderConfiguration.Mode));
		if (Destroyed) {
			Construction.Complete = true;
			return;
		}
		Interaction->StartDefaultRuntime();

		LOG_INFO(App, "Constructed engine");
		Construction.Complete = true;
	}

	Engine::~Engine() {
		Destroy();
	}

	void Engine::Destroy() {
		if (Destroyed) return;
		Destroyed = true;
		LOG_INFO(App, "Destroying engine");
		if (ResidencyPolicy) ResidencyPolicy->Stop(Content.get());
		ResidencyPolicy.reset();
		// Preparation owns immutable metadata only. Join before Engine teardown;
		// stopped work never publishes even if native code stopped Content first.
		if (ResidencyPolicyPreparation.valid()) {
			ResidencyPolicyPreparation.wait();
			ResidencyPolicyPreparation = {};
		}
		ResidencyPolicyManifest.reset();
		if (Content) Content->Stop();
		if (DataModelDestroyingConnection) {
			DataModelDestroyingConnection->Disconnect();
			DataModelDestroyingConnection.reset();
		}
		if (Interaction && !Interaction->GetDestroyed()) Interaction->ShutdownRuntime();
		if (Players && !Players->GetDestroyed()) Players->ShutdownRuntime();
		if (CharacterControl && !CharacterControl->GetDestroyed()) CharacterControl->DetachRuntime();
		if (ActionMap && !ActionMap->GetDestroyed()) ActionMap->Reset();
		if (Audio) Audio->Shutdown();
		if (Spatial) Spatial->Shutdown();
		if (Animation) Animation->Shutdown();
		if (Gui) Gui->ClearTransientState();
		Gui.reset();
		if (Renderer) Renderer->Destroy();
		if (WorldRoot && !WorldRoot->GetDestroyed()) WorldRoot->Destroy();
		if (UnbindDescendants) {
			UnbindDescendants();
			UnbindDescendants = {};
		}
		if (DescendantRemovedConnection) {
			DescendantRemovedConnection->Disconnect();
			DescendantRemovedConnection.reset();
		}
		if (Entitlements && !Entitlements->GetDestroyed()) {
			Entitlements->DetachAsyncRuntime();
			Entitlements->ShutdownProviderRuntime();
		}
		if (Script) Script->ScriptQueue.clear();
		Script.reset();
	}

	float Engine::GetDeltaTime() {
		return std::chrono::duration<float>(CurrentTick - LastTick).count();
	}

	HostEventResult Engine::ProcessEvent(const HostEvent &Event) {
		HostEventResult Result;
		if (const auto *Resize = std::get_if<WindowResizeEvent>(&Event)) {
			Renderer->Resize(static_cast<int>(Resize->Width), static_cast<int>(Resize->Height));
			RenderPublishing.RequestFullResync();
			if (Gui) Gui->SetViewport({Resize->Width, Resize->Height, 1.0f, {}});
			Workspace->GetCurrentCamera()->SetViewportSize(Vector2(Resize->Width, Resize->Height));
			return Result;
		}
		if (std::holds_alternative<WindowCloseEvent>(Event)) {
			LOG_INFO(App, "Stopping engine");
			ProcessService->MarkExit(0);
			return Result;
		}
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused && Interaction)
			Interaction->CancelInput();

		Result.Consumed = UserInputService->ProcessEvent(Event);
		const bool GuiConsumed = Gui && Gui->ProcessEvent(Event);
		Result.Consumed = Input->ProcessEvent(Event, GuiConsumed) || GuiConsumed || Result.Consumed;
		if (!Result.Consumed) Result.Command = Workspace->GetCurrentCamera()->ProcessEvent(Event);
		if (auto InputCommand = UserInputService->SynchronizeMouseBehavior()) Result.Command = InputCommand;
		if (Gui)
			if (auto TextInputCommand = Gui->SynchronizeTextInput()) Result.Command = TextInputCommand;
		return Result;
	}

	bool Engine::ReplaceEntitlementProvider(std::shared_ptr<IEntitlementProvider> Provider) {
		return Entitlements && !Entitlements->GetDestroyed() && Entitlements->ConfigureProvider(std::move(Provider));
	}

	void Engine::SetStreamingDiagnosticsEnabled(bool Enabled) {
		if (ResidencyPolicy) ResidencyPolicy->SetDiagnosticsEnabled(Enabled);
		StreamingDiagnosticsEnabled = Enabled;
		if (!Enabled) LastContentStepNanoseconds = 0;
	}

	StreamingDiagnostics Engine::GetStreamingDiagnostics(ResidencyDiagnosticQuery Query) const {
		if (Query.MaximumUnits > MaximumResidencyDiagnosticUnits ||
			Query.MaximumTimingSamples > MaximumResidencyDiagnosticSamples || Query.StartIndex > MaximumPackageContentUnits)
			throw std::invalid_argument("[Content:StreamingDiagnostics] query exceeds native limits");
		StreamingDiagnostics Result;
		Result.Enabled = StreamingDiagnosticsEnabled;
		if (!StreamingDiagnosticsEnabled) return Result;
		Result.SimulationTick = SimulationTick;
		Result.ContentConfigured = Content != nullptr;
		Result.ManifestAvailable = Content && Content->IsManifestAvailable();
		Result.LastContentStepNanoseconds = LastContentStepNanoseconds;
		if (Content) Result.Content = Content->GetDiagnosticCounters();
		if (Destroyed || Result.Content.Stopped) Result.Status = StreamingDiagnosticStatus::Stopped;
		else if (ResidencyPreparationFailed) Result.Status = StreamingDiagnosticStatus::PreparationFailed;
		else if (ResidencyPolicy) Result.Status = StreamingDiagnosticStatus::Active;
		else if (!ResidencyPolicyEnabled) Result.Status = StreamingDiagnosticStatus::NotConfigured;
		else if (ResidencyPolicyPreparation.valid()) Result.Status = StreamingDiagnosticStatus::Preparing;
		else Result.Status = StreamingDiagnosticStatus::AwaitingManifest;
		if (ResidencyPolicy && Result.Status == StreamingDiagnosticStatus::Active) {
			Result.Policy = ResidencyPolicy->GetDiagnostics(Query);
			if (Content) for (auto &Unit : Result.Policy.Units) Unit.AvailabilityState = Content->GetState(Unit.Demand.Key);
		}
		return Result;
	}

	void Engine::Step() {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::EngineStep);
		if (Destroyed || !ProcessService->Alive) return;
		// A server callback can destroy game during any Luau resume in this
		// frame. Keep the VM alive until every active callback has unwound.
		auto ScriptRuntime = Script;
		Mutations.Drain();
		if (Destroyed) return;
		if (Content) {
			if (ResidencyPolicy && !Content->IsManifestAvailable()) {
				ResidencyPolicy->Stop(Content.get());
				ResidencyPolicy.reset();
				ResidencyPolicyEnabled = false;
			}
			if (ResidencyPolicyEnabled && !ResidencyPolicy) {
				try {
					if (!ResidencyPolicyPreparation.valid() && Content->GetManifest()) {
						ResidencyPolicyManifest = Content->GetManifestSnapshot();
						ResidencyPolicyPreparation = std::async(std::launch::async, [Manifest = ResidencyPolicyManifest] {
							ExecutionDomainScope Domain(ExecutionDomain::Worker);
						return std::make_unique<ResidencyDemandPolicy>(std::move(Manifest));
						});
					}
					if (ResidencyPolicyPreparation.valid() &&
						ResidencyPolicyPreparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
						auto PreparedPolicy = ResidencyPolicyPreparation.get();
						const auto CurrentManifest = Content->GetManifestSnapshot();
						if (CurrentManifest && ResidencyPolicyManifest &&
							CurrentManifest.get() == ResidencyPolicyManifest.get() &&
							!CurrentManifest.owner_before(ResidencyPolicyManifest) &&
							!ResidencyPolicyManifest.owner_before(CurrentManifest))
							ResidencyPolicy = std::move(PreparedPolicy);
						else ResidencyPolicyEnabled = false;
						ResidencyPolicyManifest.reset();
						if (ResidencyPolicy && StreamingDiagnosticsEnabled) {
							try { ResidencyPolicy->SetDiagnosticsEnabled(true); }
							catch (const std::exception &) {
								StreamingDiagnosticsEnabled = false;
								LOG_WARN(App, "[Content:StreamingDiagnostics] collection unavailable");
							}
						}
					}
				} catch (const std::exception &Error) {
					ResidencyPolicyEnabled = false;
					ResidencyPreparationFailed = true;
					// get() consumes a failed preparation; a launch failure has no future.
					// Keep any still-running future for Destroy to join off this path.
					ResidencyPolicyManifest.reset();
					LOG_WARN(App, "[Content:ResidencyPolicy] preparation failed: %s", Error.what());
				}
			}
			if (ResidencyPolicy)
				ResidencyPolicy->Step(Content.get(), SimulationTick == std::numeric_limits<std::uint64_t>::max()
					? SimulationTick : SimulationTick + 1);
			if (StreamingDiagnosticsEnabled) {
				const auto ContentStarted = std::chrono::steady_clock::now();
				Content->Step();
				LastContentStepNanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - ContentStarted).count());
			} else Content->Step();
		}

		if (Destroyed) return;
		CurrentTick = std::chrono::steady_clock::now();
		if (LastTick.time_since_epoch().count() == 0) LastTick = CurrentTick;
		float deltaTime = GetDeltaTime();
		if (SimulationTick != std::numeric_limits<std::uint64_t>::max()) ++SimulationTick;
		if (SimulationTick == 0) SimulationTick = 1;
		if (CharacterControl) CharacterControl->BeginSimulationFrame(SimulationTick, deltaTime);

		{
			G_PROFILE("Main Thread");

			{
				G_PROFILE("Simulation");
				RunService->PreSimulation->Fire(deltaTime);
				if (Destroyed) return;
				runtime_detail::MeasureWork(runtime_detail::WorkPhase::Physics,
					[&] { WorldRoot->StepPhysics(deltaTime, std::nullopt); });
				if (Destroyed) return;
				Workspace->GetCurrentCamera()->Step(deltaTime);
				if (Destroyed) return;
				RunService->PostSimulation->Fire(deltaTime);
				if (Destroyed) return;
			}

			{
				G_PROFILE("PreRender");
				RunService->PreRender->Fire(deltaTime);
				if (Destroyed) return;
				if (Spatial) Spatial->PrepareAnimationRequirements();
				if (Animation) {
					AnimationUpdateContext UpdateContext;
					const auto Capabilities = Renderer ? Renderer->GetCapabilities()
													   : RendererCapabilities{false, false};
					UpdateContext.Environment = Capabilities.Graphical ? AnimationRuntimeEnvironment::Graphical
																	   : AnimationRuntimeEnvironment::Headless;
					if (Capabilities.Graphical) {
						const auto CameraValue = Workspace->GetCurrentCamera();
						UpdateContext.ViewOrigin = CameraValue->GetCFrame().Position;
						UpdateContext.HasViewOrigin = true;
						const auto Visibility = Renderer->GetAnimationVisibilityFeedback();
						UpdateContext.VisibilityGeneration = Visibility.Generation;
						UpdateContext.VisibilityPublication = Visibility.Publication;
						UpdateContext.VisibilityComplete = Visibility.Complete;
						UpdateContext.VisibleObjects = Visibility.VisibleObjects;
					}
					if (Spatial) {
						UpdateContext.SemanticRequirementsComplete = Spatial->AreAnimationRequirementsComplete();
						UpdateContext.SemanticRequiredObjects = Spatial->GetAnimationRequiredRigs();
					}
					runtime_detail::MeasureWork(runtime_detail::WorkPhase::Animation,
						[&] { Animation->Step(deltaTime, UpdateContext); });
					if (Destroyed) return;
					for (const auto &Request : Animation->GetRootMotionRequests()) {
						runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RootMotion);
						const auto AdmissionStarted = std::chrono::steady_clock::now();
						runtime_detail::RecordPublicationLatency({.Stage = "RootRequest", .Object = Request.TargetCharacter,
							.Tick = SimulationTick, .Kind = 301});
						++RootMotionMetrics.Requests;
						auto CharacterValue = Request.Target.lock();
						if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying() ||
							CharacterValue->GetObjectId() != Request.TargetCharacter ||
							CharacterValue->GetDataModel() != DataModel) {
							++RootMotionMetrics.Rejected;
							RootMotionMetrics.AdmissionCpuNanoseconds += static_cast<std::uint64_t>(
								std::chrono::duration_cast<std::chrono::nanoseconds>(
									std::chrono::steady_clock::now() - AdmissionStarted
								)
									.count()
							);
							continue;
						}
						const auto Kinematic = std::dynamic_pointer_cast<KinematicCharacter>(CharacterValue);
						const auto RequestedWorld = CharacterValue->GetCFrame().Rotation * Request.Delta.Translation;
						auto Result = CharacterValue->AdmitMotion(
							*WorldRoot,
							{
								.Translation = Request.Delta.Translation,
								.Velocity = Kinematic ? Kinematic->GetVelocity() : glm::vec3(0.0f),
								.YawRadians = Request.Delta.YawRadians,
								.Source = CharacterMotionSource::Animation,
								.LocalSpace = true,
							}
						);
						if (Destroyed) return;
						RootMotionMetrics.PhysicsCpuNanoseconds += Result.PhysicsCpuNanoseconds;
						if (!Result.Succeeded())
							++RootMotionMetrics.Rejected;
						else {
							++RootMotionMetrics.Accepted;
							runtime_detail::RecordPublicationLatency({.Stage = "RootCommit", .Object = Request.TargetCharacter,
								.Tick = SimulationTick, .Kind = 301});
							if (glm::length(Result.AppliedTranslation - RequestedWorld) > 1.0e-4f)
								++RootMotionMetrics.Clipped;
						}
						RootMotionMetrics.AdmissionCpuNanoseconds += static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								std::chrono::steady_clock::now() - AdmissionStarted
							)
								.count()
						);
					}
					if (Destroyed) return;
					RenderPublishing.SetAnimationPoseChanges(Animation->GetPoseUpdates(), Animation->GetPoseRemoves());
					Animation->ClearChanges();
				}
				if (Spatial) Spatial->Step();
				Interaction->Step(CurrentTick);
				if (Destroyed) return;
				if (Gui) {
					auto MeshChanges = Assets->DrainMeshChanges();
					if (!MeshChanges.Creates.empty() || !MeshChanges.Removes.empty())
						RenderPublishing.SetAssetMeshChanges(
							std::move(MeshChanges.Creates), std::move(MeshChanges.Removes)
						);
					(void)Gui->Reconcile();
					Gui->Publish(RenderPublishing);
				}
				if (Destroyed) return;
			}

			Input->EndFrame();
			UserInputService->EndFrame();

			{
				G_PROFILE("Draw");
				const auto [viewportWidth, viewportHeight] = Renderer->GetViewportSize();
				auto camera = Workspace->GetCurrentCamera();
				auto Publication = runtime_detail::MeasureWork(runtime_detail::WorkPhase::RenderPublication,
					[&] { return RenderPublishing.Publish(
						*WorldRoot, MakeRenderCameraInput(*camera), viewportWidth, viewportHeight); });
				try {
					Renderer->Draw(std::move(Publication));
				} catch (...) {
					RenderPublishing.RequestFullResync();
					throw;
				}
				if (Destroyed) return;
			}

			{
				G_PROFILE("Scripts");
				if (Entitlements) (void)Entitlements->PumpAsyncCompletions();
				if (Destroyed) return;
				ScriptRuntime->Step();
				if (Destroyed) return;
				if (Audio) Audio->Step(Workspace->GetCurrentCamera()->GetCFrame());
			}
		}

		G_PROFILE_FRAME();

		LastTick = CurrentTick;
	}
}

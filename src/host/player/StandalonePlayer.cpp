// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "StandalonePlayer.hpp"
#include "gargantuan/render/ApplicationDisplay.hpp"
#include "PackagedClient.hpp"
#include "host/common/PackagedHost.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif
#include "WorldWriter.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/persistence/LocalPersistenceProvider.hpp"
#include "gargantuan/services/WorldSaveService.hpp"
#include "gargantuan/persistence/PersistenceRuntime.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/runtime/SimulationTimeline.hpp"
#include "platform/sdl/SDLHost.hpp"
#include <SDL3/SDL.h>
#include <iostream>

namespace gargantuan::host {

	int RunStandalonePlayer(ApplicationPresentation &App, BaseRenderer &Device,
		const RuntimePackagePayload &Package, const std::filesystem::path &Root, const std::filesystem::path &Profile, int MaximumFrames) {
		SDLHost Platform;
		RenderPublisher AppPublisher;
		ApplicationDisplay Display;
		GuiApplicationFrame Displayed{};
		std::uint64_t DisplayedSince = 0;
		auto *Graphics = dynamic_cast<SDLRenderer *>(&Device);
		std::unique_ptr<HeadlessRenderer> Surface;
		std::shared_ptr<SimulationTimeline> Clock;
		std::unique_ptr<WorldWriter> Writer;
		std::unique_ptr<Engine> Session;
		std::shared_ptr<DataModel> PreparingWorld;
		std::unique_ptr<Engine> PreparingSession;
		std::shared_ptr<DataModel> ClientSources;
		std::unique_ptr<network::GameSession> Connection;
		std::uint64_t HandshakeTick = 0;
		const auto StopConnection = [&] {
			if (Connection) Connection->Stop();
			Connection.reset();
			ClientSources.reset();
			HandshakeTick = 0;
		};
		std::shared_ptr<PersistenceHandle> StorageAdmission;
		std::optional<std::vector<std::shared_ptr<PersistenceHandle>>> ClosingWrites;
		bool UncertainWrite = false;
		const auto DiscardStart = [&] {
			StopConnection();
			PreparingSession.reset();
			StorageAdmission.reset();
			PreparingWorld.reset();
			if (!Session) Writer.reset();
			if (!Session) { Surface.reset(); Clock.reset(); }
		};
		const auto ReleaseGameplayInput = [&] {
			App.GameplayInputReleasePending = false;
			if (!Session) return;
			const auto Released = Session->ProcessEvent(FocusEvent{false});
			if (Released.Command) Platform.Apply(*Released.Command);
			Platform.Apply(SetRelativePointerMode{false});
			if (const auto Command = App.Gui.SynchronizeTextInput(true)) Platform.Apply(*Command);
		};
		auto Last = std::chrono::steady_clock::now();
		std::cout << "[Lifecycle:Player] Application entry registered its title\n";
		for (std::uint64_t Frame = 0; !MaximumFrames || Frame < static_cast<std::uint64_t>(MaximumFrames); ++Frame) {
			HostEvent Event;
			std::uint64_t EventTime = 0;
			while (Platform.PollEvent(Event, &EventTime)) {
				if (std::holds_alternative<WindowCloseEvent>(Event)) return 0;
				if (auto *Size = std::get_if<WindowResizeEvent>(&Event)) {
					if (!Size->Width || !Size->Height) continue;
					Device.Resize(Size->Width, Size->Height);
					if (Session) (void)Session->ProcessEvent(Event);
					if (PreparingSession) (void)PreparingSession->ProcessEvent(Event);
					App.Gui.SetViewport({Size->Width, Size->Height, 1.0f, {}});
					Displayed = {};
				} else {
					const bool Stale = EventTime < DisplayedSince && !std::holds_alternative<FocusEvent>(Event);
					if (Stale) {
						const bool Release = std::visit([](const auto &Value) {
							if constexpr (requires { Value.State; }) return Value.State == ButtonState::Released;
							else if constexpr (requires { Value.Action; }) return Value.Action == TouchPointerAction::Up ||
								Value.Action == TouchPointerAction::Cancel;
							else if constexpr (requires { Value.Axis; }) return Value.Value == 0;
							else return false;
						}, Event);
						if (Release) {
							(void)App.ProcessEvent(Event, {});
							if (Session) {
								// Retire physical state without dispatching a click to a newer GUI.
								if (Session->Gui) Session->Gui->ClearTransientState();
								(void)Session->UserInputService->ProcessEvent(Event);
								(void)Session->Input->ProcessEvent(Event, true);
							}
						} else (void)App.Gui.ProcessEvent(Event, {});
						continue;
					}
					const bool Consumed = App.ProcessEvent(Event, Displayed);
					if (App.GameplayInputReleasePending) ReleaseGameplayInput();
					if (Session && !Consumed && App.GameplayInputEnabled && !App.Sessions.IsPaused() && !App.Sessions.IsLeaving()) {
						const auto Result = Session->ProcessEvent(Event);
						if (Result.Command) Platform.Apply(*Result.Command);
					}
				}
				if (App.QuitRequested) return 0;
			}
			const auto [Width, Height] = Device.GetViewportSize();
			App.Gui.SetViewport({Width, Height, 1.0f, {}});
			const auto Now = std::chrono::steady_clock::now();
			App.Step(std::chrono::duration<float>(Now - Last).count());
			App.Sessions.PumpCatalog(App);
			if (App.GameplayInputReleasePending) ReleaseGameplayInput();
			Last = Now;
			if (App.QuitRequested) return 0;
			App.Sessions.Commit(App, [&](SessionCommand Command) {
				switch (Command) {
				case SessionCommand::Join:
				case SessionCommand::StartLocal: {
					const bool Joining = Command == SessionCommand::Join;
					if (PreparingSession) {
						if (StorageAdmission) {
							PreparingSession->Script->GetPersistenceRuntime()->Pump();
							const auto Status = StorageAdmission->Poll().Status;
							if (Status == PersistenceStatus::Pending) return false;
							if (Status != PersistenceStatus::Ok && Status != PersistenceStatus::Missing)
								throw SessionAdmissionFailure::Unavailable;
							StorageAdmission.reset();
						}
						if (!PreparingSession->AdvanceStartup()) return false;
						if (Connection && !Connection->AttachClientRuntime(*PreparingSession)) throw SessionAdmissionFailure::Disconnected;
						Session = std::move(PreparingSession);
						Session->ProcessService->Alive = true;
						break;
					}
					if (!PreparingWorld) {
						if (Joining) {
#if defined(GARGANTUAN_WITH_GNS)
							if (!Connection) {
								const auto Endpoint = ParseEndpoint(App.Sessions.GetJoinAddress());
								if (!Endpoint) throw SessionAdmissionFailure::InvalidOptions;
								if (Package.Inspection.Configuration != PackageConfiguration::Development ||
									!network::IsLoopbackTransportEndpoint(*Endpoint) || Package.SpatialContent)
									throw SessionAdmissionFailure::UnsupportedTopology;
								ClientSources = PackageBuilder::LoadWorld(Package, Root);
								Connection = std::make_unique<network::GameSession>(std::make_shared<network::GameNetworkingSocketsTransport>(),
									ConfigurePackagedClient(Package, ClientSources, *Endpoint, !Package.TerrainArtifacts.empty()));
								if (!Connection->Start().Succeeded()) throw SessionAdmissionFailure::Disconnected;
							}
							(void)Connection->Poll();
							Connection->Step(++HandshakeTick);
							if (Connection->GetStatus() == network::GameSessionStatus::Failed) throw SessionAdmissionFailure::Disconnected;
							if (HandshakeTick >= network::DefaultGameSessionHandshakeTimeoutTicks) throw SessionAdmissionFailure::TimedOut;
							PreparingWorld = Connection->GetClientDataModel();
							if (!PreparingWorld) return false;
							auto Assets = std::dynamic_pointer_cast<AssetService>(PreparingWorld->GetService("AssetService"));
							Assets->LoadRuntimeAssetSnapshot(Package.Assets);
							(void)PackageBuilder::HydrateClientCode(ClientSources, PreparingWorld);
							ClientSources.reset();
#else
							throw SessionAdmissionFailure::UnsupportedTopology;
#endif
						} else PreparingWorld = PackageBuilder::LoadWorld(Package, Root);
						if (const auto *Slot = App.Sessions.GetStartingSlot()) {
							Writer = std::make_unique<WorldWriter>(Profile,
								*ProjectId::Parse(Slot->SaveId));
							auto Service = std::dynamic_pointer_cast<WorldSaveService>(PreparingWorld->GetService("WorldSaveService"));
							Service->BindSlot(*Slot);
						}
						return false;
					}
					const auto Mode = Device.GetCapabilities().Graphical ? HeadlessRenderer::PublicationMode::DeferredGpuPresentation
						: HeadlessRenderer::PublicationMode::SemanticOnly;
					auto NewSurface = std::make_unique<HeadlessRenderer>(Vector2(Width, Height), Mode);
					auto NewClock = std::make_shared<SimulationTimeline>();
					EngineProviderConfiguration Providers;
					Providers.AudioEnabled = Graphics != nullptr;
					Providers.AudioOutput = App.GetAudioOutput();
					Providers.DeferStartup = true;
					Providers.Mode = Joining ? RuntimeMode::NetworkClient : RuntimeMode::Offline;
					Providers.AdmitNetworkTerrain = Joining && !Package.TerrainArtifacts.empty();
					if (const auto *Slot = App.Sessions.GetStartingSlot()) {
						Providers.Persistence = MakeDurableLocalPersistenceProvider({
							.Path = Profile / (Slot->SaveId + ".sqlite"),
							.ProfileId = Package.Inspection.Identity.ToString() + "/" + Slot->WorldId, .CreateIfMissing = false});
						if (!Providers.Persistence) throw std::runtime_error("[Lifecycle:Storage] World writer unavailable");
					}
					if (!Joining) Providers.Content = PackageBuilder::GetLocalContentConfiguration(Package, Root, ContentResidencyMode::FullyResident);
					auto NewSession = std::make_unique<Engine>(PreparingWorld, NewSurface.get(), nullptr, std::move(Providers));
					if (App.Sessions.GetStartingSlot()) StorageAdmission = NewSession->Script->GetPersistenceRuntime()->Submit({
						.Store = "WorldAdmission", .Key = "Profile"});
					NewSession->SetDevelopmentClock([NewClock] { return NewClock->Now(); });
					Surface = std::move(NewSurface);
					Clock = std::move(NewClock);
					PreparingSession = std::move(NewSession);
					PreparingWorld.reset();
					return false;
				}
				case SessionCommand::Leave: {
					if (UncertainWrite) throw SessionAdmissionFailure::UnknownOutcome;
					if (const auto Storage = Session->Script->GetPersistenceRuntime()) {
						if (!ClosingWrites) ClosingWrites = Storage->GetPendingWrites();
						Storage->Pump();
						bool Failed = false;
						for (const auto &Write : *ClosingWrites) {
							const auto Status = Write->Poll().Status;
							if (Status == PersistenceStatus::Pending) return false;
							UncertainWrite |= Status == PersistenceStatus::UnknownOutcome;
							Failed |= Status != PersistenceStatus::Ok;
						}
						if (UncertainWrite) throw SessionAdmissionFailure::UnknownOutcome;
						if (Failed) throw SessionAdmissionFailure::SaveFailed;
					}
					ReleaseGameplayInput();
					StopConnection();
					Session.reset();
					Writer.reset();
					Surface.reset();
					Clock.reset();
					AppPublisher.RequestFullResync();
					App.ReturnToMenu();
					break;
				}
				case SessionCommand::Pause:
				case SessionCommand::Resume:
					if (Command == SessionCommand::Pause && !App.Sessions.IsPaused())
						ReleaseGameplayInput();
					Clock->SetHeld(SimulationTimeline::Hold::Gameplay, Command == SessionCommand::Pause);
					Session->Audio->SetPlaybackHeld(Command == SessionCommand::Pause);
					break;
				}
				return true;
			}, DiscardStart);
			if (ClosingWrites && Session && Clock) {
				const bool Held = App.Sessions.IsPaused() || App.Sessions.IsLeaving();
				Clock->SetHeld(SimulationTimeline::Hold::Gameplay, Held);
				Session->Audio->SetPlaybackHeld(Held);
			}
			if (!App.Sessions.IsLeaving()) ClosingWrites.reset();
			if (App.GameplayInputReleasePending) ReleaseGameplayInput();
			if (App.QuitRequested) return 0;
			RenderPublicationPtr Gameplay;
			App.FlushPresentation();
			if (Session && !App.Sessions.IsPaused() && !App.Sessions.IsLeaving()) {
				if (Connection) (void)Connection->Poll();
				const auto Closed = [&] {
					return Connection && (Connection->GetStatus() == network::GameSessionStatus::Closed ||
						Connection->GetStatus() == network::GameSessionStatus::Failed);
				};
				if (!Closed()) {
					Session->Step();
					if (Connection) Connection->Step(Session->GetSimulationTick());
				}
				if (Closed()) {
						ReleaseGameplayInput();
						StopConnection();
						Session.reset(); Surface.reset(); Clock.reset();
						App.Sessions.RetireDisconnected();
						App.ReturnToMenu();
						AppPublisher.RequestFullResync();
					continue;
				}
				Gameplay = Surface->TakeLastPublication();
				if (!Session->ProcessService->Alive) return Session->ProcessService->ExitCode;
			} else if (Session) {
				if (Session->Content) Session->Content->Step();
				if (const auto Storage = Session->Script->GetPersistenceRuntime()) Storage->Pump();
			}
			if (Gameplay && Gameplay->FullResync) AppPublisher.RequestFullResync();
			if (const auto Command = App.Gui.SynchronizeTextInput()) Platform.Apply(*Command);
			App.Gui.Publish(AppPublisher);
			const auto AppFrame = AppPublisher.PublishPresentation(Width, Height);
			const auto Submitted = Graphics ? Graphics->GetMetrics().FramesSubmitted : 0;
			Device.Draw(Display.Compose(Gameplay, *AppFrame, Session != nullptr));
			const auto Receipt = App.Gui.GetApplicationFrame();
			if ((!Graphics || Graphics->GetMetrics().FramesSubmitted != Submitted) && App.Gui.AcknowledgeApplicationFrame(Receipt)) {
				if (Receipt != Displayed) DisplayedSince = SDL_GetTicksNS();
				Displayed = Receipt;
			}
			SDL_Delay(16);
		}
		std::cout << "[Lifecycle:Player] Reached requested frame limit\n";
		return 0;
	}
}

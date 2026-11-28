// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "StandalonePlayer.hpp"
#include "gargantuan/render/ApplicationDisplay.hpp"
#include "PackagedClient.hpp"
#include "PrivateServerProcess.hpp"
#include "SessionSmokeProbe.hpp"
#include "host/common/PackagedHost.hpp"
#if defined(GARGANTUAN_WITH_GNS)
#include "SessionDirectory.hpp"
#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#endif
#include "runtime/WorldWriter.hpp"
#include "runtime/RuntimeWorkTrace.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/persistence/LocalPersistenceProvider.hpp"
#include "gargantuan/services/WorldSaveService.hpp"
#include "gargantuan/services/InputService.hpp"
#include "gargantuan/persistence/PersistenceRuntime.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/runtime/SimulationTimeline.hpp"
#include "platform/sdl/SDLHost.hpp"
#include <SDL3/SDL.h>
#include <iostream>
#include <thread>
#include <utility>

namespace gargantuan::host {
	namespace {
		class GuestInvitationReceiver {
		  public:
			explicit GuestInvitationReceiver(const PlayerAdmission &Grant)
				: Channel(*Grant.Refresh), Endpoint(Grant.Endpoint), ServerKey(*Grant.ServerKey), LastProof(Grant.Proof) {}
			bool Poll(ApplicationSession &Sessions, std::optional<PlayerAdmission> &Admission) {
				auto Message = Channel.Poll();
				if (!Message) return false;
				if (Message->empty()) return true;
				const auto Frame = JsonCodec::Parse(*Message, 4096, "native session invitation");
				if (!Frame || !Frame->is_object() || !Frame->contains("Version") || (*Frame)["Version"] != 1 ||
					!Frame->contains("Action") || !Frame->contains("Source") || !(*Frame)["Source"].is_string() ||
					!Frame->contains("SessionID") || !(*Frame)["SessionID"].is_string()) return false;
				if (!Sessions.MatchesInvitation((*Frame)["Source"].get_ref<const std::string &>(),
					(*Frame)["SessionID"].get_ref<const std::string &>())) return true;
				if ((*Frame)["Action"] == "Withdraw") {
					Sessions.ConsumeInvitation();
					Admission.reset();
					return false;
				}
				if ((*Frame)["Action"] != "Refresh" || !Frame->contains("GuestProof") || !(*Frame)["GuestProof"].is_string() ||
					!Frame->contains("ExpiresUnixMillis") || !(*Frame)["ExpiresUnixMillis"].is_number_unsigned()) return false;
				const auto Expiry = (*Frame)["ExpiresUnixMillis"].get<std::uint64_t>();
				const auto Now = static_cast<std::uint64_t>(InvitationTimeMillis());
				if (Expiry <= Now || Expiry > Now + 60'000 || Expiry <= LastExpiry) return true;
				const auto Proof = detail::DecodePlayerJoinProof((*Frame)["GuestProof"].get_ref<const std::string &>());
				if (!Proof) return false;
				if (*Proof == LastProof) return true;
				LastProof = *Proof;
				LastExpiry = Expiry;
				Admission = PlayerAdmission{Endpoint, *Proof, ServerKey};
				Sessions.RefreshInvitation(std::chrono::milliseconds(Expiry - Now));
				return true;
			}
		  private:
			HostInvitationChannel Channel;
			network::TransportEndpoint Endpoint;
			network::TransportServerPublicKey ServerKey;
			detail::PlayerJoinProof LastProof;
			std::uint64_t LastExpiry{};
		};
	}

	int RunStandalonePlayer(ApplicationPresentation &App, BaseRenderer &Device,
		const RuntimePackagePayload &Package, const std::filesystem::path &Root, const std::function<std::filesystem::path()> &Profile, int MaximumFrames, std::optional<PlayerAdmission> Admission, std::optional<PrivateServerPackage> PrivateServer, bool TracePresentation, bool SessionSmoke, std::optional<LauncherDirectory> Directory) {
		const bool PinnedLaunch = Admission.has_value();
#if defined(GARGANTUAN_WITH_GNS)
		std::optional<SessionDirectory> Browser;
		if (Directory) Browser.emplace(*Directory, Package.Inspection);
#endif
		std::optional<GuestInvitationReceiver> GuestInvitations;
		if (Admission && Admission->Refresh && Admission->ServerKey) GuestInvitations.emplace(*Admission);
		const bool Terrain = !Package.TerrainArtifacts.empty() || !Package.WorkspaceTerrainManifest.empty();
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
		std::unique_ptr<PrivateServerProcess> OwnedServer, RetiringServer;
		bool ChangingHold = false, LostControl = false;
		bool AwaitingOwnerReady = false;
		std::optional<WindowResizeEvent> PendingGameplayResize;
		std::optional<PlayerAdmission> PrivateGrant;
		std::shared_future<std::shared_ptr<PrivateServerSnapshot>> PrivatePackageReady;
		PackageCancellationToken PackageCopy;
		const auto StopConnection = [&] {
			if (Connection) Connection->Stop();
			Connection.reset();
			ClientSources.reset();
			HandshakeTick = 0;
		};
		std::shared_ptr<PersistenceHandle> StorageAdmission;
		bool Draining = false;
		auto StartDrainStatus = PersistenceStatus::Ok;
		const auto Drain = [&](Engine *World) {
			const auto Storage = World ? World->Script->GetPersistenceRuntime() : nullptr;
			return Storage ? Storage->PollWriteDrain(Writer != nullptr) : PersistenceStatus::Ok;
		};
		const auto DiscardStart = [&] {
#if defined(GARGANTUAN_WITH_GNS)
			if (Browser) Browser->Cancel();
#endif
			AwaitingOwnerReady = false;
			PrivateGrant.reset();
			StopConnection();
			if (PreparingSession) {
				const auto Status = Drain(PreparingSession.get());
				if (Status == PersistenceStatus::Pending) return Status;
				if (Status != PersistenceStatus::Ok) StartDrainStatus = Status;
			}
			if (OwnedServer) {
				try {
					if (!OwnedServer->Close()) return PersistenceStatus::Pending;
				} catch (SessionAdmissionFailure Failure) {
					if (StartDrainStatus == PersistenceStatus::Ok)
						StartDrainStatus = Failure == SessionAdmissionFailure::SaveFailed ?
							PersistenceStatus::Unavailable : PersistenceStatus::UnknownOutcome;
				}
				OwnedServer.reset();
			}
			PreparingSession.reset();
			StorageAdmission.reset();
			PreparingWorld.reset();
			if (!Session) Writer.reset();
			if (!Session) { Surface.reset(); Clock.reset(); }
			return std::exchange(StartDrainStatus, PersistenceStatus::Ok);
		};
		const auto ReleaseGameplayInput = [&] {
			App.GameplayInputReleasePending = false;
			if (!Session) return;
			if (!ChangingHold && !Draining && !App.Sessions.IsPaused()) {
				const auto Released = Session->ProcessEvent(FocusEvent{false});
				if (Released.Command) Platform.Apply(*Released.Command);
			}
			Platform.Apply(SetRelativePointerMode{false});
			if (const auto Command = App.Gui.SynchronizeTextInput(true)) Platform.Apply(*Command);
		};
		auto Last = std::chrono::steady_clock::now();
		std::cout << "[Lifecycle:Player] Application entry registered its title\n";
		int ExitCode = SessionSmoke ? 8 : 0;
		auto ServerCloseStatus = PersistenceStatus::Ok;
		SessionSmokeProbe SmokeProbe;
#if defined(GARGANTUAN_WORK_TRACE)
		runtime_detail::HostWorkTrace Trace;
		auto PreviousFrame = Last;
#endif
		for (std::uint64_t Frame = 0;; ++Frame) {
			const auto Started = std::chrono::steady_clock::now();
#if defined(GARGANTUAN_WORK_TRACE)
			runtime_detail::HostWorkTrace::Frame TraceFrame(Trace, Started, PreviousFrame, Frame);
			PreviousFrame = Started;
#endif
			if (RetiringServer) {
				try {
					if (RetiringServer->Close()) RetiringServer.reset();
				} catch (SessionAdmissionFailure Failure) {
					ServerCloseStatus = Failure == SessionAdmissionFailure::SaveFailed ?
						PersistenceStatus::Unavailable : PersistenceStatus::UnknownOutcome;
					ExitCode = 8;
					App.Sessions.ReportRetiredWriteStatus(App, ServerCloseStatus);
					std::cerr << "[Lifecycle:Storage] Retired private server write status "
						<< GetPersistenceStatusName(ServerCloseStatus) << '\n';
					RetiringServer.reset();
				}
			}
			if (MaximumFrames && Frame == static_cast<std::uint64_t>(MaximumFrames)) {
				std::cout << "[Lifecycle:Player] Reached requested frame limit\n";
				App.QuitRequested = true;
			}
			if (!App.QuitRequested && GuestInvitations && !GuestInvitations->Poll(App.Sessions, Admission)) {
				App.Sessions.ConsumeInvitation();
				Admission.reset();
				GuestInvitations.reset();
			}
#if defined(GARGANTUAN_WITH_GNS)
			if (!App.QuitRequested && Browser) Browser->Poll(App.Sessions);
#endif
			HostEvent Event;
			std::uint64_t EventTime = 0;
			while (Platform.PollEvent(Event, &EventTime)) {
				if (std::holds_alternative<WindowCloseEvent>(Event)) App.QuitRequested = true;
				if (App.QuitRequested) break;
				if (auto *Size = std::get_if<WindowResizeEvent>(&Event)) {
					if (!Size->Width || !Size->Height) continue;
					Device.Resize(Size->Width, Size->Height);
					if (Session) {
						if (ChangingHold || App.Sessions.IsPaused() || App.Sessions.IsLeaving()) PendingGameplayResize = *Size;
						else (void)Session->ProcessEvent(Event);
					}
					if (PreparingSession) (void)PreparingSession->ProcessEvent(Event);
					App.Gui.SetViewport({Size->Width, Size->Height, 1.0f, {}});
					Displayed = {};
				} else {
					const bool Stale = EventTime < DisplayedSince && !std::holds_alternative<FocusEvent>(Event);
					if (TracePresentation) {
						if (const auto *Button = std::get_if<PointerButtonEvent>(&Event))
							std::clog << "[Lifecycle:PresentationInput] Button=" << static_cast<int>(Button->State) << " Position="
								<< Button->Position.X << "," << Button->Position.Y << " Stale=" << Stale << std::endl;
					}
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
							if (Session && !ChangingHold && !App.Sessions.IsPaused() && !App.Sessions.IsLeaving()) {
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
					if (Session && !Consumed && App.GameplayInputEnabled && !ChangingHold && !App.Sessions.IsPaused() && !App.Sessions.IsLeaving()) {
						const auto Result = Session->ProcessEvent(Event);
						if (Result.Command) Platform.Apply(*Result.Command);
					}
				}
				if (App.QuitRequested) break;
			}
			if (App.QuitRequested) {
				App.CommitClose();
				PackageCopy.Cancel();
				if (PrivatePackageReady.valid() && PrivatePackageReady.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
					SDL_Delay(2); continue;
				}
				if (RetiringServer) { SDL_Delay(2); continue; }
				if (OwnedServer) {
					try {
						const bool Closed = App.Sessions.IsLeaving() ? OwnedServer->Leave() : OwnedServer->Close();
						if (!Closed) { SDL_Delay(2); continue; }
					} catch (SessionAdmissionFailure Failure) {
						ServerCloseStatus = Failure == SessionAdmissionFailure::SaveFailed ? PersistenceStatus::Unavailable : PersistenceStatus::UnknownOutcome;
						ExitCode = 8;
					}
					OwnedServer.reset();
				}
				const auto Polled = Drain(Session ? Session.get() : PreparingSession.get());
				const auto Status = Polled == PersistenceStatus::Pending ? Polled :
					StartDrainStatus == PersistenceStatus::UnknownOutcome ? StartDrainStatus :
					Polled == PersistenceStatus::Ok ? StartDrainStatus : Polled;
				if (Status == PersistenceStatus::Pending) { SDL_Delay(2); continue; }
				App.Sessions.FinishClose(App, Status == PersistenceStatus::Ok ? ServerCloseStatus : Status);
				if (Status != PersistenceStatus::Ok) {
					std::cerr << "[Lifecycle:Storage] Closing with write status " << GetPersistenceStatusName(Status) << '\n';
					ExitCode = 8;
				}
				break;
			}
			const auto [Width, Height] = Device.GetViewportSize();
			App.Gui.SetViewport({Width, Height, 1.0f, {}});
			const auto Now = std::chrono::steady_clock::now();
			App.Step(std::chrono::duration<float>(Now - Last).count());
			App.Sessions.PumpCatalog(App);
			if (App.GameplayInputReleasePending) ReleaseGameplayInput();
			Last = Now;
			if (App.QuitRequested) continue;
			if (OwnedServer && Session && !App.Sessions.IsLeaving() && !OwnedServer->IsClosing()) {
				try { OwnedServer->ServiceGuestInvitations(); }
				catch (SessionAdmissionFailure) { LostControl = true; }
			}
			if (Session && (LostControl || (OwnedServer && OwnedServer->Exited() && !App.Sessions.IsLeaving()) ||
				((!OwnedServer || (!App.Sessions.IsLeaving() && !OwnedServer->IsClosing())) && Connection && (Connection->GetStatus() == network::GameSessionStatus::Failed ||
					Connection->GetStatus() == network::GameSessionStatus::Closed)))) {
				if (Connection) std::clog << "[Lifecycle:Connection] Connection retired: " << Connection->GetFailure() << std::endl;
				if (OwnedServer) OwnedServer->WithdrawListing();
				ReleaseGameplayInput();
				LostControl = ChangingHold = false;
				StopConnection();
				Session.reset();
				Surface.reset();
				Clock.reset();
				if (OwnedServer) RetiringServer = std::move(OwnedServer);
				App.ReturnToMenu();
				AppPublisher.RequestFullResync();
				App.Sessions.RetireDisconnected(App);
			}
			App.Sessions.Commit(App, [&](SessionCommand Command) {
				switch (Command) {
				case SessionCommand::Join:
				case SessionCommand::Host:
				case SessionCommand::StartLocal: {
					const bool Hosting = Command == SessionCommand::Host;
					const bool Private = App.Sessions.StartingPrivateServer() || Hosting;
					if (Command == SessionCommand::Join && Package.Inspection.Target &&
						Package.Inspection.Audience != PackageAudience::DistributedPlayer)
						throw SessionAdmissionFailure::UnsupportedTopology;
					const bool Joining = Command == SessionCommand::Join || Private;
					if (RetiringServer) return false;
					if (Private && !PrivateServer) throw SessionAdmissionFailure::UnsupportedTopology;
					if (Hosting && (!PrivateServer->Invitations || Package.Inspection.Audience != PackageAudience::DistributedPlayer))
						throw SessionAdmissionFailure::UnsupportedTopology;
					if (Joining && (Private || PinnedLaunch || Directory) && (Package.SpatialContent || Terrain))
						throw SessionAdmissionFailure::UnsupportedTopology;
					if (Joining && Package.SpatialContent && Package.Inspection.Audience != PackageAudience::DistributedPlayer)
						throw SessionAdmissionFailure::UnsupportedTopology;
					if (PreparingSession) {
						if (StorageAdmission) {
							PreparingSession->Script->GetPersistenceRuntime()->Pump();
							const auto Status = StorageAdmission->Poll().Status;
							if (Status == PersistenceStatus::Pending) return false;
							if (Status != PersistenceStatus::Ok && Status != PersistenceStatus::Missing)
								throw SessionAdmissionFailure::Unavailable;
							StorageAdmission.reset();
						}
						if (!AwaitingOwnerReady) {
							if (!PreparingSession->AdvanceStartup()) return false;
							if (Connection && !Connection->AttachClientRuntime(*PreparingSession)) throw SessionAdmissionFailure::Disconnected;
							AwaitingOwnerReady = Hosting;
						}
						if (Hosting) {
							(void)Connection->Poll();
							Connection->Step(++HandshakeTick);
							if (Connection->GetStatus() != network::GameSessionStatus::Ready) throw SessionAdmissionFailure::Disconnected;
							if (!OwnedServer->OwnerReady()) return false;
							OwnedServer->PublishGuests(*PrivateServer->Invitations, Package.Inspection.Identity.ToString(), PrivateServer->Manifest);
							AwaitingOwnerReady = false;
						}
						Session = std::move(PreparingSession);
						PendingGameplayResize.reset();
						Session->ProcessService->Alive = true;
						break;
					}
					if (!PreparingWorld) {
						if (Joining) {
#if defined(GARGANTUAN_WITH_GNS)
							if (!Connection) {
								if (Private) {
									if (!PrivatePackageReady.valid())
										PrivatePackageReady = std::async(std::launch::async, &PrivateServerSnapshot::Stage,
											PrivateServer->Root, PrivateServer->Manifest, Package.Inspection, Profile(), PackageCopy, PrivateServer->RuntimeEnrollment).share();
									if (!OwnedServer) {
										if (PrivatePackageReady.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
										std::shared_ptr<PrivateServerSnapshot> Snapshot;
										try { Snapshot = PrivatePackageReady.get(); }
										catch (const std::exception &) { PrivatePackageReady = {}; throw SessionAdmissionFailure::NotAuthorized; }
										OwnedServer = std::make_unique<PrivateServerProcess>(std::move(Snapshot), *PrivateServer, Profile(), App.Sessions.GetStartingSlot(), App.Sessions.HostCapacity());
									}
									PrivateGrant = OwnedServer->PollListening();
									if (!PrivateGrant) return false;
								}
								std::optional<PlayerAdmission> Selected;
								if (!Private && Browser) {
									const auto Id = App.Sessions.GetJoinListing();
									if (Id.empty()) throw SessionAdmissionFailure::NotAuthorized;
									Selected = Browser->Acquire(App.Sessions, Id);
									if (!Selected) return false;
								}
								const auto &Grant = Private ? PrivateGrant : Selected ? Selected : Admission;
								const auto Endpoint = Private || Selected ? std::optional(Grant->Endpoint) : ParseEndpoint(App.Sessions.GetJoinAddress());
								if (!Endpoint) throw SessionAdmissionFailure::InvalidOptions;
								if (!Private && PinnedLaunch) {
									if (!Admission || App.Sessions.InvitationExpired() || Endpoint->Host != Admission->Endpoint.Host || Endpoint->Port != Admission->Endpoint.Port)
										throw SessionAdmissionFailure::NotAuthorized;
								} else if (!Private && !Selected && (Package.Inspection.Configuration != PackageConfiguration::Development ||
									!network::IsLoopbackTransportEndpoint(*Endpoint))) throw SessionAdmissionFailure::UnsupportedTopology;
								if (Package.Inspection.Audience != PackageAudience::DistributedPlayer)
									ClientSources = PackageBuilder::LoadWorld(Package, Root);
								Connection = std::make_unique<network::GameSession>(std::make_shared<network::GameNetworkingSocketsTransport>(),
									ConfigurePackagedClient(Package, ClientSources, *Endpoint, Terrain, 0, false, Grant));
								if (Private) PrivateGrant.reset();
								else if (!Selected) { Admission.reset(); App.Sessions.ConsumeInvitation(); }
								if (!Connection->Start().Succeeded()) {
									std::clog << "[Lifecycle:Connection] Transport startup failed\n";
									throw SessionAdmissionFailure::Disconnected;
								}
							}
							(void)Connection->Poll();
							Connection->Step(++HandshakeTick);
							if (Connection->GetStatus() == network::GameSessionStatus::Failed) {
								std::clog << "[Lifecycle:Connection] Admission failed: " << Connection->GetFailure() << '\n';
								if (Connection->GetFailureReason() == network::DisconnectReason::AuthenticationFailure)
									throw SessionAdmissionFailure::NotAuthorized;
								throw SessionAdmissionFailure::Disconnected;
							}
							if (HandshakeTick >= network::DefaultGameSessionHandshakeTimeoutTicks) throw SessionAdmissionFailure::TimedOut;
							PreparingWorld = Connection->GetClientDataModel();
							if (!PreparingWorld) return false;
							auto Assets = std::dynamic_pointer_cast<AssetService>(PreparingWorld->GetService("AssetService"));
							Assets->LoadRuntimeAssetSnapshot(Package.Assets);
							if (Package.Inspection.Audience == PackageAudience::DistributedPlayer)
								(void)PackageBuilder::HydrateDistributedClientCode(Package, PreparingWorld);
							else (void)PackageBuilder::HydrateClientCode(ClientSources, PreparingWorld);
							ClientSources.reset();
#else
							throw SessionAdmissionFailure::UnsupportedTopology;
#endif
						} else {
							if (Package.Inspection.Audience == PackageAudience::DistributedPlayer)
								throw SessionAdmissionFailure::UnsupportedTopology;
							PreparingWorld = PackageBuilder::LoadWorld(Package, Root);
						}
						if (const auto *Slot = App.Sessions.GetStartingSlot(); Slot && !Joining) {
							Writer = std::make_unique<WorldWriter>(Profile(),
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
					Providers.EnableDefaultInteractionPresentation = Device.GetCapabilities().Graphical;
					Providers.DeferStartup = true;
					Providers.Mode = Joining ? RuntimeMode::NetworkClient : RuntimeMode::Offline;
					Providers.AdmitNetworkTerrain = Joining && Terrain;
					Providers.AdmitSpatialContent = Joining && Package.SpatialContent;
					if (const auto *Slot = App.Sessions.GetStartingSlot(); Slot && !Joining) {
						Providers.Persistence = MakeDurableLocalPersistenceProvider({
							.Path = Profile() / (Slot->SaveId + ".sqlite"),
							.ProfileId = Package.Inspection.Identity.ToString() + "/" + Slot->WorldId, .CreateIfMissing = false, .RecordWorldWrites = true});
						if (!Providers.Persistence) throw std::runtime_error("[Lifecycle:Storage] World writer unavailable");
					}
					if (!Joining) Providers.Content = PackageBuilder::GetLocalContentConfiguration(Package, Root, ContentResidencyMode::FullyResident);
					auto NewSession = std::make_unique<Engine>(PreparingWorld, NewSurface.get(), ReportPackagedRuntimeDiagnostic, std::move(Providers));
					if (const auto Storage = NewSession->Script->GetPersistenceRuntime()) Storage->TrackSessionWrites();
					if (!Joining && App.Sessions.GetStartingSlot()) StorageAdmission = NewSession->Script->GetPersistenceRuntime()->Submit({
						.Store = "WorldAdmission", .Key = "Profile"});
					NewSession->SetDevelopmentClock([NewClock] { return NewClock->Now(); });
					Surface = std::move(NewSurface);
					Clock = std::move(NewClock);
					PreparingSession = std::move(NewSession);
					PreparingWorld.reset();
					return false;
				}
				case SessionCommand::Leave: {
					if (!Draining) ReleaseGameplayInput();
					Draining = true;
					const auto Status = Drain(Session.get());
					if (Status == PersistenceStatus::Pending) return false;
					if (Status == PersistenceStatus::UnknownOutcome) throw SessionAdmissionFailure::UnknownOutcome;
					if (Status != PersistenceStatus::Ok) throw SessionAdmissionFailure::SaveFailed;
					if (OwnedServer) {
						try {
							if (!OwnedServer->Leave()) return false;
						} catch (SessionAdmissionFailure Failure) {
							if (!OwnedServer->IsAwaitingLeave() && Failure != SessionAdmissionFailure::Disconnected) throw;
							// A lost drain or exit receipt cannot safely return this authority to play.
							RetiringServer = std::move(OwnedServer);
							StopConnection();
							Session.reset();
							Writer.reset();
							Surface.reset();
							Clock.reset();
							AppPublisher.RequestFullResync();
							App.ReturnToMenu();
							throw SessionAdmissionFailure::UnconfirmedLeave;
						}
					}
					StopConnection();
					Session.reset();
					OwnedServer.reset();
					Writer.reset();
					Surface.reset();
					Clock.reset();
					AppPublisher.RequestFullResync();
					App.ReturnToMenu();
					break;
				}
				case SessionCommand::Pause:
				case SessionCommand::Resume:
					if (OwnedServer) {
						if (!ChangingHold) {
							ReleaseGameplayInput();
							Clock->SetHeld(SimulationTimeline::Hold::Gameplay, true);
							Session->Audio->SetPlaybackHeld(true);
							ChangingHold = true;
						}
						try {
							if (!OwnedServer->ApplyHold(Command == SessionCommand::Pause)) return false;
						} catch (SessionAdmissionFailure) {
							LostControl = true;
							return false;
						}
						ChangingHold = false;
					}
					if (!OwnedServer && Command == SessionCommand::Pause && !App.Sessions.IsPaused())
						ReleaseGameplayInput();
					Clock->SetHeld(SimulationTimeline::Hold::Gameplay, Command == SessionCommand::Pause);
					Session->Audio->SetPlaybackHeld(Command == SessionCommand::Pause);
					break;
				}
				return true;
			}, DiscardStart);
			if (Draining && Session && Clock) {
				const bool Held = App.Sessions.IsPaused() || App.Sessions.IsLeaving();
				Clock->SetHeld(SimulationTimeline::Hold::Gameplay, Held);
				Session->Audio->SetPlaybackHeld(Held);
			}
			if (!App.Sessions.IsLeaving()) Draining = false;
			if (App.GameplayInputReleasePending) ReleaseGameplayInput();
			if (App.QuitRequested) continue;
			RenderPublicationPtr Gameplay;
			App.FlushPresentation();
			if (Session && !ChangingHold && !App.Sessions.IsPaused() && !App.Sessions.IsLeaving()) {
				if (PendingGameplayResize) {
					(void)Session->ProcessEvent(*PendingGameplayResize);
					PendingGameplayResize.reset();
				}
				if (Connection) (void)Connection->Poll();
				const auto Closed = [&] {
					return Connection && (Connection->GetStatus() == network::GameSessionStatus::Closed ||
						Connection->GetStatus() == network::GameSessionStatus::Failed);
				};
				if (!Closed()) {
					if (SessionSmoke && Connection && App.GameplayInputEnabled) SmokeProbe.SubmitMovement(*Session);
					Session->Step();
					if (Connection) Connection->Step(Session->GetSimulationTick());
					if (SessionSmoke && Connection && SmokeProbe.Completed(*Session, *Connection, !Graphics))
						Session->ProcessService->MarkExit(0);
				}
				if (Closed()) {
					if (OwnedServer) OwnedServer->WithdrawListing();
					std::clog << "[Lifecycle:Connection] Active session closed: " << Connection->GetFailure() << '\n';
					ReleaseGameplayInput();
					StopConnection();
					Session.reset(); Surface.reset(); Clock.reset();
					if (OwnedServer) RetiringServer = std::move(OwnedServer);
					App.ReturnToMenu();
					AppPublisher.RequestFullResync();
					App.Sessions.RetireDisconnected(App);
					continue;
				}
				Gameplay = Surface->TakeLastPublication();
				if (!Session->ProcessService->Alive) {
					ExitCode = Session->ProcessService->ExitCode;
					App.QuitRequested = true;
				}
			} else if (Session) {
				if (OwnedServer && Connection) (void)Connection->PollPrivateHold();
				if (Session->Content) Session->Content->Step(false);
				if (const auto Storage = Session->Script->GetPersistenceRuntime()) Storage->Pump();
			}
			if (Gameplay && Gameplay->FullResync) AppPublisher.RequestFullResync();
			if (const auto Command = App.Gui.SynchronizeTextInput()) Platform.Apply(*Command);
			App.Gui.Publish(AppPublisher);
			const auto AppFrame = AppPublisher.PublishPresentation(Width, Height);
			const auto Submitted = Graphics ? Graphics->GetSubmittedFrames() : 0;
			Device.Draw(Display.Compose(Gameplay, *AppFrame, Session != nullptr));
			const auto Receipt = App.Gui.GetApplicationFrame();
			if ((!Graphics || Graphics->GetSubmittedFrames() != Submitted) && App.Gui.AcknowledgeApplicationFrame(Receipt)) {
				if (Receipt != Displayed) DisplayedSince = SDL_GetTicksNS();
				Displayed = Receipt;
			}
			const auto Deadline = Started + std::chrono::microseconds(16'667);
#if defined(GARGANTUAN_WORK_TRACE)
			TraceFrame.SetDeadline(Deadline, std::chrono::steady_clock::now());
			runtime_detail::WorkScope Pacing(runtime_detail::WorkPhase::HostFramePacing);
#endif
			std::this_thread::sleep_until(Deadline);
		}
#if defined(GARGANTUAN_WORK_TRACE)
		Trace.Write();
#endif
		return ExitCode;
	}
}

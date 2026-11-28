// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "StandalonePlayer.hpp"
#include "gargantuan/Engine.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "gargantuan/runtime/SimulationTimeline.hpp"
#include "platform/sdl/SDLHost.hpp"
#include <SDL3/SDL.h>
#include <iostream>

namespace gargantuan::host {
	namespace {
		class ApplicationDisplay final {
		  public:
			RenderPublicationPtr Compose(const RenderPublicationPtr &Gameplay, const RenderPublication &App, bool HasWorld) {
				auto Frame = Gameplay ? std::make_shared<RenderPublication>(*Gameplay) : std::make_shared<RenderPublication>();
				Frame->FullResync = Gameplay ? Gameplay->FullResync : !HasWorld && App.FullResync;
				if (LastId == std::numeric_limits<RenderPublicationId>::max())
					throw std::overflow_error("[Lifecycle:Display] Publication identity exhausted");
				Frame->BaseId = Frame->FullResync ? InvalidRenderPublicationId : LastId;
				Frame->Id = ++LastId;
				if (Gameplay) LastWorldFrame = Gameplay->Frame;
				if (!Gameplay) {
					Frame->Frame = HasWorld ? LastWorldFrame : App.Frame;
					Frame->Frame.ViewportWidth = App.Frame.ViewportWidth;
					Frame->Frame.ViewportHeight = App.Frame.ViewportHeight;
					Frame->EnvironmentChanged = Frame->FullResync;
				}
				if (!HasWorld) { WorldUi.reset(); WorldViews.reset(); }
				if (Gameplay && (Gameplay->UiChanged || Gameplay->FullResync)) WorldUi = Gameplay->SharedUi;
				if (Gameplay && (Gameplay->Viewports || Gameplay->FullResync)) WorldViews = Gameplay->Viewports;
				if (App.UiChanged || App.FullResync) AppUi = App.SharedUi;
				if (App.Viewports || App.FullResync) AppViews = App.Viewports;
				if (Frame->FullResync || Frame->UiChanged || App.UiChanged) {
					auto Ui = std::make_shared<RenderUiFrame>();
					if (WorldUi) *Ui = *WorldUi;
					Ui->ViewportWidth = App.Frame.ViewportWidth;
					Ui->ViewportHeight = App.Frame.ViewportHeight;
					if (AppUi) {
						for (const auto &Batch : AppUi->Batches) {
							auto &Copy = Ui->Batches.emplace_back(Batch);
							if (Copy.Texture) Copy.Texture->Generation |= ApplicationTextureGeneration;
						}
					}
					Frame->SharedUi = std::move(Ui);
					Frame->UiChanged = true;
				}
				if (Frame->FullResync || Frame->Viewports || App.Viewports) {
					auto Views = std::make_shared<RenderViewportScenes>();
					if (WorldViews) *Views = *WorldViews;
					if (AppViews) Views->insert(Views->end(), AppViews->begin(), AppViews->end());
					if (Views->size() > MaximumRenderViewports)
						throw std::length_error("[Lifecycle:Display] Combined viewport limit exceeded");
					Frame->Viewports = std::move(Views);
				}
				const auto AppendTextures = [](auto &World, const auto &Application) {
					for (const auto &Item : World)
						if (Item.Texture.Generation & ApplicationTextureGeneration)
							throw std::overflow_error("[Lifecycle:Display] Texture generation exhausted");
					for (const auto &Item : Application)
						World.emplace_back(Item).Texture.Generation |= ApplicationTextureGeneration;
				};
				AppendTextures(Frame->TextureCreates, App.TextureCreates);
				AppendTextures(Frame->TextureUpdates, App.TextureUpdates);
				AppendTextures(Frame->TextureRemoves, App.TextureRemoves);
				return Frame;
			}
		  private:
			static constexpr std::uint32_t ApplicationTextureGeneration = 1u << 31;
			RenderPublicationId LastId{};
			RenderFrameState LastWorldFrame;
			std::shared_ptr<const RenderUiFrame> WorldUi, AppUi;
			std::shared_ptr<const RenderViewportScenes> WorldViews, AppViews;
		};
	}

	int RunStandalonePlayer(ApplicationPresentation &App, BaseRenderer &Device,
		const RuntimePackagePayload &Package, const std::filesystem::path &Root, int MaximumFrames) {
		SDLHost Platform;
		RenderPublisher AppPublisher;
		ApplicationDisplay Display;
		GuiApplicationFrame Displayed{};
		auto *Graphics = dynamic_cast<SDLRenderer *>(&Device);
		std::unique_ptr<HeadlessRenderer> Surface;
		std::shared_ptr<SimulationTimeline> Clock;
		std::unique_ptr<Engine> Session;
		auto Last = std::chrono::steady_clock::now();
		std::cout << "[Lifecycle:Player] Application entry registered its title\n";
		for (std::uint64_t Frame = 0; !MaximumFrames || Frame < static_cast<std::uint64_t>(MaximumFrames); ++Frame) {
			HostEvent Event;
			while (Platform.PollEvent(Event)) {
				if (std::holds_alternative<WindowCloseEvent>(Event)) return 0;
				if (auto *Size = std::get_if<WindowResizeEvent>(&Event)) {
					if (!Size->Width || !Size->Height) continue;
					Device.Resize(Size->Width, Size->Height);
					if (Session) (void)Session->ProcessEvent(Event);
					App.Gui.SetViewport({Size->Width, Size->Height, 1.0f, {}});
					Displayed = {};
				} else {
					const bool Consumed = App.ProcessEvent(Event, Displayed);
					if (Session && !Consumed && App.GameplayInputEnabled && !App.Sessions.IsPaused()) {
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
			Last = Now;
			if (App.QuitRequested) return 0;
			App.Sessions.Commit(App, [&](SessionCommand Command) {
				switch (Command) {
				case SessionCommand::StartLocal: {
					auto World = PackageBuilder::LoadWorld(Package, Root);
					auto NewSurface = std::make_unique<HeadlessRenderer>(Vector2(Width, Height), HeadlessRenderer::PublicationMode::DeferredGpuPresentation);
					auto NewClock = std::make_shared<SimulationTimeline>();
					EngineProviderConfiguration Providers;
					Providers.AudioEnabled = Graphics != nullptr;
					Providers.Content = PackageBuilder::GetLocalContentConfiguration(Package, Root, ContentResidencyMode::FullyResident);
					auto NewSession = std::make_unique<Engine>(World, NewSurface.get(), nullptr, std::move(Providers));
					NewSession->SetDevelopmentClock([NewClock] { return NewClock->Now(); });
					NewSession->ProcessService->Alive = true;
					Surface = std::move(NewSurface);
					Clock = std::move(NewClock);
					Session = std::move(NewSession);
					break;
				}
				case SessionCommand::Leave:
					Session.reset();
					Surface.reset();
					Clock.reset();
					AppPublisher.RequestFullResync();
					break;
				case SessionCommand::Pause:
				case SessionCommand::Resume:
					if (Command == SessionCommand::Pause && !App.Sessions.IsPaused())
						(void)Session->ProcessEvent(FocusEvent{false});
					Clock->SetHeld(SimulationTimeline::Hold::Gameplay, Command == SessionCommand::Pause);
					Session->Audio->SetPlaybackHeld(Command == SessionCommand::Pause);
					break;
				}
			});
			if (App.QuitRequested) return 0;
			RenderPublicationPtr Gameplay;
			App.FlushPresentation();
			if (Session && !App.Sessions.IsPaused()) {
				Session->Step();
				Gameplay = Surface->TakeLastPublication();
				if (!Session->ProcessService->Alive) return Session->ProcessService->ExitCode;
			}
			if (Gameplay && Gameplay->FullResync) AppPublisher.RequestFullResync();
			if (const auto Command = App.Gui.SynchronizeTextInput()) Platform.Apply(*Command);
			App.Gui.Publish(AppPublisher);
			const auto AppFrame = AppPublisher.PublishPresentation(Width, Height);
			const auto Submitted = Graphics ? Graphics->GetMetrics().FramesSubmitted : 0;
			Device.Draw(Display.Compose(Gameplay, *AppFrame, Session != nullptr));
			const auto Receipt = App.Gui.GetApplicationFrame();
			if ((!Graphics || Graphics->GetMetrics().FramesSubmitted != Submitted) && App.Gui.AcknowledgeApplicationFrame(Receipt))
				Displayed = Receipt;
			SDL_Delay(16);
		}
		return 0;
	}
}

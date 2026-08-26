#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/FileLink.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <chrono>
#include <filesystem>
#include <glm/glm.hpp>
#include <lua.h>
#include <memory>
#include <optional>

namespace gargantuan {
	Engine::Engine(
		std::shared_ptr<gargantuan::DataModel> game,
		BaseRenderer *renderer,
		std::function<void(std::string, std::string)> RuntimeDiagnostic,
		EngineProviderConfiguration ProviderConfiguration
	)
		: DataModel(game), Renderer(renderer),
		  Script(std::make_unique<class ScriptEngine>(game, std::move(RuntimeDiagnostic))),
		  Workspace(GetService<gargantuan::Workspace>()),
		  WorldRoot(std::static_pointer_cast<gargantuan::WorldRoot>(Workspace)),
		  RunService(GetService<gargantuan::RunService>()), ProcessService(GetService<gargantuan::ProcessService>()),
		  UserInputService(GetService<gargantuan::UserInputService>()), ActionMap(GetService<gargantuan::ActionMap>()),
		  Assets(GetService<gargantuan::AssetService>()),
		  Audio(std::make_unique<AudioRuntime>(
			  Assets,
			  ProviderConfiguration.AudioEnabled ? CreateSdlAudioBackend() : nullptr,
			  [](std::string Code, std::string Message) {
				  LOG_WARN(App, "[Audio:Runtime] %s: %s", Code.c_str(), Message.c_str());
			  }
		  )),
		  Entitlements(GetService<gargantuan::EntitlementService>()),
		  Interaction(GetService<gargantuan::InteractionService>()), Players(GetService<gargantuan::Players>()) {
		if (ProviderConfiguration.Entitlements &&
			!Entitlements->ConfigureProvider(std::move(ProviderConfiguration.Entitlements)))
			LOG_WARN(
				App, "[Backend:Entitlements] configured provider did not become ready; offline semantics remain active"
			);
		const auto DefaultGuiFont = Paths::GetExecutableDirectory() / "runtime" / "GargantuanSans.ttf";
		ActionMap->AttachInputService(UserInputService);
		Players->InitializeLocalPlayer();
		Interaction->AttachRuntime(
			DataModel, Players, ActionMap, Renderer && dynamic_cast<HeadlessRenderer *>(Renderer) == nullptr
		);
		Assets->ConfigureBuiltInFont(DefaultGuiFont);
		Gui = std::make_unique<GuiRuntime>(DataModel, DefaultGuiFont);
		if (Renderer) {
			const auto [Width, Height] = Renderer->GetViewportSize();
			Gui->SetViewport({Width, Height, 1.0f, {}});
		}
		if (DataModel->Filesystem) ProjectSources = std::make_unique<SourceMount>(*DataModel->Filesystem);

		UnbindDescendants = DataModel->BindDescendants([this](std::shared_ptr<Instance> inst) {
			if (auto script = std::dynamic_pointer_cast<gargantuan::Script>(inst)) {
				this->Script->ScriptQueue.insert(script);
			}
			if (auto SoundValue = std::dynamic_pointer_cast<gargantuan::Sound>(inst)) Audio->RegisterSound(SoundValue);

			if (auto link = std::dynamic_pointer_cast<gargantuan::FileLink>(inst)) {
				if (!ProjectSources) {
					LOG_WARN(
						App, "[Project:SourceMount] FileLink '%s' has no project filesystem mount",
						inst->GetFullName().c_str()
					);
					return;
				}
				auto Result = link->Synchronize(*ProjectSources);
				if (!Result) LOG_WARN(
					App, "[Project:SourceMount] FileLink '%s' failed: %s",
					inst->GetFullName().c_str(), Result.error().Format().c_str()
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

		Players->StartDefaultRuntime();
		Interaction->StartDefaultRuntime();

		LOG_INFO(App, "Constructed engine");
	}

	Engine::~Engine() { Destroy(); }

	void Engine::Destroy() {
		if (Destroyed) return;
		Destroyed = true;
		LOG_INFO(App, "Destroying engine");
		if (DataModelDestroyingConnection) {
			DataModelDestroyingConnection->Disconnect();
			DataModelDestroyingConnection.reset();
		}
		if (Interaction && !Interaction->GetDestroyed()) Interaction->ShutdownRuntime();
		if (Players && !Players->GetDestroyed()) Players->ShutdownRuntime();
		if (ActionMap && !ActionMap->GetDestroyed()) ActionMap->Reset();
		if (Audio) Audio->Shutdown();
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
		if (!GuiConsumed) Result.Consumed = ActionMap->ProcessEvent(Event) || Result.Consumed;
		else {
			ActionMap->ProcessConsumedRelease(Event);
			Result.Consumed = true;
		}
		if (!Result.Consumed) Result.Command = Workspace->GetCurrentCamera()->ProcessEvent(Event);
		if (auto InputCommand = UserInputService->SynchronizeMouseBehavior()) Result.Command = InputCommand;
		if (Gui) if (auto TextInputCommand = Gui->SynchronizeTextInput()) Result.Command = TextInputCommand;
		return Result;
	}

	bool Engine::ReplaceEntitlementProvider(std::shared_ptr<IEntitlementProvider> Provider) {
		return Entitlements && !Entitlements->GetDestroyed() && Entitlements->ConfigureProvider(std::move(Provider));
	}

	void Engine::Step() {
		if (!ProcessService->Alive) return;
		Mutations.Drain();

		CurrentTick = std::chrono::steady_clock::now();
		if (LastTick.time_since_epoch().count() == 0) LastTick = CurrentTick;
		float deltaTime = GetDeltaTime();

		{
			G_PROFILE("Main Thread");

			{
				G_PROFILE("Simulation");
				RunService->PreSimulation->Fire(deltaTime);
				WorldRoot->StepPhysics(deltaTime, std::nullopt);
				Workspace->GetCurrentCamera()->Step(deltaTime);
				RunService->PostSimulation->Fire(deltaTime);
				Interaction->Step(CurrentTick);
			}

			{
				G_PROFILE("PreRender");
				RunService->PreRender->Fire(deltaTime);
				if (Gui) {
					auto MeshChanges = Assets->DrainMeshChanges();
					if (!MeshChanges.Creates.empty() || !MeshChanges.Removes.empty())
						RenderPublishing.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
					(void)Gui->Reconcile();
					Gui->Publish(RenderPublishing);
				}
			}

			ActionMap->EndFrame();
			UserInputService->EndFrame();

			{
				G_PROFILE("Draw");
				const auto [viewportWidth, viewportHeight] = Renderer->GetViewportSize();
				auto camera = Workspace->GetCurrentCamera();
				auto Publication = RenderPublishing.Publish(
					*WorldRoot, MakeRenderCameraInput(*camera), viewportWidth, viewportHeight
				);
				try {
					Renderer->Draw(std::move(Publication));
				} catch (...) {
					RenderPublishing.RequestFullResync();
					throw;
				}
			}

			{
				G_PROFILE("Scripts");
				if (Entitlements) (void)Entitlements->PumpAsyncCompletions();
				Script->Step();
				if (Audio) Audio->Step(Workspace->GetCurrentCamera()->GetCFrame());
			}
		}

		G_PROFILE_FRAME();

		LastTick = CurrentTick;
	}
}

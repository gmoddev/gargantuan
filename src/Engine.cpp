#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/FileLink.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/Script.hpp"
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
		std::function<void(std::string, std::string)> RuntimeDiagnostic
	)
		: DataModel(game), Renderer(renderer), Script(std::make_unique<class ScriptEngine>(game, std::move(RuntimeDiagnostic))),
		  Workspace(GetService<gargantuan::Workspace>()),
		  WorldRoot(std::static_pointer_cast<gargantuan::WorldRoot>(Workspace)),
		  RunService(GetService<gargantuan::RunService>()), ProcessService(GetService<gargantuan::ProcessService>()),
		  UserInputService(GetService<gargantuan::UserInputService>()) {

		DataModel->BindDescendants([this](std::shared_ptr<Instance> inst) {
			if (auto script = std::dynamic_pointer_cast<gargantuan::Script>(inst)) {
				this->Script->ScriptQueue.insert(script);
				inst->Destroying->Once([ScriptEngine = this->Script.get(), script](std::monostate _) {
					if (ScriptEngine->ScriptQueue.contains(script)) ScriptEngine->ScriptQueue.erase(script);
				});
			}

			if (auto link = std::dynamic_pointer_cast<gargantuan::FileLink>(inst)) {
				auto relativePath = link->GetPath();
				auto absolutePath = std::filesystem::absolute(this->DataModel->Root / relativePath);
				LOG_INFO(
					App,
					"Got file link: %s, %s %s %s",
					inst->GetClassName().c_str(),
					inst->GetFullName().c_str(),
					Paths::ToUtf8(absolutePath).c_str(),
					relativePath.c_str()
				);
				link->Synchronize(absolutePath);
			}
		});

		DataModel->DescendantRemoved->Connect([this](std::shared_ptr<Instance> inst) {
			if (auto script = std::dynamic_pointer_cast<gargantuan::Script>(inst);
				script && Script->ScriptQueue.contains(script)) {
				Script->ScriptQueue.erase(script);
			}
		});

		LOG_INFO(App, "Constructed engine");
	}

	Engine::~Engine() { Destroy(); }

	void Engine::Destroy() {
		if (Destroyed) return;
		Destroyed = true;
		LOG_INFO(App, "Destroying engine");
		if (Renderer) Renderer->Destroy();
		if (WorldRoot) WorldRoot->Destroy();
		Script.reset();
	}

	float Engine::GetDeltaTime() {
		return std::chrono::duration<float>(CurrentTick - LastTick).count();
	}

	HostEventResult Engine::ProcessEvent(const HostEvent &Event) {
		HostEventResult Result;
		if (const auto *Resize = std::get_if<WindowResizeEvent>(&Event)) {
			Renderer->Resize(static_cast<int>(Resize->Width), static_cast<int>(Resize->Height));
			Workspace->GetCurrentCamera()->SetViewportSize(Vector2(Resize->Width, Resize->Height));
			return Result;
		}
		if (std::holds_alternative<WindowCloseEvent>(Event)) {
			LOG_INFO(App, "Stopping engine");
			ProcessService->MarkExit(0);
			return Result;
		}

		Result.Consumed = UserInputService->ProcessEvent(Event);
		if (!Result.Consumed) Result.Command = Workspace->GetCurrentCamera()->ProcessEvent(Event);
		return Result;
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
			}

			{
				G_PROFILE("PreRender");
				RunService->PreRender->Fire(deltaTime);
			}

			{
				G_PROFILE("Draw");
				const auto [viewportWidth, viewportHeight] = Renderer->GetViewportSize();
				auto camera = Workspace->GetCurrentCamera();
				auto snapshot = RenderExtraction.Extract(
					*WorldRoot, MakeRenderCameraInput(*camera), viewportWidth, viewportHeight
				);
				Renderer->Draw(std::move(snapshot));
			}

			{
				G_PROFILE("Scripts");
				Script->Step();
			}
		}

		G_PROFILE_FRAME();

		LastTick = CurrentTick;
	}
}

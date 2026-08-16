#pragma once

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/RunService.hpp"
#include "gargantuan/services/UserInputService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/platform/HostEvent.hpp"

#include <chrono>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <lua.h>
#include <memory>
#include <type_traits>

namespace gargantuan {
	// TODO: Move much of this into DataModel
	struct Engine {
	  public:
		std::shared_ptr<DataModel> DataModel;
		BaseRenderer *Renderer;
		std::unique_ptr<ScriptEngine> Script;
		MutationGateway Mutations;
		RenderExtractor RenderExtraction;

		std::shared_ptr<Workspace> Workspace;
		std::shared_ptr<WorldRoot> WorldRoot;
		std::shared_ptr<RunService> RunService;
		std::shared_ptr<ProcessService> ProcessService;
		std::shared_ptr<UserInputService> UserInputService;

		bool IsRunning = true;

		Engine(
			std::shared_ptr<gargantuan::DataModel> game,
			BaseRenderer *renderer,
			std::function<void(std::string, std::string)> RuntimeDiagnostic = {}
		);
		~Engine();

		void Step();
		float GetDeltaTime();
		[[nodiscard]] HostEventResult ProcessEvent(const HostEvent &Event);
		void Destroy();

	  private:
		std::chrono::steady_clock::time_point CurrentTick{};
		std::chrono::steady_clock::time_point LastTick{};
		bool Destroyed = false;

		template <typename T>
			requires std::is_base_of_v<Instance, T>
		std::shared_ptr<T> GetService() {
			return std::dynamic_pointer_cast<T>(this->DataModel->GetService(T::CLASS_DEFINITION.ClassName));
		}
	};

} // namespace gargantuan

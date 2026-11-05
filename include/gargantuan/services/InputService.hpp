#pragma once

#include "gargantuan/input/CompiledInputGraph.hpp"
#include "gargantuan/services/generated/InputService.hpp"

#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	// This signal is native-only and has no reflected property or Luau access path.
	template <> struct StackValue<CompiledInputGraph::Transition> {
		static int Push(lua_State *, const CompiledInputGraph::Transition &) {
			return 0;
		}
	};
	class InputAction;
	class InputService : public Instance {
		I_InputService;

	  public:
		~InputService() override;
		void EnsureCompiled();
		[[nodiscard]] std::uint64_t GetDefinitionRevision() {
			EnsureCompiled();
			return DefinitionRevision;
		}
		[[nodiscard]] std::span<const std::weak_ptr<InputAction>> GetCompiledActions() {
			EnsureCompiled();
			return CompiledActions;
		}
		[[nodiscard]] bool IsPublishing() const {
			return Publishing;
		}
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event, bool GuiConsumed = false);
		void EndFrame();
		void Reset(CompiledInputGraph::Reason Reason = CompiledInputGraph::Reason::FocusLost);
		[[nodiscard]] CompiledInputGraph::Snapshot Inspect();
		[[nodiscard]] const std::vector<std::string> &GetValidationErrors() const {
			return ValidationErrors;
		}
		void RegisterLegacyContext(ObjectId Context);
		Signal<CompiledInputGraph::Transition>::Pointer Transitioned =
			std::make_shared<Signal<CompiledInputGraph::Transition>>();

	  private:
		CompiledInputGraph Graph;
		bool Dirty = true;
		bool Publishing = false;
		bool Observing = false;
		std::uint64_t DefinitionRevision = 0;
		ObjectId LegacyContext;
		struct DefinitionObserver {
			std::uint64_t Revision = 0;
			std::vector<std::pair<BaseSignal::Pointer, SignalConnection::Pointer>> Connections;
		};
		std::unordered_map<ObjectId, DefinitionObserver> DefinitionObservers;
		std::vector<SignalConnection::Pointer> HierarchyConnections;
		std::vector<std::string> ValidationErrors;
		std::vector<std::weak_ptr<InputAction>> FrameActions;
		std::vector<std::weak_ptr<InputAction>> CompiledActions;
		void Observe();
		void Publish();
		void Watch(const std::shared_ptr<Instance> &Object, std::initializer_list<const char *> Properties);
		static void Disconnect(const DefinitionObserver &Observer);
	};
}

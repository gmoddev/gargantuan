#pragma once

#include "gargantuan/classes/InputObject.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/services/generated/ActionMap.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gargantuan {
	class Engine;
	class UserInputService;
	class InputService;
	class InputContext;
	class InputAction;
	class InputBinding;
	struct ActionMapTestAccess;

	// Compatibility names and binding IDs over InputService's canonical graph.
	class ActionMap : public Instance {
		I_ActionMap;

	  public:
		static constexpr std::size_t MaximumActions = 128;
		static constexpr std::size_t MaximumBindings = 512;
		static constexpr std::size_t MaximumActionNameBytes = 64;
		~ActionMap() override;

	  private:
		std::shared_ptr<InputService> LocalInput;
		std::weak_ptr<InputService> RuntimeInput;
		std::weak_ptr<InputContext> LegacyContext;
		std::unordered_map<int, std::weak_ptr<InputBinding>> Bindings;
		std::unordered_map<std::string, std::weak_ptr<InputAction>> OwnedActions;
		std::unordered_map<std::string, std::weak_ptr<InputAction>> NamedActions;
		std::unordered_map<ObjectId, std::string> PublishedNames;
		SignalConnection::Pointer TransitionConnection;
		std::uint64_t CachedDefinitionRevision = 0;
		bool NamesNeedPublication = false;
		bool RefreshingNames = false;
		bool RuntimeStopping = false;
		int NextBindingId = 1;
		friend class Engine;
		friend struct ActionMapTestAccess;
		void AttachInputService(const std::shared_ptr<UserInputService> &Service);
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event);
		void ProcessConsumedRelease(const HostEvent &Event);
		void EndFrame();
		void Reset();
		[[nodiscard]] std::shared_ptr<InputService> GetInput();
		[[nodiscard]] std::shared_ptr<InputContext> GetLegacyContext();
		[[nodiscard]] std::shared_ptr<InputAction> GetAction(const std::string &Name);
		[[nodiscard]] int
		AddBinding(const std::string &Name, const std::shared_ptr<InputBinding> &Binding, bool Pointer);
		void RefreshNames(ObjectId CurrentTransition = {});
		void CleanBindings();
	};
}

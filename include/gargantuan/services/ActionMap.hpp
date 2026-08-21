#pragma once

#include "gargantuan/classes/InputObject.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/services/generated/ActionMap.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	class Engine;
	class UserInputService;
	struct ActionMapTestAccess;

	class ActionMap : public Instance {
		I_ActionMap;

	  public:
		static constexpr std::size_t MaximumActions = 128;
		static constexpr std::size_t MaximumBindings = 512;
		static constexpr std::size_t MaximumActionNameBytes = 64;

	  private:
		enum class BindingKind : std::uint8_t { Key, MouseButton, PointerDelta };
		struct Binding {
			int Id = 0;
			std::string ActionName;
			BindingKind Kind = BindingKind::Key;
			Enums::KeyCode KeyCode = Enums::KeyCode::None;
			Enums::UserInputType InputType = Enums::UserInputType::None;
			float ScalarScale = 1.0f;
			Vector2 VectorScale{1.0f, 1.0f};
			int Priority = 0;
			bool Consume = false;
		};

		struct ActionState {
			float Scalar = 0.0f;
			Vector2 Vector;
			bool Down = false;
		};

		std::weak_ptr<UserInputService> InputService;
		std::vector<Binding> Bindings;
		std::unordered_map<std::string, ActionState> States;
		int NextBindingId = 1;

		friend class Engine;
		friend struct ActionMapTestAccess;
		void AttachInputService(const std::shared_ptr<UserInputService> &Service);
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event);
		void EndFrame();
		void Reset();
		[[nodiscard]] int AddBinding(Binding Value);
		void RefreshAction(const std::string &ActionName);
		[[nodiscard]] bool IsBindingActive(const Binding &Value) const;
	};
}

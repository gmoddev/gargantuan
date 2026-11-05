// Benchmark-only frozen evaluator from eb2ce26ab. Renamed; public generated declarations expanded.
#pragma once

#include "gargantuan/classes/InputObject.hpp"
#include "gargantuan/platform/HostEvent.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/Vector2.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	class Engine;
	class UserInputService;
	struct ActionMapBaselineTestAccess;

	class ActionMapBaseline : public Instance {
	  public:
		int BindKey(std::string ActionName, Enums::KeyCode KeyCode, float Scale, int Priority, bool Consume);
		int BindMouseButton(std::string ActionName, Enums::UserInputType InputType, float Scale, int Priority, bool Consume);
		int BindPointerDelta(std::string ActionName, Vector2 Scale, int Priority, bool Consume);
		bool Unbind(int BindingId);
		void UnbindAction(std::string ActionName);
		bool IsDown(std::string ActionName);
		float GetValue(std::string ActionName);
		Vector2 GetVector(std::string ActionName);
		int GetBindingCount();
		Signal<std::string>::Pointer ActionBegan = std::make_shared<Signal<std::string>>();
		Signal<std::tuple<std::string, Vector2>>::Pointer ActionChanged = std::make_shared<Signal<std::tuple<std::string, Vector2>>>();
		Signal<std::string>::Pointer ActionEnded = std::make_shared<Signal<std::string>>();

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
		friend struct ActionMapBaselineTestAccess;
		void AttachInputService(const std::shared_ptr<UserInputService> &Service);
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event);
		void ProcessConsumedRelease(const HostEvent &Event);
		void EndFrame();
		void Reset();
		[[nodiscard]] int AddBinding(Binding Value);
		void RefreshAction(const std::string &ActionName);
		[[nodiscard]] bool IsBindingActive(const Binding &Value) const;
	};
}

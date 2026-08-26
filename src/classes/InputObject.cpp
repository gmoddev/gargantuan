#include "gargantuan/classes/InputObject.hpp"

#include <memory>
#include <type_traits>
#include <unordered_map>

namespace gargantuan {
	const std::unordered_map<Enums::ModifierKey, std::unordered_set<Enums::KeyCode>> MODIFIER_TO_KEYCODE = {
		{Enums::ModifierKey::Ctrl, {Enums::KeyCode::LeftControl, Enums::KeyCode::RightControl}},
		{Enums::ModifierKey::Alt, {Enums::KeyCode::LeftAlt, Enums::KeyCode::RightAlt}},
		{Enums::ModifierKey::Shift, {Enums::KeyCode::LeftShift, Enums::KeyCode::RightShift}},
		{Enums::ModifierKey::Meta, {Enums::KeyCode::LeftMeta, Enums::KeyCode::RightMeta}},
	};

	static_assert(static_cast<int>(LogicalKey::A) == static_cast<int>(Enums::KeyCode::A));
	static_assert(static_cast<int>(LogicalKey::F15) == static_cast<int>(Enums::KeyCode::F15));
	static_assert(static_cast<int>(LogicalKey::Undo) == static_cast<int>(Enums::KeyCode::Undo));
	static_assert(static_cast<int>(LogicalKey::Unknown) == static_cast<int>(Enums::KeyCode::None));

	std::optional<Enums::KeyCode> InputObject::GetGamepadKeyCode(GamepadButton Button) {
		switch (Button) {
		case GamepadButton::South: return Enums::KeyCode::ButtonA;
		case GamepadButton::East: return Enums::KeyCode::ButtonB;
		case GamepadButton::West: return Enums::KeyCode::ButtonX;
		case GamepadButton::North: return Enums::KeyCode::ButtonY;
		case GamepadButton::Back: return Enums::KeyCode::ButtonSelect;
		case GamepadButton::Start: return Enums::KeyCode::ButtonStart;
		case GamepadButton::LeftStick: return Enums::KeyCode::ButtonL3;
		case GamepadButton::RightStick: return Enums::KeyCode::ButtonR3;
		case GamepadButton::LeftShoulder: return Enums::KeyCode::ButtonL1;
		case GamepadButton::RightShoulder: return Enums::KeyCode::ButtonR1;
		case GamepadButton::DPadUp: return Enums::KeyCode::DPadUp;
		case GamepadButton::DPadDown: return Enums::KeyCode::DPadDown;
		case GamepadButton::DPadLeft: return Enums::KeyCode::DPadLeft;
		case GamepadButton::DPadRight: return Enums::KeyCode::DPadRight;
		default: return std::nullopt;
		}
	}

	bool InputObject::IsModifierKeyDown(Enums::ModifierKey ModifierKey) {
		return MODIFIER_TO_KEYCODE.at(ModifierKey).contains(KeyCode);
	}

	std::shared_ptr<InputObject> InputObject::FromHostEvent(const HostEvent &Event) {
		return std::visit([](const auto &Value) -> std::shared_ptr<InputObject> {
			using EventType = std::decay_t<decltype(Value)>;
			auto Input = std::make_shared<InputObject>();
			if constexpr (std::is_same_v<EventType, KeyEvent>) {
				if (Value.Logical == LogicalKey::Unknown) return nullptr;
				Input->UserInputType = Enums::UserInputType::Keyboard;
				Input->UserInputState = Value.State == ButtonState::Pressed ? Enums::UserInputState::Begin
					: Enums::UserInputState::End;
				Input->KeyCode = static_cast<Enums::KeyCode>(Value.Logical);
				return Input;
			} else if constexpr (std::is_same_v<EventType, PointerButtonEvent>) {
				switch (Value.Button) {
				case PointerButton::Left: Input->UserInputType = Enums::UserInputType::MouseButton1; break;
				case PointerButton::Right: Input->UserInputType = Enums::UserInputType::MouseButton2; break;
				case PointerButton::Middle: Input->UserInputType = Enums::UserInputType::MouseButton3; break;
				default: return nullptr;
				}
				Input->UserInputState = Value.State == ButtonState::Pressed ? Enums::UserInputState::Begin
					: Enums::UserInputState::End;
				Input->Position = glm::vec3(Value.Position.X, Value.Position.Y, 0.0f);
				return Input;
			} else if constexpr (std::is_same_v<EventType, PointerMoveEvent>) {
				Input->UserInputType = Enums::UserInputType::MouseMovement;
				Input->UserInputState = Enums::UserInputState::Change;
				Input->Delta = glm::vec3(Value.Delta.X, Value.Delta.Y, 0.0f);
				Input->Position = glm::vec3(Value.Position.X, Value.Position.Y, 0.0f);
				return Input;
			} else if constexpr (std::is_same_v<EventType, WheelEvent>) {
				Input->UserInputType = Enums::UserInputType::MouseWheel;
				Input->UserInputState = Enums::UserInputState::Change;
				Input->Delta = glm::vec3(Value.Delta.X, Value.Delta.Y, 0.0f);
				Input->Position = glm::vec3(Value.Position.X, Value.Position.Y, 0.0f);
				return Input;
			} else if constexpr (std::is_same_v<EventType, GamepadButtonEvent>) {
				auto KeyCode = GetGamepadKeyCode(Value.Button);
				if (!KeyCode) return nullptr;
				Input->UserInputType = Enums::UserInputType::Gamepad1;
				Input->UserInputState = Value.State == ButtonState::Pressed ? Enums::UserInputState::Begin
					: Enums::UserInputState::End;
				Input->KeyCode = *KeyCode;
				return Input;
			} else {
				return nullptr;
			}
		}, Event);
	}
}

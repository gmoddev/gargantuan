#include "gargantuan/services/UserInputService.hpp"

#include <memory>
#include <vector>

namespace gargantuan {
	bool IsMouseButtonType(Enums::UserInputType inputType) {
		return inputType == Enums::UserInputType::MouseButton1 || inputType == Enums::UserInputType::MouseButton2 ||
			   inputType == Enums::UserInputType::MouseButton3;
	}

	std::vector<std::shared_ptr<InputObject>> UserInputService::GetKeysPressed() {
		std::vector<std::shared_ptr<InputObject>> result;
		result.reserve(ActiveKeys.size());
		for (const auto &[_, inputObject] : ActiveKeys) {
			result.push_back(inputObject);
		}
		return result;
	}

	Enums::UserInputType UserInputService::GetLastInputType() {
		return LastInputType;
	}

	Vector2 UserInputService::GetMouseDelta() {
		return MouseDelta;
	}

	std::vector<std::shared_ptr<InputObject>> UserInputService::GetMouseButtonsPressed() {
		std::vector<std::shared_ptr<InputObject>> result;
		result.reserve(ActiveMouseButtons.size());
		for (const auto &[_, inputObject] : ActiveMouseButtons) {
			result.push_back(inputObject);
		}
		return result;
	}

	Vector2 UserInputService::GetMouseLocation() {
		return MouseLocation;
	}

	bool UserInputService::IsKeyDown(Enums::KeyCode keyCode) {
		return ActiveKeys.contains(keyCode);
	}

	bool UserInputService::IsMouseButtonPressed(Enums::UserInputType mouseType) {
		return ActiveMouseButtons.contains(mouseType);
	}

	bool UserInputService::ProcessEvent(const HostEvent &Event) {
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && Focus->Focused) {
			WindowFocused->Fire({});
			return false;
		}
		if (const auto *Focus = std::get_if<FocusEvent>(&Event); Focus && !Focus->Focused) {
			ActiveKeys.clear();
			ActiveMouseButtons.clear();
			MouseDelta = Vector2(0.0f, 0.0f);
			if (MouseBehavior != Enums::MouseBehavior::Default) {
				MouseBehavior = Enums::MouseBehavior::Default;
				NotifyPropertyCommitted("MouseBehavior");
			}
			WindowFocusReleased->Fire({});
			return false;
		};

		auto input = InputObject::FromHostEvent(Event);
		if (!input) return false;

		auto inputType = input->GetUserInputType();
		auto inputState = input->GetUserInputState();

		if (LastInputType != inputType) {
			LastInputType = inputType;
			LastInputTypeChanged->Fire(inputType);
		}

		if (inputState == Enums::UserInputState::Begin) {
			if (inputType == Enums::UserInputType::Keyboard) {
				if (!ActiveKeys.contains(input->GetKeyCode())) ActiveKeys.emplace(input->GetKeyCode(), input);
				if (input->GetKeyCode() == Enums::KeyCode::Space) JumpRequest->Fire({});
			} else if (IsMouseButtonType(inputType)) {
				if (!ActiveMouseButtons.contains(input->GetUserInputType()))
					ActiveMouseButtons.emplace(input->GetUserInputType(), input);
			}
			InputBegan->Fire({input, false});
		} else if (inputState == Enums::UserInputState::Change) {
			if (inputType == Enums::UserInputType::MouseMovement) {
				MouseDelta = Vector2(input->GetDelta());
				MouseLocation = Vector2(input->GetPosition());
			}
			InputChanged->Fire({input, false});
		} else if (inputState == Enums::UserInputState::End) {
			if (inputType == Enums::UserInputType::Keyboard) {
				if (ActiveKeys.contains(input->GetKeyCode())) ActiveKeys.erase(input->GetKeyCode());
			} else if (IsMouseButtonType(inputType)) {
				ActiveMouseButtons.erase(input->GetUserInputType());
			}
			InputEnded->Fire({input, false});
		}
		return false;
	}
	std::optional<HostCommand> UserInputService::SynchronizeMouseBehavior() {
		const bool Requested = MouseBehavior != Enums::MouseBehavior::Default;
		if (Requested == RelativePointerMode) return std::nullopt;
		RelativePointerMode = Requested;
		return HostCommand{SetRelativePointerMode{Requested}};
	}

	void UserInputService::EndFrame() {
		MouseDelta = Vector2();
	}
}

#include "gargantuan/classes/Camera.hpp"

#include <glm/glm.hpp>
#include <type_traits>

namespace gargantuan {
	float Camera::GetAspectRatio() const {
		return ViewportSize.GetY() > 0.0f ? ViewportSize.GetX() / ViewportSize.GetY() : 1.0f;
	}

	float Camera::GetHorizontalFieldOfView() const {
		return glm::degrees(2 * glm::atan(GetAspectRatio() * glm::tan(glm::radians(FieldOfView) / 2)));
	}

	void Camera::SetHorizontalFieldOfView(float fovy) {
		ValidatePropertyMutation("HorizontalFieldOfView", fovy);
		SetFieldOfView(glm::degrees(2 * glm::atan(1 / GetAspectRatio() * glm::tan(glm::radians(fovy) / 2))));
		GetPropertyChangedSignal("HorizontalFieldOfView")->Fire({});
	}

	float Camera::GetDiagonalFieldOfView() const {
		return glm::degrees(
			2 * glm::atan(glm::sqrt(1 + glm::pow(GetAspectRatio(), 2)) * glm::tan(glm::radians(FieldOfView) / 2))
		);
	}

	void Camera::SetDiagonalFieldOfView(float fovy) {
		ValidatePropertyMutation("DiagonalFieldOfView", fovy);
		SetFieldOfView(
			glm::degrees(
				2 * glm::atan(1 / glm::sqrt(1 + glm::pow(GetAspectRatio(), 2)) * glm::tan(glm::radians(fovy) / 2))
			)
		);
		GetPropertyChangedSignal("DiagonalFieldOfView")->Fire({});
	}

	glm::mat4 Camera::GetProjectionMatrix() {
		return glm::perspective(glm::radians(FieldOfView), GetAspectRatio(), 0.1f, 100000.0f);
	}

	glm::mat4 Camera::GetViewMatrix() {
		glm::vec3 position = CFrame.Position;
		return glm::lookAt(position, position + CFrame.GetLookVector(), CFrame.GetUpVector());
	}

	std::optional<HostCommand> Camera::ProcessEvent(const HostEvent &Event) {
		if (CameraType != Enums::CameraType::Freecam) {
			return std::nullopt;
		}

		return std::visit([this](const auto &Value) -> std::optional<HostCommand> {
			using EventType = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<EventType, KeyEvent>) {
				if (Value.Physical != PhysicalKey::Unknown) {
					if (Value.State == ButtonState::Pressed) PressedKeys.insert(Value.Physical);
					else PressedKeys.erase(Value.Physical);
				}
			} else if constexpr (std::is_same_v<EventType, PointerButtonEvent>) {
				if (Value.Button == PointerButton::Right) {
					RelativePointerMode = Value.State == ButtonState::Pressed;
					return HostCommand{SetRelativePointerMode{RelativePointerMode}};
				}
			} else if constexpr (std::is_same_v<EventType, PointerMoveEvent>) {
				if (RelativePointerMode) {
					AccumulatedDeltaX += Value.Delta.X;
					AccumulatedDeltaY += Value.Delta.Y;
				}
			} else if constexpr (std::is_same_v<EventType, FocusEvent>) {
				if (!Value.Focused) {
					PressedKeys.clear();
					if (RelativePointerMode) {
						RelativePointerMode = false;
						return HostCommand{SetRelativePointerMode{false}};
					}
				}
			}
			return std::nullopt;
		}, Event);
	}

	void Camera::Step(float deltaTime) {
		if (CameraType != Enums::CameraType::Freecam) {
			return;
		}

		if (AccumulatedDeltaX != 0.0f || AccumulatedDeltaY != 0.0f) {
			Yaw -= AccumulatedDeltaX * FreecamSensitivity;

			Pitch -= AccumulatedDeltaY * FreecamSensitivity;
			Pitch = glm::clamp(Pitch, -89.0f, 89.0f);

			AccumulatedDeltaX = 0.0f;
			AccumulatedDeltaY = 0.0f;

			auto rotation = CFrame::fromEulerAnglesYXZ(glm::radians(Pitch), glm::radians(Yaw), 0.0f);
			SetCFrame(gargantuan::CFrame(CFrame.Position, rotation.Rotation));
		}

		auto lookVector = CFrame.GetLookVector();
		auto rightVector = CFrame.GetRightVector();
		auto upVector = CFrame.GetUpVector();

		if (PressedKeys.contains(PhysicalKey::W)) {
			SetCFrame(gargantuan::CFrame(CFrame.Position + lookVector * FreecamSpeed * deltaTime, CFrame.Rotation));
		}

		if (PressedKeys.contains(PhysicalKey::S)) {
			SetCFrame(gargantuan::CFrame(CFrame.Position - lookVector * FreecamSpeed * deltaTime, CFrame.Rotation));
		}

		if (PressedKeys.contains(PhysicalKey::A)) {
			SetCFrame(gargantuan::CFrame(CFrame.Position - rightVector * FreecamSpeed * deltaTime, CFrame.Rotation));
		}

		if (PressedKeys.contains(PhysicalKey::D)) {
			SetCFrame(gargantuan::CFrame(CFrame.Position + rightVector * FreecamSpeed * deltaTime, CFrame.Rotation));
		}

		if (PressedKeys.contains(PhysicalKey::Space)) {
			SetCFrame(gargantuan::CFrame(CFrame.Position + glm::vec3(0, FreecamSpeed * deltaTime, 0), CFrame.Rotation));
		}

		// complex and volatile so i can screenshot on macos
		bool shiftPressed = PressedKeys.contains(PhysicalKey::LeftShift) || PressedKeys.contains(PhysicalKey::RightShift);
		bool guiPressed = PressedKeys.contains(PhysicalKey::LeftMeta) || PressedKeys.contains(PhysicalKey::RightMeta);
		if (shiftPressed && !guiPressed) {
			SetCFrame(gargantuan::CFrame(CFrame.Position - glm::vec3(0, FreecamSpeed * deltaTime, 0), CFrame.Rotation));
		}
	}
}

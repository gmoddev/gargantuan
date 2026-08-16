#pragma once

#include "gargantuan/classes/generated/Camera.hpp"
#include "gargantuan/platform/HostEvent.hpp"

#include <optional>
#include <unordered_set>

namespace gargantuan {
	G_ENUM(
		CameraType,
		/// Camera is stationary.
		Fixed,
		/// Camera moves with the subject at a fixed offset and will rotate as the subject rotates.
		Attach,
		/// Camera is stationary but will rotate to keep the subject in the center of the screen.
		Watch,
		/// Camera moves with the subject but does not rotate automatically.
		Track,
		/// Camera moves with the subject and rotates to keep the subject in the center.
		Follow,
		/// Default mode used by Gargantuan.
		Custom,
		/// No default behavior. Used when developers need to script custom behavior.
		Scriptable,
		/// The camera has a fixed Y position, but can be rotated around the player.
		Orbital,
		/// Camera has omnidirectional movement.
		Freecam,
	);

	class Camera : public Instance {
		I_Camera;

		float AccumulatedDeltaX = 0.0f;
		float AccumulatedDeltaY = 0.0f;
		float FreecamSpeed = 10.0f;
		float FreecamSensitivity = 0.2f;
		bool RelativePointerMode = false;
		std::unordered_set<PhysicalKey> PressedKeys;

		glm::mat4 GetProjectionMatrix();
		glm::mat4 GetViewMatrix();

		[[nodiscard]] std::optional<HostCommand> ProcessEvent(const HostEvent &Event);
		void Step(float deltaTime);
	};
}

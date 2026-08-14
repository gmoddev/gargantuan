#define GLM_ENABLE_EXPERIMENTAL

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/datatypes/CFrame.hpp"

#include <glm/gtx/euler_angles.hpp>

namespace gargantuan {
	glm::vec3 BasePart::GetPosition() const {
		return GetCFrame().Position;
	}

	void BasePart::SetPosition(glm::vec3 value) {
		ValidatePropertyMutation("Position", value);
		SetCFrame(gargantuan::CFrame(value, GetCFrame().Rotation));
	}

	glm::vec3 BasePart::GetRotation() const {
		float rx, ry, rz;
		glm::extractEulerAngleYXZ(glm::mat4(GetCFrame().Rotation), ry, rx, rz);
		return glm::vec3(rx, ry, rz);
	}

	void BasePart::SetRotation(glm::vec3 value) {
		ValidatePropertyMutation("Rotation", value);
		glm::mat3 rotMatrix = glm::mat3(glm::eulerAngleYXZ(value.y, value.x, value.z));
		SetCFrame(gargantuan::CFrame(GetCFrame().Position, rotMatrix));
	}

	void BasePart::ApplyImpulse(glm::vec3 impulse) {
		AccumulatedImpulse += impulse;
	}

}

#pragma once

#include "gargantuan/datatypes/CFrame.hpp"

#include <box3d/box3d.h>
#include <box3d/math_functions.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace gargantuan::Box3DConversions {
	inline b3Vec3 ToBox3(const glm::vec3 &Vector) {
		return b3Vec3{Vector.x, Vector.y, Vector.z};
	}

	inline b3Quat ToBox3(const glm::quat &Quaternion) {
		return b3NormalizeQuat(b3Quat{{Quaternion.x, Quaternion.y, Quaternion.z}, Quaternion.w});
	}

	inline glm::vec3 FromBox3(const b3Vec3 &Vector) {
		return {Vector.x, Vector.y, Vector.z};
	}

	inline glm::quat FromBox3(const b3Quat &Quaternion) {
		return {Quaternion.s, Quaternion.v.x, Quaternion.v.y, Quaternion.v.z};
	}

	inline CFrame FromBox3(const b3WorldTransform &Transform) {
		return CFrame(FromBox3(Transform.p), glm::mat3_cast(FromBox3(Transform.q)));
	}
}

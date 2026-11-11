#pragma once

#include "gargantuan/classes/generated/ReflectionProbe.hpp"

namespace gargantuan {
	class ReflectionProbe final : public Instance {
		I_ReflectionProbe;
	  private:
		CFrame ProbeCFrame = gargantuan::CFrame();
		glm::vec3 ProbeSize = glm::vec3(16.0f);
		glm::vec3 ProbeCaptureOffset = glm::vec3(0.0f);
		float ProbeBlendDistance = 1.0f;
		float ProbeIntensity = 1.0f;
		int ProbePriority = 0;
		std::string ProbePositiveX = "";
		std::string ProbeNegativeX = "";
		std::string ProbePositiveY = "";
		std::string ProbeNegativeY = "";
		std::string ProbePositiveZ = "";
		std::string ProbeNegativeZ = "";
		void SetFace(std::string &Destination, std::string Value, const char *PropertyName);
	};
}

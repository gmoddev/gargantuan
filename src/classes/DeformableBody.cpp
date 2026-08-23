#include "gargantuan/classes/DeformableBody.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	namespace {
		[[nodiscard]] bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}
	}

	void DeformableBody::ApplyForce(glm::vec3 Force) {
		AssertCanMutate();
		const auto Candidate = AccumulatedForce + Force;
		if (!IsFinite(Force) || !IsFinite(Candidate))
			throw std::invalid_argument("[Physics:SoftBody] ApplyForce requires a finite bounded value");
		AccumulatedForce = Candidate;
	}

	void DeformableBody::ApplyImpulse(glm::vec3 Impulse) {
		AssertCanMutate();
		const auto Candidate = AccumulatedImpulse + Impulse;
		if (!IsFinite(Impulse) || !IsFinite(Candidate))
			throw std::invalid_argument("[Physics:SoftBody] ApplyImpulse requires a finite bounded value");
		AccumulatedImpulse = Candidate;
	}

	void DeformableBody::ApplyImpulseAtPosition(glm::vec3 Impulse, glm::vec3 Position) {
		AssertCanMutate();
		if (!IsFinite(Impulse) || !IsFinite(Position))
			throw std::invalid_argument("[Physics:SoftBody] ApplyImpulseAtPosition requires finite values");
		if (AccumulatedPointImpulses.size() == MaximumSoftBodyPointImpulsesPerStep)
			throw std::runtime_error("[Physics:SoftBody] Point impulse limit reached for this fixed step");
		AccumulatedPointImpulses.push_back({Impulse, Position});
	}
}

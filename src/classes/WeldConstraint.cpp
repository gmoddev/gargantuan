#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

namespace gargantuan {
	std::tuple<std::shared_ptr<BasePart>, std::shared_ptr<BasePart>> WeldConstraint::GetActiveParts() const {
		if (!Part0.has_value() || !Part0.value()) return {nullptr, nullptr};
		auto part0 = Part0.value();
		if (!WorldRoot::GetEffectiveWorld(*part0)) return {nullptr, nullptr};

		if (!Part1.has_value() || !Part1.value()) return {nullptr, nullptr};
		auto part1 = Part1.value();
		if (!WorldRoot::GetEffectiveWorld(*part1)) return {nullptr, nullptr};

		return {part0, part1};
	};

	PhysicsConstraintDesc WeldConstraint::GetPhysicsConstraint(
		PhysicsBodyId BodyA,
		PhysicsBodyId BodyB
	) const {
		return {
			.Kind = PhysicsConstraintKind::Weld,
			.BodyA = BodyA,
			.BodyB = BodyB,
			.CollideConnected = true,
		};
	}
}

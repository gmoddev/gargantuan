// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <glm/glm.hpp>
#include <array>

namespace gargantuan::blockgrid {
	struct RigidMass {
		double Mass{};
		glm::dvec3 Center{};
		glm::dmat3 WorldInertia{0.};
	};
	struct RigidVelocity { glm::dvec3 Linear{},Angular{}; };
	[[nodiscard]] std::array<RigidVelocity,2> SplitMomentum(const RigidMass& Original,RigidVelocity Velocity,
		const RigidMass& A,const RigidMass& B);
	[[nodiscard]] RigidVelocity MergeMomentum(const RigidMass& A,RigidVelocity VelocityA,
		const RigidMass& B,RigidVelocity VelocityB,const RigidMass& Merged);
}

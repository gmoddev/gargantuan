// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	SoftBodyWorld::SoftBodyWorld(SoftBodyWorldLimits Limits) : Backend(CreateSoftBodyBackend(Limits)) {
		if (!Backend || !Backend->IsValid())
			throw std::runtime_error("[Physics:SoftBody] Failed to create deformable world");
	}

	SoftBodyWorld::SoftBodyWorld(std::unique_ptr<ISoftBodyBackend> Backend) : Backend(std::move(Backend)) {
		if (!this->Backend || !this->Backend->IsValid())
			throw std::invalid_argument("[Physics:SoftBody] SoftBodyWorld requires a valid backend");
	}

	SoftBodyWorld::~SoftBodyWorld() = default;
	SoftBodyWorld::SoftBodyWorld(SoftBodyWorld &&) noexcept = default;
	SoftBodyWorld &SoftBodyWorld::operator=(SoftBodyWorld &&) noexcept = default;

	bool SoftBodyWorld::IsValid() const { return Backend && Backend->IsValid(); }
	bool SoftBodyWorld::IsBodyValid(SoftBodyId Body) const { return Backend && Backend->IsBodyValid(Body); }
	SoftBodyId SoftBodyWorld::CreateBody(const SoftBodyDefinition &Definition) {
		return Backend ? Backend->CreateBody(Definition) : SoftBodyId{};
	}
	PhysicsOperationResult SoftBodyWorld::UpdateBody(SoftBodyId Body, const SoftBodyDefinition &Definition) {
		return Backend ? Backend->UpdateBody(Body, Definition)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Soft-body backend is unavailable"};
	}
	PhysicsOperationResult SoftBodyWorld::DestroyBody(SoftBodyId Body) {
		return Backend ? Backend->DestroyBody(Body)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Soft-body backend is unavailable"};
	}
	PhysicsOperationResult SoftBodyWorld::ApplyForce(SoftBodyId Body, glm::vec3 Force) {
		return Backend ? Backend->ApplyForce(Body, Force)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Soft-body backend is unavailable"};
	}
	PhysicsOperationResult SoftBodyWorld::ApplyImpulse(SoftBodyId Body, glm::vec3 Impulse) {
		return Backend ? Backend->ApplyImpulse(Body, Impulse)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Soft-body backend is unavailable"};
	}
	std::optional<SoftBodyState> SoftBodyWorld::GetBodyState(SoftBodyId Body) const {
		return Backend ? Backend->GetBodyState(Body) : std::nullopt;
	}
	SoftBodyStepResult SoftBodyWorld::Step(const SoftBodyStepConfig &Config) {
		return Backend ? Backend->Step(Config) : SoftBodyStepResult{};
	}
	const SoftBodyWorldLimits &SoftBodyWorld::GetLimits() const {
		if (!Backend) throw std::logic_error("Soft-body backend is unavailable");
		return Backend->GetLimits();
	}
}

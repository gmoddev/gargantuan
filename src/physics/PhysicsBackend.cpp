#include "gargantuan/physics/PhysicsBackend.hpp"

#include <stdexcept>
#include <utility>

namespace gargantuan {
	PhysicsWorld::PhysicsWorld(const PhysicsWorldConfig &Config) : Backend(CreatePhysicsBackend(Config)) {
		if (!Backend || !Backend->IsValid()) throw std::runtime_error("[Physics:Backend] Failed to create physics world");
	}

	PhysicsWorld::PhysicsWorld(std::unique_ptr<IPhysicsBackend> Backend) : Backend(std::move(Backend)) {
		if (!this->Backend || !this->Backend->IsValid())
			throw std::invalid_argument("[Physics:Backend] PhysicsWorld requires a valid backend");
	}

	PhysicsWorld::~PhysicsWorld() = default;
	PhysicsWorld::PhysicsWorld(PhysicsWorld &&) noexcept = default;
	PhysicsWorld &PhysicsWorld::operator=(PhysicsWorld &&) noexcept = default;

	bool PhysicsWorld::IsValid() const { return Backend && Backend->IsValid(); }
	bool PhysicsWorld::IsBodyValid(PhysicsBodyId Body) const { return Backend && Backend->IsBodyValid(Body); }
	bool PhysicsWorld::IsConstraintValid(PhysicsConstraintId Constraint) const {
		return Backend && Backend->IsConstraintValid(Constraint);
	}
	PhysicsBodyId PhysicsWorld::CreateBody(const PhysicsBodyDesc &Description) {
		return Backend ? Backend->CreateBody(Description) : PhysicsBodyId{};
	}
	PhysicsOperationResult PhysicsWorld::UpdateBody(PhysicsBodyId Body, const PhysicsBodyDesc &Description) {
		return Backend ? Backend->UpdateBody(Body, Description)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Physics backend is unavailable"};
	}
	PhysicsOperationResult PhysicsWorld::DestroyBody(PhysicsBodyId Body) {
		return Backend ? Backend->DestroyBody(Body)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Physics backend is unavailable"};
	}
	PhysicsConstraintId PhysicsWorld::CreateConstraint(const PhysicsConstraintDesc &Description) {
		return Backend ? Backend->CreateConstraint(Description) : PhysicsConstraintId{};
	}
	PhysicsOperationResult PhysicsWorld::DestroyConstraint(PhysicsConstraintId Constraint) {
		return Backend ? Backend->DestroyConstraint(Constraint)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Physics backend is unavailable"};
	}
	PhysicsOperationResult PhysicsWorld::ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse) {
		return Backend ? Backend->ApplyLinearImpulse(Body, Impulse)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Physics backend is unavailable"};
	}
	PhysicsOperationResult PhysicsWorld::SetGravity(glm::vec3 Gravity) {
		return Backend ? Backend->SetGravity(Gravity)
			: PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Physics backend is unavailable"};
	}
	std::optional<PhysicsBodyState> PhysicsWorld::GetBodyState(PhysicsBodyId Body) const {
		return Backend ? Backend->GetBodyState(Body) : std::nullopt;
	}
	PhysicsKinematicMotionResult
	PhysicsWorld::MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const {
		return Backend ? Backend->MoveKinematicCapsule(Request)
					   : PhysicsKinematicMotionResult{
							 .Status = PhysicsOperationStatus::BackendFailure,
							 .Message = "Physics backend is unavailable",
						 };
	}
	PhysicsRaycastResult PhysicsWorld::Raycast(const PhysicsRaycastRequest &Request) const {
		return Backend ? Backend->Raycast(Request)
					   : PhysicsRaycastResult{
							 .Status = PhysicsOperationStatus::BackendFailure,
							 .Message = "Physics backend is unavailable",
						 };
	}
	PhysicsStepResult PhysicsWorld::Step(const PhysicsStepConfig &Config) {
		return Backend ? Backend->Step(Config) : PhysicsStepResult{};
	}
}

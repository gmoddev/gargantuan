#pragma once

#include "gargantuan/physics/PhysicsQueryTypes.hpp"

#include <memory>
#include <optional>

namespace gargantuan {
	class IPhysicsBackend {
	  public:
		virtual ~IPhysicsBackend() = default;

		[[nodiscard]] virtual bool IsValid() const = 0;
		[[nodiscard]] virtual bool IsBodyValid(PhysicsBodyId Body) const = 0;
		[[nodiscard]] virtual bool IsConstraintValid(PhysicsConstraintId Constraint) const = 0;
		[[nodiscard]] virtual PhysicsBodyId CreateBody(const PhysicsBodyDesc &Description) = 0;
		virtual PhysicsOperationResult UpdateBody(PhysicsBodyId Body, const PhysicsBodyDesc &Description) = 0;
		virtual PhysicsOperationResult DestroyBody(PhysicsBodyId Body) = 0;
		[[nodiscard]] virtual PhysicsConstraintId CreateConstraint(const PhysicsConstraintDesc &Description) = 0;
		[[nodiscard]] virtual PhysicsConstraintCreateResult TryCreateConstraint(const PhysicsConstraintDesc &Description) {
			const auto Constraint = CreateConstraint(Description);
			return {Constraint, Constraint.IsValid() ? PhysicsOperationResult{} :
				PhysicsOperationResult{PhysicsOperationStatus::BackendFailure, "Constraint creation failed"}};
		}
		virtual PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Constraint) = 0;
		virtual PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse) = 0;
		virtual PhysicsOperationResult ApplyAngularImpulse(PhysicsBodyId, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Angular impulse is unsupported"}; }
		virtual PhysicsOperationResult ApplyImpulseAtPosition(PhysicsBodyId, glm::vec3, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Point impulse is unsupported"}; }
		virtual PhysicsOperationResult ApplyForce(PhysicsBodyId, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Force is unsupported"}; }
		virtual PhysicsOperationResult ApplyTorque(PhysicsBodyId, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Torque is unsupported"}; }
		virtual PhysicsOperationResult SetLinearVelocity(PhysicsBodyId, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Linear velocity is unsupported"}; }
		virtual PhysicsOperationResult SetAngularVelocity(PhysicsBodyId, glm::vec3) { return {PhysicsOperationStatus::BackendFailure, "Angular velocity is unsupported"}; }
		virtual PhysicsOperationResult SetAwake(PhysicsBodyId, bool) { return {PhysicsOperationStatus::BackendFailure, "Sleep control is unsupported"}; }
		virtual PhysicsOperationResult SetGravity(glm::vec3 Gravity) = 0;
		[[nodiscard]] virtual std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Body) const = 0;
		[[nodiscard]] virtual PhysicsCollisionResidency GetCollisionResidency() const { return {}; }
		[[nodiscard]] virtual PhysicsKinematicMotionResult
		MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const = 0;
		[[nodiscard]] virtual PhysicsRaycastResult Raycast(const PhysicsRaycastRequest &Request) const = 0;
		[[nodiscard]] virtual PhysicsRaycastResult ShapeCast(const PhysicsShapeCastRequest &) const { return {.Status = PhysicsOperationStatus::BackendFailure, .Message = "Shape casts are unsupported"}; }
		[[nodiscard]] virtual PhysicsOverlapResult Overlap(const PhysicsOverlapRequest &) const { return {.Status = PhysicsOperationStatus::BackendFailure, .Message = "Overlaps are unsupported"}; }
		[[nodiscard]] virtual PhysicsStepResult Step(const PhysicsStepConfig &Config) = 0;
	};

	[[nodiscard]] std::unique_ptr<IPhysicsBackend> CreatePhysicsBackend(const PhysicsWorldConfig &Config);

	class PhysicsWorld {
	  public:
		explicit PhysicsWorld(const PhysicsWorldConfig &Config);
		explicit PhysicsWorld(std::unique_ptr<IPhysicsBackend> Backend);
		~PhysicsWorld();

		PhysicsWorld(const PhysicsWorld &) = delete;
		PhysicsWorld &operator=(const PhysicsWorld &) = delete;
		PhysicsWorld(PhysicsWorld &&) noexcept;
		PhysicsWorld &operator=(PhysicsWorld &&) noexcept;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool IsBodyValid(PhysicsBodyId Body) const;
		[[nodiscard]] bool IsConstraintValid(PhysicsConstraintId Constraint) const;
		[[nodiscard]] PhysicsBodyId CreateBody(const PhysicsBodyDesc &Description);
		PhysicsOperationResult UpdateBody(PhysicsBodyId Body, const PhysicsBodyDesc &Description);
		PhysicsOperationResult DestroyBody(PhysicsBodyId Body);
		[[nodiscard]] PhysicsConstraintId CreateConstraint(const PhysicsConstraintDesc &Description);
		[[nodiscard]] PhysicsConstraintCreateResult TryCreateConstraint(const PhysicsConstraintDesc &Description);
		PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Constraint);
		PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse);
		PhysicsOperationResult ApplyAngularImpulse(PhysicsBodyId Body, glm::vec3 Impulse);
		PhysicsOperationResult ApplyImpulseAtPosition(PhysicsBodyId Body, glm::vec3 Impulse, glm::vec3 Position);
		PhysicsOperationResult ApplyForce(PhysicsBodyId Body, glm::vec3 Force);
		PhysicsOperationResult ApplyTorque(PhysicsBodyId Body, glm::vec3 Torque);
		PhysicsOperationResult SetLinearVelocity(PhysicsBodyId Body, glm::vec3 Velocity);
		PhysicsOperationResult SetAngularVelocity(PhysicsBodyId Body, glm::vec3 Velocity);
		PhysicsOperationResult SetAwake(PhysicsBodyId Body, bool Awake);
		PhysicsOperationResult SetGravity(glm::vec3 Gravity);
		[[nodiscard]] std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Body) const;
		[[nodiscard]] PhysicsCollisionResidency GetCollisionResidency() const;
		[[nodiscard]] PhysicsKinematicMotionResult
		MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const;
		[[nodiscard]] PhysicsRaycastResult Raycast(const PhysicsRaycastRequest &Request) const;
		[[nodiscard]] PhysicsRaycastResult ShapeCast(const PhysicsShapeCastRequest &Request) const;
		[[nodiscard]] PhysicsOverlapResult Overlap(const PhysicsOverlapRequest &Request) const;
		[[nodiscard]] PhysicsStepResult Step(const PhysicsStepConfig &Config);

	  private:
		std::unique_ptr<IPhysicsBackend> Backend;
	};
}

#pragma once

#include "gargantuan/physics/PhysicsTypes.hpp"

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
		virtual PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Constraint) = 0;
		virtual PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse) = 0;
		virtual PhysicsOperationResult SetGravity(glm::vec3 Gravity) = 0;
		[[nodiscard]] virtual std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Body) const = 0;
		[[nodiscard]] virtual PhysicsKinematicMotionResult
		MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const = 0;
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
		PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Constraint);
		PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse);
		PhysicsOperationResult SetGravity(glm::vec3 Gravity);
		[[nodiscard]] std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Body) const;
		[[nodiscard]] PhysicsKinematicMotionResult
		MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const;
		[[nodiscard]] PhysicsStepResult Step(const PhysicsStepConfig &Config);

	  private:
		std::unique_ptr<IPhysicsBackend> Backend;
	};
}

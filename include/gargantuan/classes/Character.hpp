#pragma once

#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/generated/Character.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <cstdint>

namespace gargantuan {
	class WorldRoot;

	enum class CharacterMotionSource : std::uint8_t {
		Script,
		Animation,
	};

	struct CharacterMotionRequest {
		glm::vec3 Translation{0.0f};
		glm::vec3 Velocity{0.0f};
		float YawRadians = 0.0f;
		CharacterMotionSource Source = CharacterMotionSource::Script;
		bool LocalSpace = false;
	};

	struct CharacterMotionResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		glm::vec3 RequestedTranslation{0.0f};
		glm::vec3 AppliedTranslation{0.0f};
		glm::vec3 Position{0.0f};
		glm::vec3 Velocity{0.0f};
		glm::vec3 ContactNormal{0.0f};
		glm::vec3 FloorNormal{0.0f};
		float RequestedYawRadians = 0.0f;
		float AppliedYawRadians = 0.0f;
		std::uint64_t PhysicsCpuNanoseconds = 0;
		bool Collided = false;
		bool HasFloor = false;
		bool PlanesTruncated = false;

		[[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
	};

	class Character : public Folder {
		I_Character;

		CFrame Transform{0.0f, 6.0f, 0.0f};
		std::shared_ptr<BasePart> RootPartValue;

		void SynchronizeRootPart(bool Simulation = false);
		void CommitSimulationTransform(const CFrame &Value);

	  public:
		static constexpr float MaximumMotionTranslation = 16.0f;
		static constexpr float MaximumMotionYawRadians = 1.57079632679489661923f;

		[[nodiscard]] CharacterMotionResult AdmitMotion(
			WorldRoot &World,
			const CharacterMotionRequest &Request
		);
	};
}

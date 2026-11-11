#pragma once

#include "gargantuan/classes/generated/BasePart.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <glm/glm.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace gargantuan {
	class Character;
	struct WorldRootTestAccess;

	class BasePart : public Instance {
		I_BasePart;

		glm::vec3 AccumulatedImpulse = {0.0f, 0.0f, 0.0f};
		glm::vec3 AccumulatedAngularImpulse{0.0f};
		glm::vec3 AccumulatedForce{0.0f};
		glm::vec3 AccumulatedTorque{0.0f};
		std::vector<std::pair<glm::vec3, glm::vec3>> AccumulatedPointImpulses;
		std::optional<glm::vec3> PendingLinearVelocity;
		std::optional<glm::vec3> PendingAngularVelocity;
		std::optional<bool> PendingAwake;
		PhysicsBodyState PublishedPhysicsState{};
		static constexpr std::size_t MaximumPointImpulsesPerStep = 64;
		std::optional<gargantuan::CFrame> CharacterPresentationOffset;
		[[nodiscard]] virtual PhysicsShapeDesc GetPhysicsShape() const = 0;
		friend class Character;
		friend class WorldRoot;
		friend struct WorldRootTestAccess;
		void NotifyPendingPhysicsControl();
		void SetCharacterSimulationCFrame(const gargantuan::CFrame &Value);
		void SetCharacterPresentationOffset(std::optional<gargantuan::CFrame> Value);

	  public:
		[[nodiscard]] gargantuan::CFrame GetRenderCFrame() const;

	};
} // namespace gargantuan

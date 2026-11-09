#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <vector>

namespace PhysicsEvaluation {
    // Available only in the private contact qualification executable.
    void SetExperimentalXpbdClothRotation(glm::quat Rotation);
    [[nodiscard]] std::optional<std::vector<glm::vec3>> GetExperimentalXpbdVelocities(
        const gargantuan::ISoftBodyBackend &Soft, gargantuan::SoftBodyId Body);
    void FinishExperimentalXpbdTrace();
    void SetExperimentalJoltClothRotation(glm::quat Rotation);
    [[nodiscard]] bool MoveExperimentalJoltKinematic(
        gargantuan::IPhysicsBackend &Rigid, gargantuan::PhysicsBodyId Body,
        glm::vec3 Center, float DeltaTime);
}

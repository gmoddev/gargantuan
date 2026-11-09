#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <vector>

namespace PhysicsEvaluation {
    // Available only in the private contact qualification executable.
    struct XpbdContactProjection {
        glm::dvec3 VelocityDerivedMomentumDeltaNs{0.0};
        glm::dvec3 NormalImpulseOnClothNs{0.0};
        glm::dvec3 FrictionImpulseOnClothNs{0.0};
        glm::dvec3 PrescribedColliderReactionImpulseNs{0.0};
        double ContactKineticDeltaJ = 0.0;
        double PrescribedSurfaceWorkJ = 0.0;
        double KineticStartJ = 0.0;
        double KineticEndJ = 0.0;
        double AppliedDynamicsKineticDeltaJ = 0.0;
        double DampingKineticDeltaJ = 0.0;
        double StructuralProjectionKineticDeltaJ = 0.0;
        double PinProjectionKineticDeltaJ = 0.0;
        double LedgerResidualJ = 0.0;
        std::uint64_t Corrections = 0;
    };
    void SetExperimentalXpbdClothRotation(glm::quat Rotation);
    // One-way prescribed translating sphere motion for private G only. Call
    // before Step; the joined Step snapshots this ordered list once per body.
    void SetExperimentalXpbdColliderDeltas(std::vector<glm::vec3> Deltas);
    void SetExperimentalXpbdAccountingEnabled(bool Enabled);
    [[nodiscard]] glm::vec3 ResolveExperimentalXpbdRelativeMovement(
        glm::vec3 ClothMovement, glm::vec3 SurfaceMovement,
        glm::vec3 ContactNormal, float Friction);
    [[nodiscard]] std::optional<std::vector<glm::vec3>> GetExperimentalXpbdVelocities(
        const gargantuan::ISoftBodyBackend &Soft, gargantuan::SoftBodyId Body);
    [[nodiscard]] std::optional<XpbdContactProjection> GetExperimentalXpbdContactProjection(
        const gargantuan::ISoftBodyBackend &Soft, gargantuan::SoftBodyId Body);
    void FinishExperimentalXpbdTrace();
    void SetExperimentalJoltClothRotation(glm::quat Rotation);
    [[nodiscard]] bool MoveExperimentalJoltKinematic(
        gargantuan::IPhysicsBackend &Rigid, gargantuan::PhysicsBodyId Body,
        glm::vec3 Center, float DeltaTime);
}

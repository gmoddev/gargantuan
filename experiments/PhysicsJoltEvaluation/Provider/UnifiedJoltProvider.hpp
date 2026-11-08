#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace PhysicsEvaluation {
    // Test-only composition. Soft must retire before Rigid because both use the
    // same native Jolt PhysicsSystem owned by Rigid.
    struct UnifiedJoltProvider {
        std::unique_ptr<gargantuan::IPhysicsBackend> Rigid;
        std::unique_ptr<gargantuan::ISoftBodyBackend> Soft;
    };

    [[nodiscard]] UnifiedJoltProvider CreateUnifiedJoltProvider(
        const gargantuan::PhysicsWorldConfig &Config,
        gargantuan::SoftBodyWorldLimits Limits = {}, int WorkerBudget = 1);

#ifdef PHYSICS_GANTRIA_ATTACHMENTS
    // Experimental neutral-ID bridge. A live connection belongs to the rigid
    // provider and is detached before either native endpoint is retired.
    struct JoltRigidSoftAttachmentState {
        bool Active = false;
        std::uint64_t AppliedSteps = 0;
        glm::vec3 LastImpulseOnSoft{0.0f};
    };
    [[nodiscard]] gargantuan::PhysicsOperationResult ConnectJoltRigidSoftAttachment(
        gargantuan::IPhysicsBackend &Rigid,
        gargantuan::ISoftBodyBackend &Soft,
        gargantuan::SoftBodyId SoftId, std::uint32_t Vertex,
        gargantuan::PhysicsBodyId RigidId, glm::vec3 RigidLocalPoint);
    [[nodiscard]] gargantuan::PhysicsOperationResult DisconnectJoltRigidSoftAttachment(
        gargantuan::IPhysicsBackend &Rigid,
        gargantuan::SoftBodyId SoftId, gargantuan::PhysicsBodyId RigidId);
    [[nodiscard]] std::optional<JoltRigidSoftAttachmentState> GetJoltRigidSoftAttachmentState(
        const gargantuan::IPhysicsBackend &Rigid,
        gargantuan::SoftBodyId SoftId, gargantuan::PhysicsBodyId RigidId);
#endif
}

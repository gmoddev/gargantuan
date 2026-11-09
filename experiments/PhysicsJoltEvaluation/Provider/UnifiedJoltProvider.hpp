#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace PhysicsEvaluation {
    // Test-only composition. Soft must retire before Rigid because both use the
    // same native Jolt PhysicsSystem owned by Rigid.
    struct UnifiedJoltProvider {
        std::unique_ptr<gargantuan::IPhysicsBackend> Rigid;
        std::unique_ptr<gargantuan::ISoftBodyBackend> Soft;

        UnifiedJoltProvider() = default;
        UnifiedJoltProvider(std::unique_ptr<gargantuan::IPhysicsBackend> RigidValue,
                            std::unique_ptr<gargantuan::ISoftBodyBackend> SoftValue)
            : Rigid(std::move(RigidValue)), Soft(std::move(SoftValue)) {}
        UnifiedJoltProvider(const UnifiedJoltProvider &) = delete;
        UnifiedJoltProvider &operator=(const UnifiedJoltProvider &) = delete;
        UnifiedJoltProvider(UnifiedJoltProvider &&) noexcept = default;
        UnifiedJoltProvider &operator=(UnifiedJoltProvider &&Other) noexcept {
            if (this != &Other) {
                Soft.reset();
                Rigid.reset();
                Rigid = std::move(Other.Rigid);
                Soft = std::move(Other.Soft);
            }
            return *this;
        }
        // Destruction is Soft then Rigid by reverse member order. Directly
        // resetting Rigid while Soft is live violates this private contract.
    };

    [[nodiscard]] UnifiedJoltProvider CreateUnifiedJoltProvider(
        const gargantuan::PhysicsWorldConfig &Config,
        gargantuan::SoftBodyWorldLimits Limits = {}, int WorkerBudget = 1);

    // Private experiment metadata; the implementation returns the profile
    // compiled into the provider, so callers cannot mislabel a stock build.
    struct JoltSoftSolverProfile {
        std::uint32_t WorldUpdates = 2;
        std::uint32_t CollisionStepsPerUpdate = 8;
        std::uint32_t SoftIterationsPerCollisionStep = 12;
        std::uint32_t ConstraintSweepsPerIteration = 1;
        std::uint32_t SoftIntegrationsPerFrame = 192;
        bool FullUpdateSleepClock = false;
    };
    [[nodiscard]] JoltSoftSolverProfile GetJoltSoftSolverProfile();

#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
    // Private, thread-local measurements for the dedicated admission executable.
    // The normal timing and allocation targets compile without these probes.
    struct JoltAdmissionPhase {
        double Milliseconds = 0;
        std::uint64_t AllocationCalls = 0;
        std::uint64_t AllocatedBytes = 0;
    };
    struct JoltAdmissionProfile {
        JoltAdmissionPhase Topology;
        JoltAdmissionPhase Precompute;
        JoltAdmissionPhase Optimize;
        JoltAdmissionPhase NativeCreate;
        JoltAdmissionPhase SlotCommit;
    };
    void ResetJoltAdmissionProfile();
    [[nodiscard]] JoltAdmissionProfile GetJoltAdmissionProfile();
#endif

    // Cumulative vertex contacts reported by Jolt's soft-rigid manifold for
    // this live neutral-ID pair. Enable capture before Step in private fixtures;
    // default provider timing keeps the native listener unset.
    [[nodiscard]] bool SetJoltSoftRigidContactCapture(
        gargantuan::IPhysicsBackend &Rigid, bool Enabled);
    [[nodiscard]] std::uint64_t GetJoltSoftRigidContactVertexCount(
        const gargantuan::IPhysicsBackend &Rigid,
        const gargantuan::ISoftBodyBackend &Soft,
        gargantuan::SoftBodyId SoftId, gargantuan::PhysicsBodyId RigidId);

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

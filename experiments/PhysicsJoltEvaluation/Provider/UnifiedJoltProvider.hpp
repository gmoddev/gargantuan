#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <memory>

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
}

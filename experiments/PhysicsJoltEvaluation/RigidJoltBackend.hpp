#pragma once

#include "gargantuan/physics/PhysicsBackend.hpp"

#include <memory>

namespace PhysicsEvaluation {
    // Experimental rigid provider. WorkerBudget includes the calling thread.
    [[nodiscard]] std::unique_ptr<gargantuan::IPhysicsBackend> CreateRigidJoltBackend(
        const gargantuan::PhysicsWorldConfig &Config, int WorkerBudget);
}

#pragma once

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace PhysicsJoltEvaluation {
    struct FeedbackImpulse {
        std::size_t ColliderIndex = 0;
        glm::vec3 Impulse{0.0f};
        glm::vec3 AngularImpulseOrigin{0.0f};
        glm::vec3 Position{0.0f};
        std::size_t Contacts = 0;
    };

    struct AcceptedSoftBodyDynamics {
        std::array<double, 3> Momentum{};
        double TranslationalKineticEnergy = 0.0;
        double MovableMass = 0.0;
        std::size_t MovableVertices = 0;
        bool Finite = true;
    };

    // Private experiment seam. The production soft-body interface is unchanged.
    class IFeedbackSoftBodyBackend : public gargantuan::ISoftBodyBackend {
      public:
        [[nodiscard]] virtual std::vector<FeedbackImpulse> TakeFeedback() = 0;
        [[nodiscard]] virtual std::optional<AcceptedSoftBodyDynamics> ReadAcceptedDynamics(
            gargantuan::SoftBodyId Body, std::uint64_t ExpectedVertexRevision) const = 0;
    };

    [[nodiscard]] std::unique_ptr<IFeedbackSoftBodyBackend>
    CreateFeedbackSoftBodyBackend(gargantuan::SoftBodyWorldLimits Limits = {});
}

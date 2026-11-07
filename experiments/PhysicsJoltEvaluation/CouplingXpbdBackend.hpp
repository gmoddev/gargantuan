#pragma once

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <memory>
#include <vector>

namespace PhysicsJoltEvaluation {
    struct FeedbackImpulse {
        std::size_t ColliderIndex = 0;
        glm::vec3 Impulse{0.0f};
        glm::vec3 AngularImpulseOrigin{0.0f};
        glm::vec3 Position{0.0f};
        std::size_t Contacts = 0;
    };

    // Private experiment seam. The production soft-body interface is unchanged.
    class IFeedbackSoftBodyBackend : public gargantuan::ISoftBodyBackend {
      public:
        [[nodiscard]] virtual std::vector<FeedbackImpulse> TakeFeedback() = 0;
    };

    [[nodiscard]] std::unique_ptr<IFeedbackSoftBodyBackend>
    CreateFeedbackSoftBodyBackend(gargantuan::SoftBodyWorldLimits Limits = {});
}

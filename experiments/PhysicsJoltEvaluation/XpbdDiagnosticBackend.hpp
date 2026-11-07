#pragma once

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <string>

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

    struct DiagnosticParticle {
        glm::vec3 Position{0.0f};
        glm::vec3 Velocity{0.0f};
        float InverseMass = 0.0f;
    };

    struct DiagnosticPhase {
        std::string Name;
        int Iteration = -1;
        double TranslationalKineticEnergy = 0.0;
        std::array<double, 3> Momentum{};
        double PositionCorrectionSum = 0.0;
        double PositionCorrectionMax = 0.0;
        std::vector<DiagnosticParticle> Particles;
    };

    struct DiagnosticContact {
        int Iteration = 0;
        std::size_t ParticleIndex = 0;
        std::size_t ColliderIndex = 0;
        glm::vec3 Normal{0.0f};
        glm::vec3 PositionBefore{0.0f};
        glm::vec3 PositionAfter{0.0f};
        glm::vec3 Reaction{0.0f};
        double PenetrationDepth = 0.0;
        bool FrictionApplied = false;
    };

    struct DiagnosticStep {
        std::uint64_t VertexRevision = 0;
        int Iterations = 0;
        std::vector<DiagnosticPhase> Phases;
        std::vector<DiagnosticContact> Contacts;
    };

    struct DiagnosticSettings {
        int Iterations = 8;
    };

    // Private experiment seam. The production soft-body interface is unchanged.
    class IFeedbackSoftBodyBackend : public gargantuan::ISoftBodyBackend {
      public:
        [[nodiscard]] virtual std::vector<FeedbackImpulse> TakeFeedback() = 0;
        [[nodiscard]] virtual std::optional<AcceptedSoftBodyDynamics> ReadAcceptedDynamics(
            gargantuan::SoftBodyId Body, std::uint64_t ExpectedVertexRevision) const = 0;
        [[nodiscard]] virtual std::optional<DiagnosticStep> ReadDiagnosticStep(
            gargantuan::SoftBodyId Body, std::uint64_t ExpectedVertexRevision) const = 0;
    };

    [[nodiscard]] std::unique_ptr<IFeedbackSoftBodyBackend>
    CreateDiagnosticSoftBodyBackend(gargantuan::SoftBodyWorldLimits Limits = {},
                                    DiagnosticSettings Settings = {});
}

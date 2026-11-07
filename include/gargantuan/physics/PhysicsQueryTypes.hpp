#pragma once

#include "gargantuan/physics/PhysicsTypes.hpp"

namespace gargantuan {
    inline constexpr std::size_t MaximumPhysicsOverlapResults = 256;
    inline constexpr float MaximumPhysicsQueryExtent = 10'000.0f;
    enum class PhysicsQueryShapeKind : std::uint8_t { Sphere, Capsule, Box };
    struct PhysicsQueryShape {
        PhysicsQueryShapeKind Kind = PhysicsQueryShapeKind::Sphere;
        CFrame Transform;
        glm::vec3 Size{1.0f};
        float Radius = 0.5f;
        float Height = 1.0f;
    };
    struct PhysicsShapeCastRequest {
        PhysicsQueryShape Shape;
        glm::vec3 Direction{0.0f};
        PhysicsQueryFilter Filter;
    };
    struct PhysicsOverlapRequest {
        PhysicsQueryShape Shape;
        PhysicsQueryFilter Filter;
    };
    struct PhysicsOverlapResult {
        PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
        std::string Message;
        std::vector<PhysicsBodyId> Bodies;
        bool Truncated = false;
        [[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
    };
}

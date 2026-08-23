// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/physics/PhysicsTypes.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	inline constexpr std::size_t MaximumSoftBodies = 64;
	inline constexpr std::size_t MaximumSoftBodyVertices = 65536;
	inline constexpr std::size_t MaximumSoftBodyWorldVertices = 131072;
	inline constexpr std::size_t MaximumSoftBodyConstraints = 1048576;
	inline constexpr std::size_t MaximumSoftBodyAttachments = 4096;
	inline constexpr std::size_t MaximumSoftBodyColliders = 4096;
	inline constexpr std::size_t MaximumSoftBodyStepsPerFrame = 4;
	inline constexpr float SoftBodyStepInterval = 1.0f / 60.0f;

	struct SoftBodyId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
		auto operator<=>(const SoftBodyId &) const = default;
	};

	enum class SoftBodyKind : std::uint8_t { Cloth, Rubber };
	enum class SoftBodyQuality : std::uint8_t { Automatic, Low, Medium, High };
	enum class SoftBodyCollisionMode : std::uint8_t { None, RigidPrimitives };

	struct SoftBodyMaterialDesc {
		float ParticleMass = 0.1f;
		float Damping = 0.04f;
		float StretchCompliance = 0.000001f;
		float BendCompliance = 0.0001f;
		float ShapeCompliance = 0.00001f;
		float Friction = 0.2f;
		float Thickness = 0.05f;
	};

	struct SoftBodyAttachmentDesc {
		std::uint32_t Vertex = 0;
		glm::vec3 Position{0.0f};
	};

	struct SoftBodyDefinition {
		SoftBodyKind Kind = SoftBodyKind::Cloth;
		glm::vec3 Position{0.0f};
		glm::vec3 Size{8.0f, 8.0f, 0.5f};
		std::uint32_t ResolutionX = 16;
		std::uint32_t ResolutionY = 16;
		std::uint32_t ResolutionZ = 4;
		SoftBodyMaterialDesc Material;
		SoftBodyQuality Quality = SoftBodyQuality::Automatic;
		SoftBodyCollisionMode CollisionMode = SoftBodyCollisionMode::RigidPrimitives;
		bool Enabled = true;
		std::vector<SoftBodyAttachmentDesc> Attachments;
	};

	struct SoftBodyCollider {
		PhysicsShapeDesc Shape;
		CFrame Transform;
	};

	struct SoftBodyState {
		SoftBodyId Body;
		std::uint64_t TopologyRevision = 0;
		std::uint64_t VertexRevision = 0;
		bool Simulated = false;
		std::shared_ptr<const std::vector<glm::vec3>> Positions;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
	};

	struct SoftBodyWorldLimits {
		std::size_t MaximumBodies = MaximumSoftBodies;
		std::size_t MaximumVerticesPerBody = MaximumSoftBodyVertices;
		std::size_t MaximumWorldVertices = MaximumSoftBodyWorldVertices;
		std::size_t MaximumConstraints = MaximumSoftBodyConstraints;
		std::size_t MaximumAttachments = MaximumSoftBodyAttachments;
		std::size_t MaximumColliders = MaximumSoftBodyColliders;
	};

	struct SoftBodyStepConfig {
		float DeltaTime = SoftBodyStepInterval;
		glm::vec3 Gravity{0.0f, -196.2f, 0.0f};
		std::vector<SoftBodyCollider> Colliders;
	};

	struct SoftBodyStepProfile {
		std::uint64_t IntegrationNanoseconds = 0;
		std::uint64_t ConstraintNanoseconds = 0;
		std::uint64_t CollisionNanoseconds = 0;
		std::uint64_t ExtractionNanoseconds = 0;
		std::size_t SimulatedBodies = 0;
		std::size_t SimulatedVertices = 0;
		std::size_t ConstraintCount = 0;
		std::size_t EstimatedBytes = 0;
	};

	struct SoftBodyStepResult {
		std::vector<SoftBodyState> States;
		SoftBodyStepProfile Profile;
		bool BodiesTruncated = false;
		bool VerticesTruncated = false;
		bool CollidersTruncated = false;
	};
}

template <> struct std::hash<gargantuan::SoftBodyId> {
	std::size_t operator()(const gargantuan::SoftBodyId &Id) const noexcept {
		const auto Combined = (static_cast<std::uint64_t>(Id.Generation) << 32) | Id.Slot;
		return std::hash<std::uint64_t>{}(Combined);
	}
};

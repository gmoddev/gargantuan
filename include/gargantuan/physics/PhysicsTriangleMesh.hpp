// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <glm/vec3.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	// Immutable canonical triangles. A live backend shape retains its source buffers.
	struct PhysicsTriangleMesh {
		std::string ResourceId;
		std::uint64_t ContentRevision = 0;
		std::shared_ptr<const std::vector<glm::vec3>> Positions;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
	};
}

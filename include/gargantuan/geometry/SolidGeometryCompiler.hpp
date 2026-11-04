// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/assets/AssetTypes.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

namespace gargantuan {
	enum class SolidNodeKind : std::uint8_t { Box, Sphere, ImportedMesh, Transform, Union, Intersection, Subtraction };

	struct SolidNode {
		std::string Id;
		SolidNodeKind Kind = SolidNodeKind::Box;
		std::vector<std::string> Inputs;
		glm::vec3 Size{1.0f};
		float Radius = 0.5f;
		glm::mat4 Matrix{1.0f};
		std::shared_ptr<const ImportedMesh> Mesh;
	};

	struct SolidGraph {
		std::uint32_t Version = 1;
		std::string Root;
		std::vector<SolidNode> Nodes;
	};

	struct SolidCompilation {
		ImportedMesh Mesh;
		ImportedCollision Collision;
	};

	struct SolidCompileLimits {
		std::size_t MaximumNodes = 128;
		std::size_t MaximumDepth = 24;
		std::size_t MaximumInputTriangles = 8192;
		std::size_t MaximumIntermediateTriangles = 32768;
		std::size_t MaximumOutputTriangles = 32768;
		std::size_t MaximumBooleanSteps = 32;
	};

	// Compile on a worker. Results are candidates only; AssetService owns publication.
	// Cancellation is checked between graph nodes and at Manifold phase boundaries.
	// A single Boolean phase is not preemptible and peak kernel allocations are not capped.
	[[nodiscard]] std::expected<SolidCompilation, AssetDiagnostic> CompileSolidGeometry(
		const SolidGraph &Graph,
		const AssetCancellationToken &Cancellation = {},
		std::chrono::steady_clock::time_point Deadline = std::chrono::steady_clock::time_point::max(),
		const SolidCompileLimits &Limits = {}
	);
}

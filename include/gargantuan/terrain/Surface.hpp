// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Types.hpp"

#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace gargantuan::terrain {
	struct SurfacePoint {
		float X{}, Y{}, Z{};
		bool operator==(const SurfacePoint &) const = default;
	};

	struct SurfaceVertex {
		SurfacePoint Position;
		SurfacePoint Normal;
		MaterialId Material{};
		// Index into the halo sample span. Geometry can survive a material-only revision.
		std::uint32_t MaterialSample{};
		// For a lattice-edge crossing, the local integer start along the varying axis.
		std::uint8_t EdgeStart{};
		std::uint8_t EdgeAxis{3};
		bool operator==(const SurfaceVertex &) const = default;
	};

	struct SurfaceInput {
		// X-fast order over [-1, Cells + 1] in each axis, including a central-difference halo.
		std::span<const Sample> Samples;
		std::uint32_t CellsX{}, CellsY{}, CellsZ{};
		double Spacing{1.0};
	};

	struct SurfaceMesh {
		std::vector<SurfaceVertex> Vertices;
		std::vector<std::uint32_t> Indices;
		double Spacing{1.0};
	};

	struct SurfaceLimits {
		std::uint32_t MaxCells{32 * 32 * 32};
		std::uint32_t MaxVertices{262144};
		std::uint32_t MaxIndices{32 * 32 * 32 * 12 * 3};
	};

	enum class SurfaceStatus : std::uint8_t {
		Success,
		InvalidInput,
		WorkLimit,
		Canceled,
		AllocationFailure,
		Degenerate,
	};

	// Owns cells with minima in [0, Cells). All outputs are in chunk-local world units.
	// Success replaces Output, including with an empty mesh. Failure preserves Output.
	[[nodiscard]] SurfaceStatus ExtractSurface(const SurfaceInput &Input, const SurfaceLimits &Limits,
		SurfaceMesh &Output, std::stop_token Stop = {});

	// The caller supplies a paint-only snapshot with the same halo extent and geometry revision.
	// Invalid material sample references leave the existing bindings unchanged.
	[[nodiscard]] SurfaceStatus RefreshSurfaceMaterials(std::span<const Sample> Samples, SurfaceMesh &Mesh);

	struct CollisionSurfacePolicy {
		// Every chunk must use the same inset at one authored spacing to preserve shared edges.
		float MinimumEdgeFraction{1.0f / 64.0f};
		float MinimumAreaSquared{1.0e-10f};
	};

	struct CollisionSurfaceMesh {
		std::vector<SurfacePoint> Positions;
		std::vector<std::uint32_t> Indices;
		std::vector<MaterialId> TriangleMaterials;
		float MaximumDisplacement{};
	};

	// Repositions lattice-edge crossings inside their source edges with a bounded displacement.
	// Keeps all faces and shared boundary edges. Reversals or provider-sized slivers fail atomically.
	[[nodiscard]] SurfaceStatus BuildCollisionSurface(const SurfaceMesh &Source,
		const CollisionSurfacePolicy &Policy, CollisionSurfaceMesh &Output, std::stop_token Stop = {});
}

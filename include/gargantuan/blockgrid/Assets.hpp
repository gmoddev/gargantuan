// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/blockgrid/Geometry.hpp"
#include "gargantuan/physics/PhysicsTriangleMesh.hpp"

namespace gargantuan {
	class AssetService;
	namespace blockgrid {
		inline constexpr std::size_t MaximumAssetPins=64, MaximumMeshVertices=2'048,
			MaximumMeshTriangles=1'024, MaximumMeshPrimitives=32, MaximumCatalogPins=1'024;
		inline constexpr std::size_t MaximumCatalogAssetBytes=32*1024*1024;
		// Vertices are centered cell-local coordinates in [-0.5,0.5]. Indices, UVs, normals,
		// tangents and contiguous imported primitive ranges belong to the pinned mesh revision.
		struct ResolvedBlockAssets {
			std::optional<ImportedMesh> VisualMesh;
			std::optional<RenderMaterialState> Material;
			std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
			std::shared_ptr<const PhysicsTriangleMesh> QueryMesh;
			bool FullyOpaque{true};
			std::size_t RetainedBytes{};
		};
		// Empty Pins requests trusted current project revisions. Nonempty Pins must match the entire closure.
		[[nodiscard]] std::shared_ptr<const AssetBinding> ResolveBlockAssets(AssetService& Service,const AssetBinding& Requested);
	}
}

// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderEnvironment.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace gargantuan {
	using RenderPublicationId = std::uint64_t;
	inline constexpr RenderPublicationId InvalidRenderPublicationId = 0;

	struct RenderFrameState {
		std::uint32_t ViewportWidth = 0;
		std::uint32_t ViewportHeight = 0;
		float DpiScale = 1.0f;
		RenderCameraSnapshot Camera;
		RenderEnvironmentState Environment;
	};

	enum class RenderUpdateDomain : std::uint32_t {
		None = 0,
		Transform = 1u << 0,
		Material = 1u << 1,
		Visibility = 1u << 2,
		Geometry = 1u << 3,
		DeformableVertices = 1u << 4,
		Hierarchy = 1u << 5,
		Environment = 1u << 6,
		AnimationPose = 1u << 7,
	};

	[[nodiscard]] constexpr RenderUpdateDomain operator|(RenderUpdateDomain Left, RenderUpdateDomain Right) {
		return static_cast<RenderUpdateDomain>(static_cast<std::uint32_t>(Left) | static_cast<std::uint32_t>(Right));
	}

	[[nodiscard]] constexpr bool HasRenderUpdateDomain(RenderUpdateDomain Value, RenderUpdateDomain Domain) {
		return (static_cast<std::uint32_t>(Value) & static_cast<std::uint32_t>(Domain)) != 0;
	}

	struct RenderMeshIdentity {
		std::uint64_t Slot = 0;
		std::uint32_t Generation = 0;
		auto operator<=>(const RenderMeshIdentity &) const = default;
		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
	};

	enum class RenderTextureFormat : std::uint8_t { Rgba8Unorm };

	struct RenderTextureCreate {
		RenderTextureIdentity Texture;
		std::uint64_t Revision = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		RenderTextureFormat Format = RenderTextureFormat::Rgba8Unorm;
		std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
	};

	struct RenderTextureUpdate {
		RenderTextureIdentity Texture;
		std::uint64_t Revision = 0;
		std::uint32_t X = 0;
		std::uint32_t Y = 0;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
	};

	struct RenderTextureRemove { RenderTextureIdentity Texture; };

	enum class RenderOpacityMode : std::uint8_t { Opaque, Masked, Transparent };

	struct RenderMaterialState {
		std::uint64_t Revision = 1;
		glm::vec4 BaseColorFactor{1.0f};
		std::optional<RenderTextureIdentity> BaseColorTexture;
		std::optional<RenderTextureIdentity> NormalTexture;
		float Metallic = 0.0f;
		float Roughness = 1.0f;
		RenderOpacityMode OpacityMode = RenderOpacityMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
	};

	struct RenderPrimitiveMaterialState {
		std::uint32_t FirstIndex = 0;
		std::uint32_t IndexCount = 0;
		RenderMaterialState Material;
	};

	struct RenderObjectCreate {
		RenderItem Item;
		std::optional<RenderMeshIdentity> Mesh;
		RenderMaterialState Material;
		bool Visible = true;
		std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
	};
	struct RenderObjectUpdate {
		ObjectId Object;
		RenderUpdateDomain Domains = RenderUpdateDomain::None;
		RenderItem Item;
		std::optional<RenderMeshIdentity> Mesh;
		RenderMaterialState Material;
		bool Visible = true;
		std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
	};
	struct RenderObjectRemove { ObjectId Object; };

	struct RenderMeshIdentityHash {
		std::size_t operator()(const RenderMeshIdentity &Value) const noexcept {
			return std::hash<std::uint64_t>{}(Value.Slot) ^ (std::hash<std::uint32_t>{}(Value.Generation) << 1);
		}
	};

	struct RenderVertex {
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f, 1.0f, 0.0f};
		glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		glm::vec2 TextureCoordinate{0.0f};
	};

	struct RenderBounds {
		glm::vec3 Minimum{0.0f};
		glm::vec3 Maximum{0.0f};
	};

	struct RenderSkinInfluence {
		std::array<std::uint16_t, 4> Joints{};
		glm::vec4 Weights{0.0f};
	};

	struct RenderSkeletonIdentity {
		std::array<std::uint8_t, 32> Bytes{};
		auto operator<=>(const RenderSkeletonIdentity &) const = default;
		[[nodiscard]] bool IsValid() const {
			return std::ranges::any_of(Bytes, [](std::uint8_t Value) { return Value != 0; });
		}
	};

	struct alignas(16) RenderSkinPaletteEntry {
		glm::mat4 PositionMatrix{1.0f};
		glm::mat4 NormalMatrix{1.0f};
	};
	static_assert(sizeof(RenderSkinPaletteEntry) == 128,
		"The renderer-neutral skin palette must remain two tightly packed mat4 values");
	inline constexpr std::uint32_t MaximumRenderSkinPaletteEntries = 256;

	struct RenderSkinPalette {
		RenderSkeletonIdentity Skeleton;
		std::shared_ptr<const std::vector<RenderSkinPaletteEntry>> Entries;
	};

	enum class RenderAnimationSkinningMode : std::uint8_t {
		GpuPalette,
		CpuFallback,
	};

	struct RenderMeshCreate {
		RenderMeshIdentity Mesh;
		std::uint64_t TopologyRevision = 0;
		std::uint64_t VertexRevision = 0;
		std::shared_ptr<const std::vector<RenderVertex>> Vertices;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
		RenderBounds Bounds;
		std::shared_ptr<const std::vector<RenderSkinInfluence>> SkinInfluences;
		RenderSkeletonIdentity Skeleton;
		std::uint32_t SkeletonJointCount = 0;
	};

	struct RenderAnimationPoseUpdate {
		ObjectId Object;
		RenderMeshIdentity SourceMesh;
		RenderMeshIdentity PosedMesh;
		std::uint64_t PoseRevision = 0;
		RenderSkinPalette Palette;
		RenderAnimationSkinningMode Mode = RenderAnimationSkinningMode::GpuPalette;
	};

	struct RenderAnimationPoseRemove { ObjectId Object; };

	// Complete renderer-neutral state handed from animation evaluation to the
	// publisher. The publication separates the semantic palette update from the
	// optional CPU-skinned mesh fallback. GPU-capable backends consume only the
	// static source mesh and final position/normal palette.
	struct RenderAnimationPoseState {
		RenderAnimationPoseUpdate Pose;
		std::uint64_t TopologyRevision = 0;
		std::shared_ptr<const std::vector<RenderVertex>> Vertices;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
		RenderBounds Bounds;
	};

	struct RenderMeshVertexUpdate {
		RenderMeshIdentity Mesh;
		std::uint64_t VertexRevision = 0;
		std::uint32_t FirstVertex = 0;
		std::shared_ptr<const std::vector<RenderVertex>> Vertices;
		RenderBounds Bounds;
	};

	struct RenderMeshRemove { RenderMeshIdentity Mesh; };

	struct RenderUiClipRect {
		float X = 0.0f;
		float Y = 0.0f;
		float Width = 0.0f;
		float Height = 0.0f;
	};

	struct RenderUiVertex {
		glm::vec2 Position{0.0f};
		glm::vec2 TextureCoordinate{0.0f};
		glm::vec4 Color{1.0f};
	};

	struct RenderUiBatch {
		std::optional<RenderTextureIdentity> Texture;
		std::optional<RenderUiClipRect> Clip;
		std::int32_t Layer = 0;
		float Opacity = 1.0f;
		std::vector<RenderUiVertex> Vertices;
		std::vector<std::uint32_t> Indices;
	};

	struct RenderUiFrame {
		std::uint32_t ViewportWidth = 0;
		std::uint32_t ViewportHeight = 0;
		float DpiScale = 1.0f;
		std::vector<RenderUiBatch> Batches;
	};

	struct RenderPublication {
		RenderPublicationId Id = InvalidRenderPublicationId;
		RenderPublicationId BaseId = InvalidRenderPublicationId;
		bool FullResync = false;
		RenderFrameState Frame;
		bool EnvironmentChanged = false;
		std::vector<RenderObjectCreate> Creates;
		std::vector<RenderObjectUpdate> Updates;
		std::vector<RenderObjectRemove> Removes;
		std::vector<RenderMeshCreate> MeshCreates;
		std::vector<RenderMeshVertexUpdate> MeshVertexUpdates;
		std::vector<RenderMeshRemove> MeshRemoves;
		std::vector<RenderAnimationPoseUpdate> AnimationPoseUpdates;
		std::vector<RenderAnimationPoseRemove> AnimationPoseRemoves;
		std::vector<RenderTextureCreate> TextureCreates;
		std::vector<RenderTextureUpdate> TextureUpdates;
		std::vector<RenderTextureRemove> TextureRemoves;
		// A UI frame is complete state when present. Incremental publications leave
		// the projection's committed UI untouched unless this bit is set.
		bool UiChanged = false;
		RenderUiFrame Ui;
		// Foundation 2 can publish the same immutable complete frame through the
		// runtime, publisher, projection, and renderer without copying its geometry.
		// Ui remains the value-form compatibility surface for direct producers.
		std::shared_ptr<const RenderUiFrame> SharedUi;
		[[nodiscard]] const RenderUiFrame &GetUi() const { return SharedUi ? *SharedUi : Ui; }
		std::vector<RenderExtractionDiagnostic> Diagnostics;
	};

	using RenderPublicationPtr = std::shared_ptr<const RenderPublication>;
}

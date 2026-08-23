// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"

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
		glm::vec3 LightDirection{0.0f, 1.0f, 0.0f};
	};

	enum class RenderUpdateDomain : std::uint32_t {
		None = 0,
		Transform = 1u << 0,
		Material = 1u << 1,
		Visibility = 1u << 2,
		Geometry = 1u << 3,
		DeformableVertices = 1u << 4,
		Hierarchy = 1u << 5,
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

	struct RenderTextureIdentity {
		std::uint64_t Slot = 0;
		std::uint32_t Generation = 0;
		auto operator<=>(const RenderTextureIdentity &) const = default;
		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
	};

	struct RenderTextureIdentityHash {
		std::size_t operator()(const RenderTextureIdentity &Value) const noexcept {
			return std::hash<std::uint64_t>{}(Value.Slot) ^ (std::hash<std::uint32_t>{}(Value.Generation) << 1);
		}
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
	};

	struct RenderObjectCreate {
		RenderItem Item;
		std::optional<RenderMeshIdentity> Mesh;
		RenderMaterialState Material;
		bool Visible = true;
	};
	struct RenderObjectUpdate {
		ObjectId Object;
		RenderUpdateDomain Domains = RenderUpdateDomain::None;
		RenderItem Item;
		std::optional<RenderMeshIdentity> Mesh;
		RenderMaterialState Material;
		bool Visible = true;
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

	struct RenderMeshCreate {
		RenderMeshIdentity Mesh;
		std::uint64_t TopologyRevision = 0;
		std::uint64_t VertexRevision = 0;
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
		std::vector<RenderObjectCreate> Creates;
		std::vector<RenderObjectUpdate> Updates;
		std::vector<RenderObjectRemove> Removes;
		std::vector<RenderMeshCreate> MeshCreates;
		std::vector<RenderMeshVertexUpdate> MeshVertexUpdates;
		std::vector<RenderMeshRemove> MeshRemoves;
		std::vector<RenderTextureCreate> TextureCreates;
		std::vector<RenderTextureUpdate> TextureUpdates;
		std::vector<RenderTextureRemove> TextureRemoves;
		RenderUiFrame Ui;
		std::vector<RenderExtractionDiagnostic> Diagnostics;
	};

	using RenderPublicationPtr = std::shared_ptr<const RenderPublication>;
}

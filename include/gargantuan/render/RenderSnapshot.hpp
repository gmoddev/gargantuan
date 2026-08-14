// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace gargantuan {
	using RenderSnapshotId = std::uint64_t;
	inline constexpr RenderSnapshotId InvalidRenderSnapshotId = 0;

	enum class RenderGeometry : std::uint8_t {
		Block,
		Ball,
		Cylinder,
		Wedge,
		CornerWedge,
	};

	struct RenderCameraSnapshot {
		glm::vec3 Position{0.0f};
		glm::vec3 RightDirection{1.0f, 0.0f, 0.0f};
		glm::vec3 UpDirection{0.0f, 1.0f, 0.0f};
		glm::vec3 LookDirection{0.0f, 0.0f, -1.0f};
		glm::mat4 ViewMatrix{1.0f};
		glm::mat4 ProjectionMatrix{1.0f};
		glm::mat4 ViewProjectionMatrix{1.0f};
		float VerticalFieldOfView = 70.0f;
		float NearPlane = 0.1f;
		float FarPlane = 100000.0f;
	};

	struct RenderItem {
		ObjectId Object{};
		RenderGeometry Geometry = RenderGeometry::Block;
		glm::mat4 ModelMatrix{1.0f};
		glm::mat4 InverseModelMatrix{1.0f};
		glm::vec4 Color{1.0f};
		bool CastShadow = true;
	};

	enum class RenderExtractionIssue : std::uint8_t {
		DeadObject,
		StaleObjectId,
		UnsupportedGeometry,
		InvalidTransform,
		InvalidVisualState,
	};

	struct RenderExtractionDiagnostic {
		RenderExtractionIssue Issue = RenderExtractionIssue::DeadObject;
		ObjectId Object{};
		std::string Message;
	};

	struct RenderSnapshot {
		RenderSnapshotId Id = InvalidRenderSnapshotId;
		std::uint32_t ViewportWidth = 0;
		std::uint32_t ViewportHeight = 0;
		RenderCameraSnapshot Camera;
		glm::vec3 LightDirection{0.0f, 1.0f, 0.0f};
		std::vector<RenderItem> Items;
		std::vector<RenderExtractionDiagnostic> Diagnostics;
	};

	using RenderSnapshotPtr = std::shared_ptr<const RenderSnapshot>;
}

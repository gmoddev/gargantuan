// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"

#include <cstdint>

#include <glm/glm.hpp>

namespace gargantuan {
	class Camera;
	class WorldRoot;

	struct RenderCameraInput {
		glm::vec3 Position{0.0f};
		glm::vec3 LookDirection{0.0f, 0.0f, -1.0f};
		glm::vec3 UpDirection{0.0f, 1.0f, 0.0f};
		float VerticalFieldOfView = 70.0f;
		float NearPlane = 0.1f;
		float FarPlane = 100000.0f;
	};

	[[nodiscard]] RenderCameraInput MakeRenderCameraInput(const Camera &camera);
	[[nodiscard]] RenderCameraInput MakeLookAtRenderCameraInput(
		glm::vec3 position,
		glm::vec3 target,
		glm::vec3 up,
		float verticalFieldOfView = 70.0f
	);

	class RenderExtractor {
	  public:
		[[nodiscard]] RenderSnapshotPtr Extract(
			const WorldRoot &world,
			const RenderCameraInput &camera,
			std::uint32_t viewportWidth,
			std::uint32_t viewportHeight,
			glm::vec3 lightDirection = {0.75f, 1.0f, 0.5f}
		);

		[[nodiscard]] RenderSnapshotId GetLastSnapshotId() const { return LastSnapshotId; }

	  private:
		RenderSnapshotId LastSnapshotId = InvalidRenderSnapshotId;
	};
}

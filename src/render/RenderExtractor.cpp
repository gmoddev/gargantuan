// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderExtractor.hpp"

#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/environment/EnvironmentSemantics.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/services/Lighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/vector_relational.hpp>

namespace gargantuan {
	namespace {
		bool IsFinite(const glm::vec3 &value) {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(const glm::vec4 &value) {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
				std::isfinite(value.w);
		}

		bool IsFinite(const glm::mat4 &value) {
			for (glm::length_t column = 0; column < 4; ++column) {
				if (!IsFinite(value[column])) return false;
			}
			return true;
		}

		std::optional<RenderGeometry> GetRenderGeometry(Enums::PartType shape) {
			switch (shape) {
				case Enums::PartType::Block: return RenderGeometry::Block;
				case Enums::PartType::Ball: return RenderGeometry::Ball;
				case Enums::PartType::Cylinder: return RenderGeometry::Cylinder;
				case Enums::PartType::Wedge: return RenderGeometry::Wedge;
				case Enums::PartType::CornerWedge: return RenderGeometry::CornerWedge;
			}
			return std::nullopt;
		}

		RenderCameraSnapshot BuildCameraSnapshot(
			const RenderCameraInput &input,
			std::uint32_t viewportWidth,
			std::uint32_t viewportHeight
		) {
			if (viewportWidth == 0 || viewportHeight == 0)
				throw std::invalid_argument("RenderSnapshot viewport dimensions must be nonzero");
			if (!IsFinite(input.Position) || !IsFinite(input.LookDirection) || !IsFinite(input.UpDirection))
				throw std::invalid_argument("RenderSnapshot camera vectors must be finite");
			if (!std::isfinite(input.VerticalFieldOfView) || input.VerticalFieldOfView <= 0.0f ||
				input.VerticalFieldOfView >= 180.0f)
				throw std::invalid_argument("RenderSnapshot camera field of view must be between 0 and 180 degrees");
			if (!std::isfinite(input.NearPlane) || !std::isfinite(input.FarPlane) || input.NearPlane <= 0.0f ||
				input.FarPlane <= input.NearPlane)
				throw std::invalid_argument("RenderSnapshot camera clipping planes are invalid");

			const float lookLength = glm::length(input.LookDirection);
			const float upLength = glm::length(input.UpDirection);
			if (lookLength < 1e-6f || upLength < 1e-6f)
				throw std::invalid_argument("RenderSnapshot camera directions must be nonzero");
			const auto look = input.LookDirection / lookLength;
			const auto inputUp = input.UpDirection / upLength;
			const auto rightCandidate = glm::cross(look, inputUp);
			if (glm::length(rightCandidate) < 1e-6f)
				throw std::invalid_argument("RenderSnapshot camera look and up directions are collinear");
			const auto right = glm::normalize(rightCandidate);
			const auto up = glm::normalize(glm::cross(right, look));
			const auto aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);

			RenderCameraSnapshot camera;
			camera.Position = input.Position;
			camera.RightDirection = right;
			camera.UpDirection = up;
			camera.LookDirection = look;
			camera.VerticalFieldOfView = input.VerticalFieldOfView;
			camera.NearPlane = input.NearPlane;
			camera.FarPlane = input.FarPlane;
			camera.ViewMatrix = glm::lookAt(input.Position, input.Position + look, up);
			camera.ProjectionMatrix = glm::perspective(
				glm::radians(input.VerticalFieldOfView), aspect, input.NearPlane, input.FarPlane
			);
			camera.ViewProjectionMatrix = camera.ProjectionMatrix * camera.ViewMatrix;
			if (!IsFinite(camera.ViewMatrix) || !IsFinite(camera.ProjectionMatrix) ||
				!IsFinite(camera.ViewProjectionMatrix))
				throw std::invalid_argument("RenderSnapshot camera matrices are not finite");
			return camera;
		}

		RenderEnvironmentState BuildBasicEnvironment(const WorldRoot &World) {
			RenderEnvironmentState Result;
			auto DataModelValue = World.GetDataModel();
			if (!DataModelValue) return Result;
			auto Service = DataModelValue->FindService("Lighting");
			auto LightingValue = Service ? std::dynamic_pointer_cast<Lighting>(*Service) : nullptr;
			if (!LightingValue) return Result;

			const auto Sun = ComputeEnvironmentSunState(
				LightingValue->GetClockTime(), LightingValue->GetBrightness(), LightingValue->GetSunColor()
			);
			Result.AmbientColor = static_cast<glm::vec3>(LightingValue->GetAmbient());
			Result.SunDirection = Sun.Direction;
			Result.SunColor = Sun.Color;
			Result.SunIntensity = Sun.Intensity;
			Result.ExposureMultiplier = ComputeEnvironmentExposure(LightingValue->GetExposureCompensation());
			Result.EnvironmentColor = static_cast<glm::vec3>(LightingValue->GetEnvironmentColor());
			Result.Fog = {
				.Enabled = LightingValue->GetFogEnabled(),
				.Color = static_cast<glm::vec3>(LightingValue->GetFogColor()),
				.Start = LightingValue->GetFogStart(),
				.End = LightingValue->GetFogEnd(),
			};
			return Result;
		}
	}

	RenderCameraInput MakeRenderCameraInput(const Camera &camera) {
		const auto frame = camera.GetCFrame();
		return {
			.Position = frame.Position,
			.LookDirection = frame.GetLookVector(),
			.UpDirection = frame.GetUpVector(),
			.VerticalFieldOfView = camera.GetFieldOfView(),
		};
	}

	RenderCameraInput MakeLookAtRenderCameraInput(
		glm::vec3 position,
		glm::vec3 target,
		glm::vec3 up,
		float verticalFieldOfView
	) {
		if (!IsFinite(position) || !IsFinite(target) || !IsFinite(up) || glm::length(target - position) < 1e-6f)
			throw std::invalid_argument("Render camera look-at vectors are invalid");
		const auto look = glm::normalize(target - position);
		if (glm::length(up) < 1e-6f) throw std::invalid_argument("Render camera up direction is invalid");
		auto safeUp = glm::normalize(up);
		if (glm::length(glm::cross(look, safeUp)) < 1e-6f)
			safeUp = std::abs(look.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		return {
			.Position = position,
			.LookDirection = look,
			.UpDirection = safeUp,
			.VerticalFieldOfView = verticalFieldOfView,
		};
	}

	RenderSnapshotPtr RenderExtractor::Extract(
		const WorldRoot &world,
		const RenderCameraInput &cameraInput,
		std::uint32_t viewportWidth,
		std::uint32_t viewportHeight
	) {
		AssertAuthoritativeMutation("RenderSnapshot extraction");
		if (world.GetDestroyed() || world.IsDestroying())
			throw std::invalid_argument("Cannot extract a RenderSnapshot from a dead WorldRoot");
		if (LastSnapshotId == std::numeric_limits<RenderSnapshotId>::max())
			throw std::overflow_error("RenderSnapshot identity exhausted and will not roll over");

		auto candidate = std::make_shared<RenderSnapshot>();
		candidate->Id = LastSnapshotId + 1;
		candidate->ViewportWidth = viewportWidth;
		candidate->ViewportHeight = viewportHeight;
		candidate->Camera = BuildCameraSnapshot(cameraInput, viewportWidth, viewportHeight);
		candidate->Environment = BuildBasicEnvironment(world);
		candidate->Items.reserve(world.Parts.size());

		for (const auto &basePart : world.Parts) {
			if (!basePart || basePart->GetDestroyed() || basePart->IsDestroying()) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::DeadObject,
					.Message = "Skipped a dead renderable object",
				});
				continue;
			}

			const auto objectId = basePart->GetObjectId();
			if (!objectId.IsValid() || ObjectRegistry::Get().Lookup(objectId).get() != basePart.get()) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::StaleObjectId,
					.Object = objectId,
					.Message = "Skipped a renderable object with stale identity",
				});
				continue;
			}

			// RenderPublisher owns the asset-aware MeshPart projection. The legacy
			// compatibility snapshot has no renderer-resource identity surface.
			if (std::dynamic_pointer_cast<MeshPart>(basePart)) continue;
			auto part = std::dynamic_pointer_cast<Part>(basePart);
			if (!part) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::UnsupportedGeometry,
					.Object = objectId,
					.Message = "Skipped a BasePart without a supported primitive geometry descriptor",
				});
				continue;
			}
			const auto geometry = GetRenderGeometry(part->GetShape());
			if (!geometry) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::UnsupportedGeometry,
					.Object = objectId,
					.Message = "Skipped a Part with an unsupported primitive shape",
				});
				continue;
			}

			const auto frame = part->GetCFrame();
			const auto size = part->GetSize();
			if (!IsFinite(frame.Position) || !IsFinite(glm::vec4(frame.Rotation[0], 0.0f)) ||
				!IsFinite(glm::vec4(frame.Rotation[1], 0.0f)) || !IsFinite(glm::vec4(frame.Rotation[2], 0.0f)) ||
				!IsFinite(size) || std::abs(size.x) < 1e-6f || std::abs(size.y) < 1e-6f ||
				std::abs(size.z) < 1e-6f) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::InvalidTransform,
					.Object = objectId,
					.Message = "Skipped a primitive with a non-finite or singular transform",
				});
				continue;
			}

			const auto colorValue = static_cast<glm::vec3>(part->GetColor());
			const auto transparency = part->GetTransparency();
			const glm::vec4 color(colorValue, 1.0f - transparency);
			if (!IsFinite(color)) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::InvalidVisualState,
					.Object = objectId,
					.Message = "Skipped a primitive with non-finite color or transparency",
				});
				continue;
			}

			const auto model = glm::translate(glm::mat4(1.0f), frame.Position) * glm::mat4(frame.Rotation) *
				glm::scale(glm::mat4(1.0f), size);
			const auto inverseModel = glm::inverse(model);
			if (!IsFinite(model) || !IsFinite(inverseModel)) {
				candidate->Diagnostics.push_back({
					.Issue = RenderExtractionIssue::InvalidTransform,
					.Object = objectId,
					.Message = "Skipped a primitive whose render transform could not be inverted",
				});
				continue;
			}

			candidate->Items.push_back({
				.Object = objectId,
				.Geometry = *geometry,
				.ModelMatrix = model,
				.InverseModelMatrix = inverseModel,
				.Color = color,
				.CastShadow = part->GetCastShadow(),
			});
		}

		std::ranges::sort(candidate->Items, {}, &RenderItem::Object);
		LastSnapshotId = candidate->Id;
		return std::shared_ptr<const RenderSnapshot>(std::move(candidate));
	}
}

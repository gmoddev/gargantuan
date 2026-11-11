// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <limits>
#include <optional>

namespace gargantuan {
	inline constexpr std::uint32_t MaximumDirectionalCascades = 4;

	struct SDLDirectionalCascadeSettings {
		std::uint32_t Count = 4;
		std::uint32_t Resolution = 2048;
		float MaximumDistance = 1000.0f;
		float SplitLambda = 0.65f;
		float TransitionFraction = 0.08f;
	};

	struct SDLDirectionalCascade {
		glm::mat4 Matrix{1.0f};
		glm::vec3 Origin{0.0f};
		float Near = 0.0f;
		float Far = 0.0f;
		float TransitionStart = 0.0f;
		float WorldUnitsPerTexel = 0.0f;
		float DepthSpan = 0.0f;
	};

	struct SDLDirectionalCascadePlan {
		std::array<SDLDirectionalCascade, MaximumDirectionalCascades> Cascades{};
		std::uint32_t Count = 0;
		std::uint32_t Resolution = 0;
	};

	// The split sphere has a fixed radius for each split and FOV. Its center moves
	// on the light plane only at texel boundaries. All arithmetic up to the final
	// GPU matrix uses double precision so a distant camera does not quantize the
	// snap decision to a float-sized world step.
	[[nodiscard]] inline std::optional<SDLDirectionalCascadePlan> MakeSDLDirectionalCascadePlan(
		const RenderCameraSnapshot &Camera, glm::vec3 SunDirection, float Aspect,
		const SDLDirectionalCascadeSettings &Settings = {}) {
		if (Settings.Count == 0 || Settings.Count > MaximumDirectionalCascades ||
			Settings.Resolution < 256 || Settings.Resolution > 4096 ||
			!std::isfinite(Settings.MaximumDistance) || !std::isfinite(Settings.SplitLambda) ||
			!std::isfinite(Settings.TransitionFraction) || Settings.SplitLambda < 0.0f ||
			Settings.SplitLambda > 1.0f || Settings.TransitionFraction < 0.0f ||
			Settings.TransitionFraction > 0.25f || !std::isfinite(Aspect) || Aspect <= 0.0f ||
			!std::isfinite(Camera.NearPlane) || !std::isfinite(Camera.FarPlane) ||
			Camera.NearPlane <= 0.0f || Camera.FarPlane <= Camera.NearPlane ||
			!std::isfinite(Camera.VerticalFieldOfView) || Camera.VerticalFieldOfView <= 0.0f ||
			Camera.VerticalFieldOfView >= 179.0f || !std::isfinite(glm::length(SunDirection)) ||
			glm::length(SunDirection) < 0.5f || !std::isfinite(glm::length(Camera.LookDirection)) ||
			glm::length(Camera.LookDirection) < 0.5f) return std::nullopt;
		const double Near = Camera.NearPlane;
		const double Far = std::min<double>(Camera.FarPlane, Settings.MaximumDistance);
		if (Far <= Near) return std::nullopt;
		const glm::dvec3 Direction = glm::normalize(glm::dvec3(SunDirection));
		const glm::dvec3 Look = glm::normalize(glm::dvec3(Camera.LookDirection));
		const glm::dvec3 Up = std::abs(Direction.y) > 0.99 ? glm::dvec3(0, 0, 1) : glm::dvec3(0, 1, 0);
		const glm::dvec3 LightX = glm::normalize(glm::cross(Up, Direction));
		const glm::dvec3 LightY = glm::cross(Direction, LightX);
		const glm::dvec3 CameraPosition(Camera.Position);
		const glm::dvec3 Origin = glm::floor(CameraPosition / 1024.0) * 1024.0;
		const double TanHalf = std::tan(glm::radians(static_cast<double>(Camera.VerticalFieldOfView)) * 0.5);
		SDLDirectionalCascadePlan Plan;
		Plan.Count = Settings.Count;
		Plan.Resolution = Settings.Resolution;
		double Start = Near;
		for (std::uint32_t Index = 0; Index < Settings.Count; ++Index) {
			const double Fraction = static_cast<double>(Index + 1) / Settings.Count;
			const double Logarithmic = Near * std::pow(Far / Near, Fraction);
			const double Uniform = Near + (Far - Near) * Fraction;
			const double End = Index + 1 == Settings.Count ? Far :
				Settings.SplitLambda * Logarithmic + (1.0 - Settings.SplitLambda) * Uniform;
			const double HalfDepth = (End - Start) * 0.5;
			const double CenterDepth = (Start + End) * 0.5;
			const double CornerRadius = End * TanHalf * std::sqrt(1.0 + static_cast<double>(Aspect) * Aspect);
			// A bounding sphere around the whole slice has a view-independent XY extent.
			// Snapping can displace the light-plane center by half a texel on
			// each axis. Reserve two texels at every edge before fitting.
			const double Radius = std::hypot(HalfDepth, CornerRadius) *
				(1.0 + 4.0 / Settings.Resolution);
			const double Texel = 2.0 * Radius / Settings.Resolution;
			const auto Center = CameraPosition + Look * CenterDepth;
			const double SnapX = std::floor(glm::dot(Center, LightX) / Texel + 0.5) * Texel;
			const double SnapY = std::floor(glm::dot(Center, LightY) / Texel + 0.5) * Texel;
			// Depth translation has no effect on XY texel alignment, but a moving
			// unsnapped depth center would invalidate an otherwise reusable map.
			const double DepthStep = 32.0;
			const double CenterZ = std::floor(glm::dot(Center, Direction) / DepthStep + 0.5) * DepthStep;
			const glm::dvec3 SnappedCenter = LightX * SnapX + LightY * SnapY + Direction * CenterZ;
			// The extra depth range admits off-slice casters along the sun axis.
			// XY coverage remains the actual cache and draw-classification boundary.
			const double DepthExtent = Radius + 0.5 * Settings.MaximumDistance + DepthStep;
			const auto RelativeCenter = SnappedCenter - Origin;
			const auto View = glm::lookAt(RelativeCenter + Direction * DepthExtent,
				RelativeCenter, LightY);
			const auto Projection = glm::orthoRH_ZO(-Radius, Radius, -Radius, Radius, 0.0, 2.0 * DepthExtent);
			const glm::dmat4 Matrix = Projection * View;
			for (std::uint32_t Column = 0; Column < 4; ++Column)
				for (std::uint32_t Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row]) ||
						std::abs(Matrix[Column][Row]) > std::numeric_limits<float>::max()) return std::nullopt;
			auto &Cascade = Plan.Cascades[Index];
			Cascade.Matrix = glm::mat4(Matrix);
			Cascade.Origin = glm::vec3(Origin);
			Cascade.Near = static_cast<float>(Start);
			Cascade.Far = static_cast<float>(End);
			Cascade.TransitionStart = static_cast<float>(End - (End - Start) * Settings.TransitionFraction);
			Cascade.WorldUnitsPerTexel = static_cast<float>(Texel);
			Cascade.DepthSpan = static_cast<float>(2.0 * DepthExtent);
			Start = End;
		}
		return Plan;
	}
}

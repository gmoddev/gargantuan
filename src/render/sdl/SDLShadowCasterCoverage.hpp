// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace gargantuan {
	// Conservatively test the light-plane footprint. Casters may lie outside a
	// receiver split along the light axis, so clip depth is deliberately ignored.
	[[nodiscard]] inline bool SDLShadowCasterMayAffect(const RenderProjection &Projection,
		const RenderItem &Item, const std::optional<RenderMeshIdentity> &Mesh,
		const glm::mat4 &ShadowMatrix, std::uint32_t Resolution,
		glm::vec3 Origin = glm::vec3(0.0f)) {
		if (Projection.GetAnimationPose(Item.Object)) return true;
		RenderBounds Bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
		if (Mesh) {
			const auto *MeshBounds = Projection.GetMeshBounds(*Mesh);
			if (!MeshBounds) return true;
			Bounds = *MeshBounds;
		}
		auto RelativeModel = glm::dmat4(Item.ModelMatrix);
		RelativeModel[3] -= glm::dvec4(Origin, 0.0);
		const glm::dmat4 Transform = glm::dmat4(ShadowMatrix) * RelativeModel;
		double MinX = std::numeric_limits<double>::infinity();
		double MinY = MinX;
		double MaxX = -MinX;
		double MaxY = -MinX;
		for (unsigned Corner = 0; Corner < 8; ++Corner) {
			const glm::dvec4 Point(
				(Corner & 1) ? Bounds.Maximum.x : Bounds.Minimum.x,
				(Corner & 2) ? Bounds.Maximum.y : Bounds.Minimum.y,
				(Corner & 4) ? Bounds.Maximum.z : Bounds.Minimum.z, 1.0);
			const auto Clip = Transform * Point;
			if (!std::isfinite(Clip.x) || !std::isfinite(Clip.y) || !std::isfinite(Clip.w) ||
				Clip.w <= 0.0) return true;
			const double X = Clip.x / Clip.w;
			const double Y = Clip.y / Clip.w;
			MinX = std::min(MinX, X);
			MinY = std::min(MinY, Y);
			MaxX = std::max(MaxX, X);
			MaxY = std::max(MaxY, Y);
		}
		const double Guard = 4.0 / std::max(Resolution, 1u);
		return MaxX >= -1.0 - Guard && MinX <= 1.0 + Guard &&
			MaxY >= -1.0 - Guard && MinY <= 1.0 + Guard;
	}
}

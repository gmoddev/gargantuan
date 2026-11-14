// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#pragma once

#include "gargantuan/render/RenderEnvironment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace gargantuan {
	[[nodiscard]] inline bool IsValidReflectionProbe(const RenderReflectionProbeState &Probe) {
		const auto Finite = [](const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		};
		if (!Probe.Object.IsValid() || !Finite(Probe.Position) || !Finite(Probe.HalfSize) ||
			!Finite(Probe.CaptureOffset) || !std::isfinite(Probe.BlendDistance) || !std::isfinite(Probe.Intensity) ||
			glm::any(glm::greaterThan(glm::abs(Probe.Position), glm::vec3(1.0e6f))) ||
			glm::any(glm::lessThan(Probe.HalfSize, glm::vec3(0.05f))) ||
			glm::any(glm::greaterThan(Probe.HalfSize, glm::vec3(8192.0f))) ||
			glm::any(glm::greaterThanEqual(glm::abs(Probe.CaptureOffset), Probe.HalfSize)) ||
			Probe.BlendDistance < 0.0f || Probe.BlendDistance > 8192.0f ||
			Probe.Intensity < 0.0f || Probe.Intensity > 8.0f || Probe.Priority < -1024 || Probe.Priority > 1024 ||
			Probe.Source.FaceDimension == 0 || Probe.Source.FaceDimension > 1024) return false;
		for (glm::length_t Column = 0; Column < 3; ++Column) {
			if (!Finite(Probe.Rotation[Column])) return false;
			for (glm::length_t Other = Column; Other < 3; ++Other)
				if (std::abs(glm::dot(Probe.Rotation[Column], Probe.Rotation[Other]) -
					(Column == Other ? 1.0f : 0.0f)) > 1.0e-3f) return false;
		}
		if (std::abs(glm::determinant(Probe.Rotation) - 1.0f) > 1.0e-3f) return false;
		for (const auto &Face : Probe.Source.Faces)
			if (!Face.Texture.IsValid() || Face.ContentRevision == 0) return false;
		return true;
	}

	// Weight depends on the shading point, never the view origin. A zero blend
	// distance gives a hard authored boundary; otherwise the inner fade is C1.
	[[nodiscard]] inline float ReflectionProbeInfluence(const RenderReflectionProbeState &Probe, const glm::vec3 &Point) {
		const auto Local = glm::transpose(Probe.Rotation) * (Point - Probe.Position);
		const auto Margin = Probe.HalfSize - glm::abs(Local);
		const float Distance = std::min({Margin.x, Margin.y, Margin.z});
		if (!(Distance >= 0.0f)) return 0.0f;
		if (Probe.BlendDistance == 0.0f) return 1.0f;
		const float T = std::clamp(Distance / Probe.BlendDistance, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	struct ReflectionProbeBlend {
		std::array<std::size_t, 2> Indices{};
		std::array<float, 2> Weights{};
		float GlobalWeight = 1.0f;
	};

	[[nodiscard]] inline ReflectionProbeBlend SelectReflectionProbes(
		std::span<const RenderReflectionProbeState> Probes, const glm::vec3 &Point
	) {
		ReflectionProbeBlend Result;
		std::array<std::int32_t, 2> Priorities{std::numeric_limits<std::int32_t>::min(),
			std::numeric_limits<std::int32_t>::min()};
		for (std::size_t Index = 0; Index < Probes.size(); ++Index) {
			const auto &Probe = Probes[Index];
			const float Weight = ReflectionProbeInfluence(Probe, Point);
			if (Weight <= 0.0f) continue;
			const auto Before = [&](std::size_t Slot) {
				return Probe.Priority > Priorities[Slot] || (Probe.Priority == Priorities[Slot] &&
					(Weight > Result.Weights[Slot] || (Weight == Result.Weights[Slot] &&
					Probe.Object < Probes[Result.Indices[Slot]].Object)));
			};
			if (Before(0)) {
				Result.Indices[1] = Result.Indices[0]; Result.Weights[1] = Result.Weights[0]; Priorities[1] = Priorities[0];
				Result.Indices[0] = Index; Result.Weights[0] = Weight; Priorities[0] = Probe.Priority;
			} else if (Before(1)) {
				Result.Indices[1] = Index; Result.Weights[1] = Weight; Priorities[1] = Probe.Priority;
			}
		}
		if (Priorities[0] != Priorities[1]) Result.Weights[1] *= 1.0f - Result.Weights[0];
		const auto Sum = Result.Weights[0] + Result.Weights[1];
		if (Sum > 1.0f) {
			Result.Weights[0] /= Sum; Result.Weights[1] /= Sum;
		}
		Result.GlobalWeight = std::max(0.0f, 1.0f - Result.Weights[0] - Result.Weights[1]);
		return Result;
	}

	// Return a direction in the captured cube's local axes. A slab exit avoids
	// division by zero for exactly parallel rays and supports rotated volumes.
	[[nodiscard]] inline glm::vec3 ParallaxCorrectReflection(
		const RenderReflectionProbeState &Probe, const glm::vec3 &Point, const glm::vec3 &Ray
	) {
		const auto InverseRotation = glm::transpose(Probe.Rotation);
		const auto LocalRay = InverseRotation * Ray;
		if (!Probe.BoxProjection) return LocalRay;
		const auto Local = InverseRotation * (Point - Probe.Position);
		if (glm::any(glm::greaterThan(glm::abs(Local), Probe.HalfSize))) return LocalRay;
		double Exit = std::numeric_limits<double>::infinity();
		for (glm::length_t Axis = 0; Axis < 3; ++Axis) {
			if (std::abs(LocalRay[Axis]) <= 1.0e-12f) continue;
			const double Boundary = LocalRay[Axis] > 0.0f ? Probe.HalfSize[Axis] : -Probe.HalfSize[Axis];
			Exit = std::min(Exit, (Boundary - Local[Axis]) / static_cast<double>(LocalRay[Axis]));
		}
		if (!std::isfinite(Exit) || Exit < 0.0) return LocalRay;
		return Local + LocalRay * static_cast<float>(Exit) - Probe.CaptureOffset;
	}
}

// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/classes/LocalLight.hpp"
#include "gargantuan/classes/SpotLight.hpp"
#include "gargantuan/render/RenderEnvironment.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <cmath>
#include <stdexcept>

namespace gargantuan {
	namespace {
		bool InRange(float Value, float Minimum, float Maximum) {
			return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
		}

		bool IsRigid(const CFrame &Frame) {
			for (const auto Component : {Frame.Position.x, Frame.Position.y, Frame.Position.z})
				if (!InRange(Component, -MaximumLocalLightCoordinate, MaximumLocalLightCoordinate)) return false;
			const auto &Rotation = Frame.Rotation;
			constexpr float Tolerance = 1.0e-3f;
			for (glm::length_t Row = 0; Row < 3; ++Row) {
				for (glm::length_t Column = Row; Column < 3; ++Column) {
					const float Error = glm::dot(Rotation[Row], Rotation[Column]) - (Row == Column ? 1.0f : 0.0f);
					if (!InRange(Error, -Tolerance, Tolerance)) return false;
				}
			}
			return InRange(glm::determinant(Rotation), 1.0f - Tolerance, 1.0f + Tolerance);
		}
	}

	Color3 LocalLight::GetLightColor() const { return LightColor; }

	void LocalLight::SetLightColor(Color3 Value) {
		AssertCanMutate();
		for (const auto Component : {Value.R, Value.G, Value.B})
			if (!InRange(Component, 0.0f, 1.0f))
				throw std::invalid_argument("[Environment:LocalLight] LightColor channels must be in [0,1]");
		ValidatePropertyMutation("LightColor", Value);
		if (static_cast<glm::vec3>(LightColor) == static_cast<glm::vec3>(Value)) return;
		LightColor = Value;
		NotifyPropertyCommitted("LightColor");
	}

	float LocalLight::GetLightIntensity() const { return LightIntensity; }

	void LocalLight::SetLightIntensity(float Value) {
		AssertCanMutate();
		if (!InRange(Value, 0.0f, MaximumLocalLightIntensity))
			throw std::invalid_argument("[Environment:LocalLight] LightIntensity must be in [0,4096]");
		ValidatePropertyMutation("LightIntensity", Value);
		if (LightIntensity == Value) return;
		LightIntensity = Value;
		NotifyPropertyCommitted("LightIntensity");
	}

	float LocalLight::GetLightRange() const { return LightRange; }

	void LocalLight::SetLightRange(float Value) {
		AssertCanMutate();
		if (!InRange(Value, MinimumLocalLightRange, MaximumLocalLightRange))
			throw std::invalid_argument("[Environment:LocalLight] LightRange must be in [0.01,1024]");
		ValidatePropertyMutation("LightRange", Value);
		if (LightRange == Value) return;
		LightRange = Value;
		NotifyPropertyCommitted("LightRange");
	}

	CFrame LocalLight::GetLightCFrame() const { return LightCFrame; }

	void LocalLight::SetLightCFrame(CFrame Value) {
		AssertCanMutate();
		if (!IsRigid(Value))
			throw std::invalid_argument("[Environment:LocalLight] LightCFrame must be rigid, finite, and within world coordinate bounds");
		ValidatePropertyMutation("LightCFrame", Value);
		if (LightCFrame.Position == Value.Position && LightCFrame.Rotation == Value.Rotation) return;
		LightCFrame = Value;
		NotifyPropertyCommitted("LightCFrame");
	}

	Vector2 SpotLight::GetLightConeAngles() const { return LightConeAngles; }

	void SpotLight::SetLightConeAngles(Vector2 Value) {
		AssertCanMutate();
		if (!InRange(Value.GetY(), 1.0f, 89.0f) || !InRange(Value.GetX(), 0.0f, Value.GetY()))
			throw std::invalid_argument("[Environment:SpotLight] Expected cone half angles 0 <= inner <= outer and 1 <= outer <= 89");
		ValidatePropertyMutation("LightConeAngles", Value);
		if (LightConeAngles == Value) return;
		LightConeAngles = Value;
		NotifyPropertyCommitted("LightConeAngles");
	}
}

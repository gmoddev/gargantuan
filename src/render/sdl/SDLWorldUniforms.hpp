// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLShadowResources.hpp"
#include "SDLSpotShadowAtlas.hpp"

#include <array>
#include <cstddef>
#include <glm/glm.hpp>

namespace gargantuan {
	// Keep the base fields and appended shadow fields byte-identical to
	// assets/shaders/world-uniforms.glsl in both vertex and fragment stages.
	struct alignas(16) SDLWorldUniforms {
		glm::mat4 ViewMatrix;
		glm::mat4 ProjectionMatrix;
		glm::mat4 ShadowBiasMatrix;
		glm::vec4 SunDirectionIntensity;
		glm::vec4 AmbientExposure;
		glm::vec4 SunColorFogEnabled;
		glm::vec4 FogColorStart;
		glm::vec4 CameraPositionFogEnd;
		glm::vec4 EnvironmentLighting;
		glm::vec4 EnvironmentRotationTone;
		glm::vec4 LocalLighting;
		glm::vec4 ShadowControls;
		std::array<glm::mat4, MaximumDirectionalCascades> CascadeShadowMatrices;
		std::array<glm::vec4, MaximumDirectionalCascades> CascadeShadowOrigins;
		std::array<glm::vec4, MaximumDirectionalCascades> CascadeShadowSplits;
		std::array<glm::vec4, MaximumDirectionalCascades> CascadeShadowMetrics;
		glm::vec4 SpotShadowControls;
		std::array<glm::mat4, MaximumShadowedSpotlights> SpotShadowMatrices;
		std::array<glm::vec4, MaximumShadowedSpotlights> SpotShadowOrigins;
	};
	static_assert(offsetof(SDLWorldUniforms, ShadowControls) == 320);
	static_assert(offsetof(SDLWorldUniforms, CascadeShadowMatrices) == 336);
	static_assert(offsetof(SDLWorldUniforms, SpotShadowControls) == 784);
	static_assert(sizeof(SDLWorldUniforms) == 1440);

	inline const glm::mat4 SDLShadowBiasMatrix{
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f,
	};

	inline void PopulateSDLWorldShadows(SDLWorldUniforms &World, const SDLFrameContext &Context) {
		const auto &Settings = Context.Projection.GetFrame().Environment.DirectionalShadows;
		World.ShadowControls = {};
		if (Context.CascadedShadows && Context.CascadeShadowTexture && Context.CascadePlan.Count > 0) {
			World.ShadowControls = {static_cast<float>(Context.CascadePlan.Count),
				Settings.DepthBias, Settings.NormalBias, 1.0f / Context.CascadePlan.Resolution};
			for (std::uint32_t Index = 0; Index < Context.CascadePlan.Count; ++Index) {
				const auto &Cascade = Context.CascadePlan.Cascades[Index];
				World.CascadeShadowMatrices[Index] = SDLShadowBiasMatrix * Cascade.Matrix;
				World.CascadeShadowOrigins[Index] = glm::vec4(Cascade.Origin, 0.0f);
				World.CascadeShadowSplits[Index] = {Cascade.Near, Cascade.Far,
					Cascade.TransitionStart, 0.0f};
				World.CascadeShadowMetrics[Index] = {Cascade.WorldUnitsPerTexel, Cascade.DepthSpan, 0.0f, 0.0f};
			}
		}
		World.SpotShadowControls = {};
		if (Context.SpotShadowAtlas && Context.LocalShadowTexture) {
			for (const auto &Assignment : Context.SpotShadowAtlas->GetAssignments()) {
				if (!Assignment.Sampleable || Assignment.Layer >= MaximumShadowedSpotlights) continue;
				World.SpotShadowMatrices[Assignment.Layer] = SDLShadowBiasMatrix * Assignment.Matrix;
				World.SpotShadowOrigins[Assignment.Layer] = glm::vec4(Assignment.Origin, 0.0f);
				World.SpotShadowControls.x += 1.0f;
			}
			World.SpotShadowControls.y = 0.0005f;
			World.SpotShadowControls.z = 0.01f;
			World.SpotShadowControls.w = 1.0f / SpotlightShadowResolution;
		}
	}
}

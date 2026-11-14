// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/runtime/ObjectId.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

namespace gargantuan {
	struct RenderTextureIdentity {
		std::uint64_t Slot = 0;
		std::uint32_t Generation = 0;
		auto operator<=>(const RenderTextureIdentity &) const = default;
		[[nodiscard]] bool IsValid() const {
			return Slot != 0 && Generation != 0;
		}
	};

	struct RenderTextureIdentityHash {
		std::size_t operator()(const RenderTextureIdentity &Value) const noexcept {
			return std::hash<std::uint64_t>{}(Value.Slot) ^ (std::hash<std::uint32_t>{}(Value.Generation) << 1);
		}
	};

	enum class RenderSkyFace : std::uint8_t { PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ, NegativeZ };

	struct RenderSkyFaceState {
		RenderTextureIdentity Texture;
		std::uint64_t ContentRevision = 0;
		bool operator==(const RenderSkyFaceState &) const = default;
	};

	struct RenderSkyState {
		std::uint32_t FaceDimension = 0;
		std::array<RenderSkyFaceState, 6> Faces{};
		bool operator==(const RenderSkyState &) const = default;
	};

	struct RenderFogState {
		bool Enabled = false;
		glm::vec3 Color{0.5f, 0.6f, 0.7f};
		float Start = 0.0f;
		float End = 1000.0f;
	};

	inline constexpr std::size_t MaximumRenderLocalLights = 512;
	inline constexpr float MaximumLocalLightIntensity = 4096.0f;
	inline constexpr float MinimumLocalLightRange = 0.01f;
	inline constexpr float MaximumLocalLightRange = 1024.0f;
	inline constexpr float MaximumLocalLightCoordinate = 1.0e6f;
	inline constexpr std::size_t MaximumRenderReflectionProbes = 32;
	inline constexpr std::size_t MaximumResidentReflectionProbes = 12;

	struct RenderReflectionProbeState {
		ObjectId Object;
		glm::vec3 Position{0.0f};
		glm::mat3 Rotation{1.0f};
		glm::vec3 HalfSize{8.0f};
		glm::vec3 CaptureOffset{0.0f};
		float BlendDistance = 1.0f;
		float Intensity = 1.0f;
		std::int32_t Priority = 0;
		bool BoxProjection = true;
		RenderSkyState Source;
		bool operator==(const RenderReflectionProbeState &) const = default;
	};

	struct RenderDirectionalShadowSettings {
		std::uint32_t CascadeCount = 0;
		std::uint32_t Resolution = 2048;
		float Distance = 250.0f;
		float SplitLambda = 0.6f;
		float TransitionFraction = 0.1f;
		float DepthBias = 0.0005f;
		float NormalBias = 0.01f;
		bool operator==(const RenderDirectionalShadowSettings &) const = default;
	};

	enum class RenderLocalLightKind : std::uint8_t { Point, Spot };
	enum class RenderToneMapping : std::uint8_t { Filmic, Reinhard };

	struct RenderLocalLightState {
		ObjectId Object;
		RenderLocalLightKind Kind = RenderLocalLightKind::Point;
		glm::vec3 Position{0.0f};
		glm::vec3 Direction{0.0f, 0.0f, -1.0f};
		glm::vec3 Color{1.0f};
		float Intensity = 16.0f;
		float Range = 16.0f;
		float InnerConeCosine = 1.0f;
		float OuterConeCosine = 1.0f;
		bool ShadowEnabled = false;
		std::int32_t ShadowPriority = 0;

		bool operator==(const RenderLocalLightState &) const = default;
	};

	struct RenderEnvironmentState {
		glm::vec3 AmbientColor{0.2f};
		glm::vec3 SunDirection{0.0f, 1.0f, 0.0f};
		glm::vec3 SunColor{1.0f};
		float SunIntensity = 1.0f;
		float ExposureMultiplier = 1.0f;
		glm::vec3 EnvironmentColor{0.08f, 0.12f, 0.2f};
		RenderFogState Fog;
		std::optional<RenderSkyState> Sky;
		std::shared_ptr<const std::vector<RenderLocalLightState>> LocalLights;
		float EnvironmentRotation = 0.0f;
		float IblIntensity = 1.0f;
		RenderToneMapping ToneMapping = RenderToneMapping::Filmic;
		std::shared_ptr<const std::vector<RenderReflectionProbeState>> ReflectionProbes;
		RenderDirectionalShadowSettings DirectionalShadows;
	};
}

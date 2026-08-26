// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

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
	};

	struct RenderSkyState {
		std::uint32_t FaceDimension = 0;
		std::array<RenderSkyFaceState, 6> Faces{};
	};

	struct RenderFogState {
		bool Enabled = false;
		glm::vec3 Color{0.5f, 0.6f, 0.7f};
		float Start = 0.0f;
		float End = 1000.0f;
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
	};
}

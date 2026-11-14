// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"

#include <SDL3/SDL_gpu.h>

#include <optional>
#include <unordered_set>

namespace gargantuan {
	class SDLDirectionalShadowCache final {
	  public:
		void ApplyPublication(const RenderProjection &Before, const RenderPublication &Publication);
		[[nodiscard]] bool Restore(const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target,
			glm::mat4 &Matrix) const;
		void BeginRecording();
		void DependOnMesh(RenderMeshIdentity Mesh);
		void DependOnMask(RenderTextureIdentity Texture);
		void FinishRecording(const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target,
			const glm::mat4 &Matrix, bool AllCastersResident, bool HasCasters);
		[[nodiscard]] bool OnFrameSubmitted();
		void Invalidate();

	  private:
		struct CachedMap {
			const SDL_GPUTexture *Target;
			glm::vec3 SunDirection;
			glm::mat4 Matrix;
			bool SunEnabled;
			bool HasCasters;
		};
		std::optional<CachedMap> Committed;
		std::optional<CachedMap> Recorded;
		std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> Meshes;
		std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> Masks;
		bool DependenciesOverflow = false;
	};
}

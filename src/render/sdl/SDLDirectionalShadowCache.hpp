// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "render/sdl/SDLDirectionalCascadePlan.hpp"
#include "render/sdl/SDLShadowPoseLedger.hpp"

#include <SDL3/SDL_gpu.h>

#include <optional>
#include <array>
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
		[[nodiscard]] bool RestoreCascade(std::uint32_t Index, const RenderEnvironmentState &Environment,
			const SDL_GPUTexture *Target, const glm::mat4 &ExpectedMatrix, glm::mat4 &Matrix,
			glm::vec3 Origin = glm::vec3(0.0f)) const;
		[[nodiscard]] bool HasCascadeCasters(std::uint32_t Index) const;
		void BeginCascadeRecording(std::uint32_t Index);
		void DependOnMesh(std::uint32_t Index, RenderMeshIdentity Mesh);
		void DependOnMask(std::uint32_t Index, RenderTextureIdentity Texture);
		void DependOnObject(std::uint32_t Index, ObjectId Object);
		void RecordPose(std::uint32_t Index, ObjectId Object, std::uint64_t Revision);
		void MergeCommittedPoses(std::uint32_t Index, SDLShadowPoseRevisions &Destination) const;
		void FinishCascadeRecording(std::uint32_t Index, const RenderEnvironmentState &Environment,
			const SDL_GPUTexture *Target, const glm::mat4 &Matrix, bool AllCastersResident, bool HasCasters,
			glm::vec3 Origin = glm::vec3(0.0f));

	  private:
		struct CachedMap {
			const SDL_GPUTexture *Target;
			glm::vec3 SunDirection;
			glm::mat4 Matrix;
			glm::vec3 Origin{0.0f};
			bool SunEnabled;
			bool HasCasters;
		};
		struct Slot {
			std::optional<CachedMap> Committed;
			std::optional<CachedMap> Recorded;
			std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> Meshes;
			std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> Masks;
			std::unordered_set<ObjectId> Objects;
			SDLShadowPoseRevisions CommittedPoses;
			SDLShadowPoseRevisions RecordingPoses;
			bool DependenciesOverflow = false;
			bool CascadeMode = false;
		};
		std::array<Slot, MaximumDirectionalCascades> Slots;
	};
}

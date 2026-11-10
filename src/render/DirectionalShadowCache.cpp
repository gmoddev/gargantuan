// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLDirectionalShadowCache.hpp"

#include <algorithm>
#include <utility>

namespace gargantuan {
	namespace {
		constexpr std::size_t MAX_DEPENDENCIES_PER_KIND = 32'768;
		enum class Coverage { None, Solid, AlphaTexture };

		Coverage GetCoverage(const RenderMaterialState &Material) {
			if (Material.OpacityMode != RenderOpacityMode::Masked) return Coverage::Solid;
			if (Material.BaseColorFactor.a < Material.AlphaCutoff) return Coverage::None;
			return Material.BaseColorTexture ? Coverage::AlphaTexture : Coverage::Solid;
		}

		bool SameDepthMaterial(const RenderMaterialState &Left, const RenderMaterialState &Right) {
			const auto Shape = GetCoverage(Left);
			if (Shape != GetCoverage(Right)) return false;
			if (Shape == Coverage::None) return true;
			if (Left.DoubleSided != Right.DoubleSided) return false;
			if (Shape == Coverage::Solid) return true;
			constexpr auto BaseColor = static_cast<std::size_t>(RenderMaterialTextureSlot::BaseColor);
			return Left.BaseColorTexture == Right.BaseColorTexture && Left.BaseColorFactor.a == Right.BaseColorFactor.a &&
				Left.AlphaCutoff == Right.AlphaCutoff && Left.TextureSamplers[BaseColor] == Right.TextureSamplers[BaseColor];
		}

		bool SameDepthPrimitives(const RenderProjectedObject &Left, const RenderObjectUpdate &Right) {
			if (!Left.Primitives && !Right.Primitives) return SameDepthMaterial(Left.Material, Right.Material);
			if (Left.Primitives == Right.Primitives) return true;
			if (!Left.Primitives || !Right.Primitives || Left.Primitives->size() != Right.Primitives->size()) return false;
			return std::ranges::equal(*Left.Primitives, *Right.Primitives, [](const auto &A, const auto &B) {
				return A.FirstIndex == B.FirstIndex && A.IndexCount == B.IndexCount && SameDepthMaterial(A.Material, B.Material);
			});
		}

		bool IsCaster(const auto &Object) { return Object.Visible && Object.Item.CastShadow; }

		bool ShadowObjectChanged(const RenderProjectedObject *Previous, const RenderObjectUpdate &Next) {
			const bool OldCaster = Previous && IsCaster(*Previous);
			const bool NewCaster = IsCaster(Next);
			if (OldCaster != NewCaster) return true;
			if (!OldCaster) return false;
			return Previous->Item.ModelMatrix != Next.Item.ModelMatrix || Previous->Item.Geometry != Next.Item.Geometry ||
				Previous->Mesh != Next.Mesh || !SameDepthPrimitives(*Previous, Next);
		}

		void TrackDependency(auto &Set, auto Identity, bool &Overflow) {
			if (Set.size() < MAX_DEPENDENCIES_PER_KIND) Set.insert(Identity);
			else if (!Set.contains(Identity)) Overflow = true;
		}
	}

	void SDLDirectionalShadowCache::ApplyPublication(const RenderProjection &Before, const RenderPublication &Publication) {
		if (Publication.FullResync) {
			Invalidate();
			return;
		}
		if (!Committed || !Committed->SunEnabled) return;
		if (!Committed->HasCasters) {
			const auto BecomesCaster = [](const auto &Object) { return IsCaster(Object); };
			if (std::ranges::any_of(Publication.Creates, BecomesCaster) ||
				std::ranges::any_of(Publication.Updates, BecomesCaster)) Invalidate();
			return;
		}
		const auto &Objects = Before.GetObjects();
		const auto Find = [&](ObjectId Object) -> const RenderProjectedObject * {
			const auto Found = Objects.find(Object);
			return Found == Objects.end() ? nullptr : &Found->second;
		};
		const auto WasCaster = [&](const auto &Change) {
			const auto *Previous = Find(Change.Object);
			return Previous && IsCaster(*Previous);
		};
		if (std::ranges::any_of(Publication.Creates, [](const auto &Create) { return IsCaster(Create); }) ||
			std::ranges::any_of(Publication.Removes, WasCaster) ||
			std::ranges::any_of(Publication.Updates, [&](const auto &Update) {
				return ShadowObjectChanged(Find(Update.Object), Update);
			}) || std::ranges::any_of(Publication.AnimationPoseUpdates, WasCaster) ||
			std::ranges::any_of(Publication.AnimationPoseRemoves, WasCaster) ||
			std::ranges::any_of(Publication.MeshVertexUpdates, [&](const auto &Update) { return Meshes.contains(Update.Mesh); }) ||
			std::ranges::any_of(Publication.MeshRemoves, [&](const auto &Remove) { return Meshes.contains(Remove.Mesh); }) ||
			std::ranges::any_of(Publication.TextureUpdates, [&](const auto &Update) { return Masks.contains(Update.Texture); }) ||
			std::ranges::any_of(Publication.TextureRemoves, [&](const auto &Remove) { return Masks.contains(Remove.Texture); }))
			Invalidate();
	}

	bool SDLDirectionalShadowCache::Restore(
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target, glm::mat4 &Matrix
	) const {
		const bool SunEnabled = Environment.SunIntensity > 0.0f;
		if (!Committed || Committed->Target != Target || Committed->SunEnabled != SunEnabled ||
			(SunEnabled && Committed->SunDirection != Environment.SunDirection)) return false;
		Matrix = Committed->Matrix;
		return true;
	}

	void SDLDirectionalShadowCache::BeginRecording() {
		Invalidate();
		Meshes.clear();
		Masks.clear();
		DependenciesOverflow = false;
	}

	void SDLDirectionalShadowCache::DependOnMesh(RenderMeshIdentity Mesh) {
		TrackDependency(Meshes, Mesh, DependenciesOverflow);
	}

	void SDLDirectionalShadowCache::DependOnMask(RenderTextureIdentity Texture) {
		TrackDependency(Masks, Texture, DependenciesOverflow);
	}

	void SDLDirectionalShadowCache::FinishRecording(
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target, const glm::mat4 &Matrix,
		bool AllCastersResident, bool HasCasters
	) {
		if (AllCastersResident && !DependenciesOverflow)
			Recorded = CachedMap{Target, Environment.SunDirection, Matrix, Environment.SunIntensity > 0.0f, HasCasters};
	}

	bool SDLDirectionalShadowCache::OnFrameSubmitted() {
		if (!Recorded) return false;
		Committed = std::exchange(Recorded, std::nullopt);
		return true;
	}

	void SDLDirectionalShadowCache::Invalidate() {
		Committed.reset();
		Recorded.reset();
	}
}

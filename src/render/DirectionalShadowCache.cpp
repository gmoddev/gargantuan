// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLDirectionalShadowCache.hpp"
#include "render/sdl/SDLShadowCasterCoverage.hpp"

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
		if (std::ranges::none_of(Slots, [](const auto &Slot) {
			return Slot.Committed && Slot.Committed->SunEnabled;
		})) return;
		const auto &Objects = Before.GetObjects();
		const auto Find = [&](ObjectId Object) -> const RenderProjectedObject * {
			const auto Found = Objects.find(Object);
			return Found == Objects.end() ? nullptr : &Found->second;
		};
		const auto IncomingCaster = [&](ObjectId Id) {
			return std::ranges::any_of(Publication.Creates,
				[&](const auto &Create) { return Create.Item.Object == Id && IsCaster(Create); }) ||
				std::ranges::any_of(Publication.Updates,
					[&](const auto &Update) { return Update.Object == Id && IsCaster(Update); });
		};
		const bool PoseCasterChanged = std::ranges::any_of(Publication.AnimationPoseUpdates,
			[&](const auto &Pose) {
				const auto *Previous = Find(Pose.Object);
				return (Previous && IsCaster(*Previous)) || IncomingCaster(Pose.Object);
			}) || std::ranges::any_of(Publication.AnimationPoseRemoves,
			[&](const auto &Pose) {
				const auto *Previous = Find(Pose.Object);
				return Previous && IsCaster(*Previous);
			});
		bool DeformableCasterChanged = false;
		if (!Publication.MeshVertexUpdates.empty()) {
			std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> EditedMeshes;
			for (const auto &Update : Publication.MeshVertexUpdates) EditedMeshes.insert(Update.Mesh);
			DeformableCasterChanged = std::ranges::any_of(Objects, [&](const auto &Entry) {
				return IsCaster(Entry.second) && Entry.second.Mesh && EditedMeshes.contains(*Entry.second.Mesh);
			}) || std::ranges::any_of(Publication.Creates, [&](const auto &Create) {
				return IsCaster(Create) && Create.Mesh && EditedMeshes.contains(*Create.Mesh);
			}) || std::ranges::any_of(Publication.Updates, [&](const auto &Update) {
				return IsCaster(Update) && Update.Mesh && EditedMeshes.contains(*Update.Mesh);
			});
		}
		const bool UnboundedCasterChange = PoseCasterChanged || DeformableCasterChanged;
		for (auto &Slot : Slots) {
			if (!Slot.Committed || !Slot.Committed->SunEnabled) continue;
			const auto &Matrix = Slot.Committed->Matrix;
			const auto Relevant = [&](const auto &Object) {
				return IsCaster(Object) && (!Slot.CascadeMode ||
					SDLShadowCasterMayAffect(Before, Object.Item, Object.Mesh, Matrix, 256,
						Slot.Committed->Origin));
			};
			const auto WasRelevant = [&](ObjectId Object) {
				const auto *Previous = Find(Object);
				return Previous && Relevant(*Previous);
			};
			if (!Slot.Committed->HasCasters) {
				if (UnboundedCasterChange || std::ranges::any_of(Publication.Creates, Relevant) ||
					std::ranges::any_of(Publication.Updates, Relevant)) {
					Slot.Committed.reset();
					Slot.CommittedPoses.clear();
				}
				continue;
			}
			const bool Changed = UnboundedCasterChange || std::ranges::any_of(Publication.Creates, Relevant) ||
				std::ranges::any_of(Publication.Removes, [&](const auto &Remove) { return WasRelevant(Remove.Object); }) ||
				std::ranges::any_of(Publication.Updates, [&](const auto &Update) {
					return ShadowObjectChanged(Find(Update.Object), Update) &&
						(WasRelevant(Update.Object) || Relevant(Update));
				}) ||
				std::ranges::any_of(Publication.MeshRemoves,
					[&](const auto &Remove) { return Slot.Meshes.contains(Remove.Mesh); }) ||
				std::ranges::any_of(Publication.TextureUpdates,
					[&](const auto &Update) { return Slot.Masks.contains(Update.Texture); }) ||
				std::ranges::any_of(Publication.TextureRemoves,
					[&](const auto &Remove) { return Slot.Masks.contains(Remove.Texture); });
			if (Changed) {
				Slot.Committed.reset();
				Slot.CommittedPoses.clear();
			}
		}
	}

	bool SDLDirectionalShadowCache::Restore(
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target, glm::mat4 &Matrix
	) const {
		const auto &Committed = Slots[0].Committed;
		const bool SunEnabled = Environment.SunIntensity > 0.0f;
		if (!Committed || Committed->Target != Target || Committed->SunEnabled != SunEnabled ||
			(SunEnabled && Committed->SunDirection != Environment.SunDirection)) return false;
		Matrix = Committed->Matrix;
		return true;
	}

	bool SDLDirectionalShadowCache::RestoreCascade(std::uint32_t Index,
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target,
		const glm::mat4 &ExpectedMatrix, glm::mat4 &Matrix, glm::vec3 Origin) const {
		if (Index >= MaximumDirectionalCascades) return false;
		const auto &Committed = Slots[Index].Committed;
		const bool SunEnabled = Environment.SunIntensity > 0.0f;
		if (!Committed || !Slots[Index].CascadeMode || Committed->Target != Target ||
			Committed->SunEnabled != SunEnabled || Committed->Matrix != ExpectedMatrix ||
			Committed->Origin != Origin ||
			(SunEnabled && Committed->SunDirection != Environment.SunDirection)) return false;
		Matrix = Committed->Matrix;
		return true;
	}

	bool SDLDirectionalShadowCache::HasCascadeCasters(std::uint32_t Index) const {
		return Index < MaximumDirectionalCascades && Slots[Index].Committed && Slots[Index].Committed->HasCasters;
	}

	void SDLDirectionalShadowCache::BeginRecording() {
		BeginCascadeRecording(0);
		Slots[0].CascadeMode = false;
	}

	void SDLDirectionalShadowCache::BeginCascadeRecording(std::uint32_t Index) {
		if (Index >= MaximumDirectionalCascades) return;
		auto &Slot = Slots[Index];
		Slot.Committed.reset();
		Slot.Recorded.reset();
		Slot.Meshes.clear();
		Slot.Masks.clear();
		Slot.Objects.clear();
		Slot.CommittedPoses.clear();
		Slot.RecordingPoses.clear();
		Slot.DependenciesOverflow = false;
		Slot.CascadeMode = true;
	}

	void SDLDirectionalShadowCache::DependOnMesh(RenderMeshIdentity Mesh) { DependOnMesh(0, Mesh); }
	void SDLDirectionalShadowCache::DependOnMask(RenderTextureIdentity Texture) { DependOnMask(0, Texture); }
	void SDLDirectionalShadowCache::DependOnMesh(std::uint32_t Index, RenderMeshIdentity Mesh) {
		if (Index < MaximumDirectionalCascades)
			TrackDependency(Slots[Index].Meshes, Mesh, Slots[Index].DependenciesOverflow);
	}
	void SDLDirectionalShadowCache::DependOnMask(std::uint32_t Index, RenderTextureIdentity Texture) {
		if (Index < MaximumDirectionalCascades)
			TrackDependency(Slots[Index].Masks, Texture, Slots[Index].DependenciesOverflow);
	}
	void SDLDirectionalShadowCache::DependOnObject(std::uint32_t Index, ObjectId Object) {
		if (Index < MaximumDirectionalCascades)
			TrackDependency(Slots[Index].Objects, Object, Slots[Index].DependenciesOverflow);
	}
	void SDLDirectionalShadowCache::RecordPose(std::uint32_t Index, ObjectId Object, std::uint64_t Revision) {
		if (Index >= MaximumDirectionalCascades || Revision == 0) return;
		auto &Slot = Slots[Index];
		if (Slot.RecordingPoses.size() >= MAX_DEPENDENCIES_PER_KIND && !Slot.RecordingPoses.contains(Object)) {
			Slot.DependenciesOverflow = true;
			return;
		}
		MergeSDLShadowPoseRevision(Slot.RecordingPoses, Object, Revision);
	}
	void SDLDirectionalShadowCache::MergeCommittedPoses(std::uint32_t Index,
		SDLShadowPoseRevisions &Destination) const {
		if (Index < MaximumDirectionalCascades && Slots[Index].Committed)
			MergeSDLShadowPoseRevisions(Destination, Slots[Index].CommittedPoses);
	}

	void SDLDirectionalShadowCache::FinishRecording(
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target, const glm::mat4 &Matrix,
		bool AllCastersResident, bool HasCasters
	) {
		FinishCascadeRecording(0, Environment, Target, Matrix, AllCastersResident, HasCasters);
	}

	void SDLDirectionalShadowCache::FinishCascadeRecording(std::uint32_t Index,
		const RenderEnvironmentState &Environment, const SDL_GPUTexture *Target, const glm::mat4 &Matrix,
		bool AllCastersResident, bool HasCasters, glm::vec3 Origin) {
		if (Index >= MaximumDirectionalCascades) return;
		auto &Slot = Slots[Index];
		if (AllCastersResident && !Slot.DependenciesOverflow)
			Slot.Recorded = CachedMap{Target, Environment.SunDirection, Matrix, Origin,
				Environment.SunIntensity > 0.0f, HasCasters};
	}

	bool SDLDirectionalShadowCache::OnFrameSubmitted() {
		bool Built = false;
		for (auto &Slot : Slots) {
			if (Slot.Recorded) {
				Slot.Committed = std::exchange(Slot.Recorded, std::nullopt);
				Slot.CommittedPoses = std::move(Slot.RecordingPoses);
				Built = true;
			}
			Slot.RecordingPoses.clear();
		}
		return Built;
	}

	void SDLDirectionalShadowCache::Invalidate() {
		for (auto &Slot : Slots) {
			Slot.Committed.reset();
			Slot.Recorded.reset();
			Slot.CommittedPoses.clear();
			Slot.RecordingPoses.clear();
		}
	}
}

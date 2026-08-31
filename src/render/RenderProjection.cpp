// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderProjection.hpp"

#include <algorithm>
#include <glm/geometric.hpp>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <cmath>

namespace gargantuan {
	namespace {
		bool IsFinite(const glm::vec2 &Value) { return std::isfinite(Value.x) && std::isfinite(Value.y); }
		bool IsFinite(const glm::vec3 &Value) { return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z); }
		bool IsFinite(const glm::vec4 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
		}
		bool IsFinite(const glm::mat4 &Value) {
			for (glm::length_t Column = 0; Column < 4; ++Column) if (!IsFinite(Value[Column])) return false;
			return true;
		}
		bool IsValid(const RenderBounds &Bounds) {
			return IsFinite(Bounds.Minimum) && IsFinite(Bounds.Maximum) &&
				Bounds.Minimum.x <= Bounds.Maximum.x && Bounds.Minimum.y <= Bounds.Maximum.y &&
				Bounds.Minimum.z <= Bounds.Maximum.z;
		}
		bool IsValid(const RenderItem &Item) {
			return Item.Object.IsValid() && IsFinite(Item.ModelMatrix) && IsFinite(Item.InverseModelMatrix) &&
				IsFinite(Item.Color);
		}
		bool IsValid(const RenderMaterialState &Material) {
			const bool OpacityModeValid = Material.OpacityMode == RenderOpacityMode::Opaque ||
				Material.OpacityMode == RenderOpacityMode::Masked || Material.OpacityMode == RenderOpacityMode::Transparent;
			return Material.Revision != 0 && IsFinite(Material.BaseColorFactor) &&
				(!Material.BaseColorTexture || Material.BaseColorTexture->IsValid()) &&
				(!Material.NormalTexture || Material.NormalTexture->IsValid()) &&
				std::isfinite(Material.Metallic) && Material.Metallic >= 0.0f && Material.Metallic <= 1.0f &&
				std::isfinite(Material.Roughness) && Material.Roughness >= 0.0f && Material.Roughness <= 1.0f &&
				OpacityModeValid && std::isfinite(Material.AlphaCutoff) && Material.AlphaCutoff >= 0.0f &&
				Material.AlphaCutoff <= 1.0f;
		}
		bool AreValid(const std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> &Primitives) {
			if (!Primitives) return true;
			if (Primitives->empty()) return false;
			std::uint64_t PreviousEnd = 0;
			for (const auto &Primitive : *Primitives) {
				const auto End = static_cast<std::uint64_t>(Primitive.FirstIndex) + Primitive.IndexCount;
				if (Primitive.IndexCount == 0 || Primitive.IndexCount % 3 != 0 ||
					Primitive.FirstIndex < PreviousEnd || End > std::numeric_limits<std::uint32_t>::max() ||
					!IsValid(Primitive.Material)) return false;
				PreviousEnd = End;
			}
			return true;
		}
		bool IsValid(const RenderVertex &Vertex) {
			return IsFinite(Vertex.Position) && IsFinite(Vertex.Normal) && IsFinite(Vertex.Tangent) &&
				IsFinite(Vertex.TextureCoordinate);
		}
		bool IsUnitColor(const glm::vec3 &Value) {
			return IsFinite(Value) && Value.x >= 0.0f && Value.x <= 1.0f && Value.y >= 0.0f &&
				Value.y <= 1.0f && Value.z >= 0.0f && Value.z <= 1.0f;
		}
		bool IsValid(const RenderEnvironmentState &Environment) {
			if (!IsUnitColor(Environment.AmbientColor) || !IsFinite(Environment.SunDirection) ||
				!IsUnitColor(Environment.SunColor) || !std::isfinite(Environment.SunIntensity) ||
				Environment.SunIntensity < 0.0f || Environment.SunIntensity > 8.0f ||
				!std::isfinite(Environment.ExposureMultiplier) || Environment.ExposureMultiplier < (1.0f / 256.0f) ||
				Environment.ExposureMultiplier > 256.0f || !IsUnitColor(Environment.EnvironmentColor) ||
				!IsUnitColor(Environment.Fog.Color) || !std::isfinite(Environment.Fog.Start) ||
				!std::isfinite(Environment.Fog.End) || Environment.Fog.Start < 0.0f ||
				Environment.Fog.End < Environment.Fog.Start || Environment.Fog.End > 100000.0f ||
				std::abs(glm::length(Environment.SunDirection) - 1.0f) > 1e-4f) return false;
			if (!Environment.Sky) return true;
			if (Environment.Sky->FaceDimension == 0 || Environment.Sky->FaceDimension > 1024) return false;
			for (const auto &Face : Environment.Sky->Faces)
				if (!Face.Texture.IsValid() || Face.ContentRevision == 0) return false;
			return true;
		}
		bool IsValid(const RenderFrameState &Frame) {
			const auto &Camera = Frame.Camera;
			return Frame.ViewportWidth != 0 && Frame.ViewportHeight != 0 && std::isfinite(Frame.DpiScale) &&
				Frame.DpiScale > 0.0f && IsValid(Frame.Environment) &&
				IsFinite(Camera.Position) && IsFinite(Camera.RightDirection) && IsFinite(Camera.UpDirection) &&
				IsFinite(Camera.LookDirection) && std::isfinite(Camera.VerticalFieldOfView) &&
				Camera.VerticalFieldOfView > 0.0f && Camera.VerticalFieldOfView < 180.0f &&
				std::isfinite(Camera.NearPlane) && std::isfinite(Camera.FarPlane) && Camera.NearPlane > 0.0f &&
				Camera.FarPlane > Camera.NearPlane && IsFinite(Camera.ViewMatrix) && IsFinite(Camera.ProjectionMatrix) &&
				IsFinite(Camera.ViewProjectionMatrix);
		}

		bool HasExactRgba8Bytes(
			std::uint32_t Width,
			std::uint32_t Height,
			const std::shared_ptr<const std::vector<std::uint8_t>> &Pixels
		) {
			if (Width == 0 || Height == 0 || !Pixels) return false;
			const auto Expected = static_cast<std::uint64_t>(Width) * static_cast<std::uint64_t>(Height) * 4u;
			return Expected <= std::numeric_limits<std::size_t>::max() && Pixels->size() == Expected;
		}

		bool ItemsEqual(const RenderItem &Left, const RenderItem &Right) {
			return Left.Object == Right.Object && Left.Geometry == Right.Geometry &&
				Left.ModelMatrix == Right.ModelMatrix && Left.InverseModelMatrix == Right.InverseModelMatrix &&
				Left.Color == Right.Color && Left.CastShadow == Right.CastShadow;
		}
	}

	RenderProjectionChanges RenderProjection::Apply(const RenderPublication &Publication) {
		if (Publication.Id == InvalidRenderPublicationId)
			throw std::invalid_argument("Render projection requires a valid RenderPublication identity");
		if (!Publication.FullResync && Publication.BaseId != LastPublicationId)
			throw std::invalid_argument("Render projection rejects a stale or out-of-order publication");
		if (!Publication.FullResync && Publication.Id <= Publication.BaseId)
			throw std::invalid_argument("Render projection requires publication identities to increase");
		if (Publication.FullResync && (!Publication.Updates.empty() || !Publication.Removes.empty() ||
			!Publication.MeshVertexUpdates.empty() || !Publication.MeshRemoves.empty() ||
			!Publication.AnimationPoseRemoves.empty() || !Publication.TextureUpdates.empty() ||
			!Publication.TextureRemoves.empty()))
			throw std::invalid_argument("A full RenderPublication may contain only complete create state");
		if (!IsValid(Publication.Frame))
			throw std::invalid_argument("Render publication frame state is invalid");
		const auto &PublishedUi = Publication.GetUi();
		const bool HasUiUpdate = Publication.FullResync || Publication.UiChanged || Publication.SharedUi ||
			PublishedUi.ViewportWidth != 0 || PublishedUi.ViewportHeight != 0 || !PublishedUi.Batches.empty();

		std::unordered_set<ObjectId> TouchedObjects;
		TouchedObjects.reserve(Publication.Creates.size() + Publication.Updates.size() + Publication.Removes.size());
		for (const auto &Remove : Publication.Removes)
			if (!Remove.Object.IsValid() || !TouchedObjects.insert(Remove.Object).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate object operation");
		for (const auto &Create : Publication.Creates)
			if (!IsValid(Create.Item) || !IsValid(Create.Material) || !AreValid(Create.Primitives) ||
				(Create.Mesh && !Create.Mesh->IsValid()) ||
				!TouchedObjects.insert(Create.Item.Object).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate object operation");
		for (const auto &Update : Publication.Updates) {
			constexpr auto ValidUpdateDomains = static_cast<std::uint32_t>(RenderUpdateDomain::Transform) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Material) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Visibility) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Geometry) |
				static_cast<std::uint32_t>(RenderUpdateDomain::DeformableVertices) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Hierarchy) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Environment) |
				static_cast<std::uint32_t>(RenderUpdateDomain::AnimationPose);
			if (!Update.Object.IsValid() || Update.Item.Object != Update.Object || !IsValid(Update.Item) ||
				!IsValid(Update.Material) || !AreValid(Update.Primitives) || (Update.Mesh && !Update.Mesh->IsValid()) ||
				Update.Domains == RenderUpdateDomain::None ||
				(static_cast<std::uint32_t>(Update.Domains) & ~ValidUpdateDomains) != 0 ||
				!TouchedObjects.insert(Update.Object).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate object update");
		}

		std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> TouchedMeshes;
		TouchedMeshes.reserve(Publication.MeshCreates.size() + Publication.MeshVertexUpdates.size() + Publication.MeshRemoves.size());
		for (const auto &Remove : Publication.MeshRemoves)
			if (!Remove.Mesh.IsValid() || !TouchedMeshes.insert(Remove.Mesh).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate mesh operation");
		for (const auto &Create : Publication.MeshCreates) {
			if (!Create.Mesh.IsValid() || Create.TopologyRevision == 0 || Create.VertexRevision == 0 ||
				!Create.Vertices || !Create.Indices || Create.Vertices->empty() || Create.Indices->empty() ||
				!IsValid(Create.Bounds) || !TouchedMeshes.insert(Create.Mesh).second)
				throw std::invalid_argument("Render publication mesh creation is invalid");
			for (const auto &Vertex : *Create.Vertices) if (!IsValid(Vertex))
				throw std::invalid_argument("Render publication rejects non-finite mesh vertices");
			for (const auto Index : *Create.Indices) if (Index >= Create.Vertices->size())
				throw std::invalid_argument("Render publication rejects out-of-range mesh indices");
			if (Create.SkinInfluences) {
				if (!Create.Skeleton.IsValid() || Create.SkeletonJointCount == 0 ||
					Create.SkeletonJointCount > MaximumRenderSkinPaletteEntries ||
					Create.SkinInfluences->size() != Create.Vertices->size())
					throw std::invalid_argument("Render publication skin influence count differs from vertices");
				for (const auto &Influence : *Create.SkinInfluences) {
					float Sum = 0.0f;
					for (std::size_t Index = 0; Index < 4; ++Index) {
						if (Influence.Joints[Index] >= Create.SkeletonJointCount ||
							!std::isfinite(Influence.Weights[Index]) ||
							Influence.Weights[Index] < 0.0f)
							throw std::invalid_argument("Render publication rejects malformed skin weights");
						Sum += Influence.Weights[Index];
					}
					if (std::abs(Sum - 1.0f) > 1.0e-4f)
						throw std::invalid_argument("Render publication rejects non-normalized skin weights");
				}
			} else if (Create.Skeleton.IsValid() || Create.SkeletonJointCount != 0) {
				throw std::invalid_argument("Render publication rejects skeleton metadata without skin influences");
			}
		}
		for (const auto &Update : Publication.MeshVertexUpdates) {
			if (!Update.Mesh.IsValid() || Update.VertexRevision == 0 || !Update.Vertices || Update.Vertices->empty() ||
				!IsValid(Update.Bounds) || !TouchedMeshes.insert(Update.Mesh).second)
				throw std::invalid_argument("Render publication mesh update is invalid");
			for (const auto &Vertex : *Update.Vertices) if (!IsValid(Vertex))
				throw std::invalid_argument("Render publication rejects non-finite deformable vertices");
		}

		std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> TouchedTextures;
		TouchedTextures.reserve(
			Publication.TextureCreates.size() + Publication.TextureUpdates.size() + Publication.TextureRemoves.size()
		);
		for (const auto &Remove : Publication.TextureRemoves)
			if (!Remove.Texture.IsValid() || !TouchedTextures.insert(Remove.Texture).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate texture operation");
		for (const auto &Create : Publication.TextureCreates) {
			if (!Create.Texture.IsValid() || Create.Revision == 0 ||
				Create.Format != RenderTextureFormat::Rgba8Unorm ||
				!HasExactRgba8Bytes(Create.Width, Create.Height, Create.Pixels) ||
				!TouchedTextures.insert(Create.Texture).second)
				throw std::invalid_argument("Render publication texture creation is invalid");
		}
		for (const auto &Update : Publication.TextureUpdates) {
			if (!Update.Texture.IsValid() || Update.Revision == 0 ||
				!HasExactRgba8Bytes(Update.Width, Update.Height, Update.Pixels) ||
				!TouchedTextures.insert(Update.Texture).second)
				throw std::invalid_argument("Render publication texture update is invalid");
		}

		std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> CreatedTextures;
		CreatedTextures.reserve(Publication.TextureCreates.size());
		for (const auto &Create : Publication.TextureCreates) CreatedTextures.insert(Create.Texture);
		std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> RemovedTextures;
		RemovedTextures.reserve(Publication.TextureRemoves.size());
		for (const auto &Remove : Publication.TextureRemoves) RemovedTextures.insert(Remove.Texture);
		const auto TextureExistsAfterPublication = [&](const RenderTextureIdentity &Texture) {
			if (CreatedTextures.contains(Texture)) return true;
			return !Publication.FullResync && Textures.contains(Texture) && !RemovedTextures.contains(Texture);
		};
		const auto TextureMatchesSkyFace = [&](const RenderSkyFaceState &Face, std::uint32_t Dimension) {
			for (const auto &Create : Publication.TextureCreates)
				if (Create.Texture == Face.Texture)
					return Create.Revision == Face.ContentRevision && Create.Width == Dimension && Create.Height == Dimension;
			if (Publication.FullResync || RemovedTextures.contains(Face.Texture)) return false;
			const auto Existing = Textures.find(Face.Texture);
			if (Existing == Textures.end() || Existing->second.Width != Dimension || Existing->second.Height != Dimension)
				return false;
			for (const auto &Update : Publication.TextureUpdates)
				if (Update.Texture == Face.Texture) return Update.Revision == Face.ContentRevision;
			return Existing->second.Revision == Face.ContentRevision;
		};
		if (Publication.Frame.Environment.Sky)
			for (const auto &Face : Publication.Frame.Environment.Sky->Faces)
				if (!TextureMatchesSkyFace(Face, Publication.Frame.Environment.Sky->FaceDimension))
					throw std::invalid_argument("Render publication Sky references an unavailable or incoherent texture");
		const auto MaterialTexturesExist = [&](const RenderMaterialState &Material) {
			return (!Material.BaseColorTexture || TextureExistsAfterPublication(*Material.BaseColorTexture)) &&
				(!Material.NormalTexture || TextureExistsAfterPublication(*Material.NormalTexture));
		};
		const auto ObjectTexturesExist = [&](const auto &Object) {
			if (!MaterialTexturesExist(Object.Material)) return false;
			return !Object.Primitives || std::ranges::all_of(*Object.Primitives, [&](const auto &Primitive) {
				return MaterialTexturesExist(Primitive.Material);
			});
		};
		for (const auto &Create : Publication.Creates) if (!ObjectTexturesExist(Create))
			throw std::invalid_argument("Render publication material references a texture without residency");
		for (const auto &Update : Publication.Updates) if (!ObjectTexturesExist(Update))
			throw std::invalid_argument("Render publication material references a texture without residency");
		if (!Publication.FullResync && !Publication.TextureRemoves.empty()) {
			for (const auto &[Object, Existing] : Entries) {
				if (TouchedObjects.contains(Object)) continue;
				if (!MaterialTexturesExist(Existing.Material) ||
					(Existing.Primitives && !std::ranges::all_of(*Existing.Primitives, [&](const auto &Primitive) {
						return MaterialTexturesExist(Primitive.Material);
					})))
					throw std::invalid_argument("Render publication removes a texture that remains referenced by a material");
			}
		}

		if (HasUiUpdate && (PublishedUi.ViewportWidth != 0 || PublishedUi.ViewportHeight != 0 ||
			!PublishedUi.Batches.empty())) {
			if (PublishedUi.ViewportWidth != Publication.Frame.ViewportWidth ||
				PublishedUi.ViewportHeight != Publication.Frame.ViewportHeight ||
				!std::isfinite(PublishedUi.DpiScale) || PublishedUi.DpiScale <= 0.0f)
				throw std::invalid_argument("Render publication UI frame does not match frame state");
			for (const auto &Batch : PublishedUi.Batches) {
				if ((Batch.Texture && (!Batch.Texture->IsValid() || !TextureExistsAfterPublication(*Batch.Texture))) ||
					!std::isfinite(Batch.Opacity) ||
					Batch.Opacity < 0.0f || Batch.Opacity > 1.0f)
					throw std::invalid_argument("Render publication UI batch state is invalid");
				if (Batch.Clip && (!std::isfinite(Batch.Clip->X) || !std::isfinite(Batch.Clip->Y) ||
					!std::isfinite(Batch.Clip->Width) || !std::isfinite(Batch.Clip->Height) ||
					Batch.Clip->Width < 0.0f || Batch.Clip->Height < 0.0f))
					throw std::invalid_argument("Render publication UI clip is invalid");
				for (const auto &Vertex : Batch.Vertices)
					if (!IsFinite(Vertex.Position) || !IsFinite(Vertex.TextureCoordinate) || !IsFinite(Vertex.Color))
						throw std::invalid_argument("Render publication rejects non-finite UI vertices");
				for (const auto Index : Batch.Indices) if (Index >= Batch.Vertices.size())
					throw std::invalid_argument("Render publication rejects out-of-range UI indices");
			}
		}

		std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> CreatedMeshes;
		CreatedMeshes.reserve(Publication.MeshCreates.size());
		for (const auto &Create : Publication.MeshCreates) CreatedMeshes.insert(Create.Mesh);
		std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> RemovedMeshes;
		RemovedMeshes.reserve(Publication.MeshRemoves.size());
		for (const auto &Remove : Publication.MeshRemoves) RemovedMeshes.insert(Remove.Mesh);
		const auto MeshExistsAfterPublication = [&](const RenderMeshIdentity &Mesh) {
			if (CreatedMeshes.contains(Mesh)) return true;
			return !Publication.FullResync && Meshes.contains(Mesh) && !RemovedMeshes.contains(Mesh);
		};
		const auto MeshIndexCountAfterPublication = [&](const RenderMeshIdentity &Mesh) -> std::optional<std::size_t> {
			for (const auto &Create : Publication.MeshCreates) if (Create.Mesh == Mesh) return Create.Indices->size();
			if (!Publication.FullResync) if (const auto Existing = Meshes.find(Mesh); Existing != Meshes.end() &&
				!RemovedMeshes.contains(Mesh)) return Existing->second.IndexCount;
			return std::nullopt;
		};
		const auto PrimitiveRangesFit = [&](const auto &Object) {
			if (!Object.Primitives) return true;
			if (!Object.Mesh) return false;
			const auto Count = MeshIndexCountAfterPublication(*Object.Mesh);
			return Count && std::ranges::all_of(*Object.Primitives, [&](const auto &Primitive) {
				return Primitive.FirstIndex <= *Count && Primitive.IndexCount <= *Count - Primitive.FirstIndex;
			});
		};
		const auto ObjectMeshAfterPublication = [&](ObjectId Object) -> std::optional<RenderMeshIdentity> {
			for (const auto &Create : Publication.Creates)
				if (Create.Item.Object == Object) return Create.Mesh;
			for (const auto &Update : Publication.Updates)
				if (Update.Object == Object) return Update.Mesh;
			for (const auto &Remove : Publication.Removes)
				if (Remove.Object == Object) return std::nullopt;
			if (!Publication.FullResync) {
				const auto Existing = Entries.find(Object);
				if (Existing != Entries.end()) return Existing->second.Mesh;
			}
			return std::nullopt;
		};
		const auto SkeletonAfterPublication = [&](RenderMeshIdentity Mesh) -> std::optional<RenderSkeletonIdentity> {
			for (const auto &Create : Publication.MeshCreates)
				if (Create.Mesh == Mesh && Create.SkinInfluences) return Create.Skeleton;
			if (!Publication.FullResync) if (const auto Existing = Meshes.find(Mesh);
				Existing != Meshes.end() && Existing->second.SkinInfluences) return Existing->second.Skeleton;
			return std::nullopt;
		};
		const auto SkeletonJointCountAfterPublication = [&](RenderMeshIdentity Mesh) -> std::uint32_t {
			for (const auto &Create : Publication.MeshCreates)
				if (Create.Mesh == Mesh) return Create.SkeletonJointCount;
			if (!Publication.FullResync) if (const auto Existing = Meshes.find(Mesh); Existing != Meshes.end())
				return Existing->second.SkeletonJointCount;
			return 0;
		};
		TouchedPoseScratch.clear();
		TouchedPoseScratch.reserve(Publication.AnimationPoseUpdates.size() + Publication.AnimationPoseRemoves.size());
		for (const auto &Remove : Publication.AnimationPoseRemoves) {
			if (!Remove.Object.IsValid() ||
				(!Publication.FullResync && !AnimationPoses.contains(Remove.Object)))
				throw std::invalid_argument("Render publication contains an invalid or stale animation pose removal");
			TouchedPoseScratch.push_back(Remove.Object);
		}
		for (const auto &Update : Publication.AnimationPoseUpdates) {
			const auto SourceSkeleton = SkeletonAfterPublication(Update.SourceMesh);
			const auto SourceJointCount = SkeletonJointCountAfterPublication(Update.SourceMesh);
			if (!Update.Object.IsValid() || !Update.SourceMesh.IsValid() || Update.PoseRevision == 0 ||
				!Update.Palette.Skeleton.IsValid() || !Update.Palette.Entries || Update.Palette.Entries->empty() ||
				Update.Palette.Entries->size() > MaximumRenderSkinPaletteEntries ||
				!MeshExistsAfterPublication(Update.SourceMesh) ||
				!SourceSkeleton || *SourceSkeleton != Update.Palette.Skeleton ||
				SourceJointCount != Update.Palette.Entries->size())
				throw std::invalid_argument("Render publication animation pose update is invalid or incoherent");
			TouchedPoseScratch.push_back(Update.Object);
			if (Update.Mode == RenderAnimationSkinningMode::GpuPalette) {
				if (Update.PosedMesh.IsValid() ||
					ObjectMeshAfterPublication(Update.Object) != std::optional(Update.SourceMesh))
					throw std::invalid_argument("Render publication GPU pose must retain the source mesh binding");
			} else if (!Update.PosedMesh.IsValid() || Update.SourceMesh == Update.PosedMesh ||
				!MeshExistsAfterPublication(Update.PosedMesh) ||
				ObjectMeshAfterPublication(Update.Object) != std::optional(Update.PosedMesh)) {
				throw std::invalid_argument("Render publication CPU fallback pose must bind its posed mesh");
			}
			for (const auto &Entry : *Update.Palette.Entries)
				if (!IsFinite(Entry.PositionMatrix) || !IsFinite(Entry.NormalMatrix))
					throw std::invalid_argument("Render publication rejects a non-finite animation pose palette");
			// Source influences were validated against SkeletonJointCount at mesh
			// creation, and the palette count is required to match that exact count.
			// Re-walking shared source vertices here would multiply validation cost by
			// the number of rigs without strengthening the shader bounds guarantee.
			if (!Publication.FullResync) if (const auto Existing = AnimationPoses.find(Update.Object);
				Existing != AnimationPoses.end() && Update.PoseRevision <= Existing->second.PoseRevision)
				throw std::invalid_argument("Render publication rejects a stale animation pose update");
		}
		std::ranges::sort(TouchedPoseScratch);
		if (std::ranges::adjacent_find(TouchedPoseScratch) != TouchedPoseScratch.end())
			throw std::invalid_argument("Render publication contains duplicate animation pose operations");
		for (const auto &Create : Publication.Creates)
			if ((Create.Mesh && !MeshExistsAfterPublication(*Create.Mesh)) || !PrimitiveRangesFit(Create))
				throw std::invalid_argument("Render publication object creation references a missing mesh");
		for (const auto &Update : Publication.Updates)
			if ((Update.Mesh && !MeshExistsAfterPublication(*Update.Mesh)) || !PrimitiveRangesFit(Update))
				throw std::invalid_argument("Render publication object update references a missing mesh");

		if (!Publication.FullResync && !Publication.MeshRemoves.empty()) {
			for (const auto &[Object, Existing] : Entries) {
				if (!Existing.Mesh || TouchedObjects.contains(Object)) continue;
				if (!MeshExistsAfterPublication(*Existing.Mesh))
					throw std::invalid_argument("Render publication removes a mesh that remains referenced");
			}
			for (const auto &[Object, Pose] : AnimationPoses) {
				if (std::ranges::binary_search(TouchedPoseScratch, Object)) continue;
				if (!MeshExistsAfterPublication(Pose.SourceMesh) ||
					(Pose.Mode == RenderAnimationSkinningMode::CpuFallback &&
						!MeshExistsAfterPublication(Pose.PosedMesh)))
					throw std::invalid_argument("Render publication removes a mesh that remains referenced by an animation pose");
			}
		}

		if (!Publication.FullResync) {
			for (const auto &Remove : Publication.Removes) if (!Entries.contains(Remove.Object))
				throw std::invalid_argument("Render publication rejects a stale object removal");
			for (const auto &Create : Publication.Creates) if (Entries.contains(Create.Item.Object))
				throw std::invalid_argument("Render publication rejects a duplicate object creation");
			for (const auto &Update : Publication.Updates) if (!Entries.contains(Update.Object))
				throw std::invalid_argument("Render publication rejects a stale object update");
			for (const auto &Remove : Publication.MeshRemoves) if (!Meshes.contains(Remove.Mesh))
				throw std::invalid_argument("Render publication rejects a stale mesh removal");
			for (const auto &Create : Publication.MeshCreates) if (Meshes.contains(Create.Mesh))
				throw std::invalid_argument("Render publication rejects a duplicate mesh creation");
			for (const auto &Update : Publication.MeshVertexUpdates) {
				auto Existing = Meshes.find(Update.Mesh);
				if (Existing == Meshes.end() || Update.VertexRevision <= Existing->second.VertexRevision ||
					static_cast<std::size_t>(Update.FirstVertex) > Existing->second.VertexCount ||
					Update.Vertices->size() > Existing->second.VertexCount - Update.FirstVertex)
					throw std::invalid_argument("Render publication rejects a stale or out-of-range mesh update");
			}
			for (const auto &Remove : Publication.TextureRemoves) if (!Textures.contains(Remove.Texture))
				throw std::invalid_argument("Render publication rejects a stale texture removal");
			for (const auto &Create : Publication.TextureCreates) if (Textures.contains(Create.Texture))
				throw std::invalid_argument("Render publication rejects a duplicate texture creation");
			for (const auto &Update : Publication.TextureUpdates) {
				const auto Existing = Textures.find(Update.Texture);
				if (Existing == Textures.end() || Update.Revision <= Existing->second.Revision ||
					Update.X > Existing->second.Width || Update.Y > Existing->second.Height ||
					Update.Width > Existing->second.Width - Update.X ||
					Update.Height > Existing->second.Height - Update.Y)
					throw std::invalid_argument("Render publication rejects a stale or out-of-range texture update");
			}
		}

		RenderProjectionChanges Changes;
		if (Publication.FullResync) {
			Changes.Removed = Entries.size();
			Changes.MeshesRemoved = Meshes.size();
			Changes.TexturesRemoved = Textures.size();
			Changes.AnimationPosesRemoved = AnimationPoses.size();
			Entries.clear();
			Meshes.clear();
			Textures.clear();
			AnimationPoses.clear();
		}
		for (const auto &Remove : Publication.Removes) { Entries.erase(Remove.Object); ++Changes.Removed; }
		for (const auto &Remove : Publication.MeshRemoves) { Meshes.erase(Remove.Mesh); ++Changes.MeshesRemoved; }
		Entries.reserve(Entries.size() + Publication.Creates.size());
		for (const auto &Create : Publication.Creates) {
			Entries.emplace(Create.Item.Object, RenderProjectedObject{
				Create.Item, Create.Mesh, Create.Material, Create.Visible, Create.Primitives
			});
			++Changes.Created;
		}
		for (const auto &Update : Publication.Updates) {
			auto &EntryValue = Entries.at(Update.Object);
			EntryValue = RenderProjectedObject{
				Update.Item, Update.Mesh, Update.Material, Update.Visible, Update.Primitives
			};
			++Changes.Updated;
		}
		Meshes.reserve(Meshes.size() + Publication.MeshCreates.size());
		for (const auto &Create : Publication.MeshCreates) {
			Meshes.emplace(Create.Mesh, MeshEntry{Create.TopologyRevision, Create.VertexRevision,
				Create.Vertices->size(), Create.Indices->size(), Create.Bounds, Create.SkinInfluences,
				Create.Skeleton, Create.SkeletonJointCount});
			++Changes.MeshesCreated;
			Changes.VertexUploadBytes += Create.Vertices->size() * sizeof(RenderVertex) + Create.Indices->size() * sizeof(std::uint32_t);
		}
		for (const auto &Update : Publication.MeshVertexUpdates) {
			auto &Existing = Meshes.at(Update.Mesh);
			Existing.VertexRevision = Update.VertexRevision;
			Existing.Bounds = Update.Bounds;
			++Changes.MeshesUpdated;
			Changes.VertexUploadBytes += Update.Vertices->size() * sizeof(RenderVertex);
		}
		for (const auto &Remove : Publication.AnimationPoseRemoves) {
			AnimationPoses.erase(Remove.Object);
			++Changes.AnimationPosesRemoved;
		}
		AnimationPoses.reserve(AnimationPoses.size() + Publication.AnimationPoseUpdates.size());
		for (const auto &Update : Publication.AnimationPoseUpdates) {
			AnimationPoses.insert_or_assign(Update.Object, Update);
			++Changes.AnimationPosesUpdated;
			Changes.PaletteUploadBytes += Update.Palette.Entries->size() * sizeof(RenderSkinPaletteEntry);
		}
		for (const auto &Remove : Publication.TextureRemoves) {
			Textures.erase(Remove.Texture);
			++Changes.TexturesRemoved;
		}
		Textures.reserve(Textures.size() + Publication.TextureCreates.size());
		for (const auto &Create : Publication.TextureCreates) {
			Textures.emplace(Create.Texture, TextureEntry{Create.Revision, Create.Width, Create.Height, Create.Format});
			++Changes.TexturesCreated;
			Changes.TextureUploadBytes += Create.Pixels->size();
		}
		for (const auto &Update : Publication.TextureUpdates) {
			auto &Existing = Textures.at(Update.Texture);
			Existing.Revision = Update.Revision;
			++Changes.TexturesUpdated;
			Changes.TextureUploadBytes += Update.Pixels->size();
		}
		if (HasUiUpdate) {
			Changes.UiBatches = PublishedUi.Batches.size();
			for (const auto &Batch : PublishedUi.Batches) {
				Changes.UiVertices += Batch.Vertices.size();
				Changes.UiIndices += Batch.Indices.size();
			}
			Ui = Publication.SharedUi ? Publication.SharedUi : std::make_shared<const RenderUiFrame>(PublishedUi);
		}
		if (Publication.FullResync || Publication.EnvironmentChanged) ++Changes.EnvironmentsUpdated;
		Frame = Publication.Frame;
		LastPublicationId = Publication.Id;
		return Changes;
	}

	RenderProjectionChanges RenderProjection::Apply(const RenderSnapshot &Snapshot) {
		if (Snapshot.Id == InvalidRenderSnapshotId)
			throw std::invalid_argument("Render projection requires a valid RenderSnapshot identity");

		std::unordered_set<ObjectId> Seen;
		Seen.reserve(Snapshot.Items.size());
		for (const auto &Item : Snapshot.Items) {
			if (!Item.Object.IsValid())
				throw std::invalid_argument("Render projection requires valid ObjectId values");
			if (!Seen.insert(Item.Object).second)
				throw std::invalid_argument("Render projection rejects duplicate ObjectId values");
		}

		RenderProjectionChanges Changes;
		Entries.reserve(Snapshot.Items.size());
		for (const auto &Item : Snapshot.Items) {
			auto Existing = Entries.find(Item.Object);
			if (Existing == Entries.end()) {
				Entries.emplace(Item.Object, RenderProjectedObject{Item});
				++Changes.Created;
				continue;
			}

			if (ItemsEqual(Existing->second.Item, Item)) {
				++Changes.Unchanged;
			} else {
				Existing->second.Item = Item;
				++Changes.Updated;
			}
		}

		for (auto Existing = Entries.begin(); Existing != Entries.end();) {
			if (Seen.contains(Existing->first)) {
				++Existing;
				continue;
			}
			Existing = Entries.erase(Existing);
			++Changes.Removed;
		}
		Frame.ViewportWidth = Snapshot.ViewportWidth;
		Frame.ViewportHeight = Snapshot.ViewportHeight;
		Frame.Camera = Snapshot.Camera;
		Frame.Environment = Snapshot.Environment;
		++Changes.EnvironmentsUpdated;
		Ui = std::make_shared<const RenderUiFrame>();
		return Changes;
	}

	void RenderProjection::Clear() {
		Entries.clear();
		Meshes.clear();
		Textures.clear();
		AnimationPoses.clear();
		LastPublicationId = InvalidRenderPublicationId;
		Frame = {};
		Ui = std::make_shared<const RenderUiFrame>();
	}

	const RenderItem *RenderProjection::GetItem(ObjectId Object) const {
		const auto Existing = Entries.find(Object);
		return Existing == Entries.end() ? nullptr : &Existing->second.Item;
	}

	const RenderAnimationPoseUpdate *RenderProjection::GetAnimationPose(ObjectId Object) const {
		const auto Existing = AnimationPoses.find(Object);
		return Existing == AnimationPoses.end() ? nullptr : &Existing->second;
	}

	bool RenderProjection::BuildAnimationVisibilityFeedback(
		std::vector<ObjectId> &VisibleObjects, std::size_t MaximumObjects
	) const {
		VisibleObjects.clear();
		if (AnimationPoses.size() > MaximumObjects) return false;
		for (const auto &[Object, Pose] : AnimationPoses) {
			const auto Projected = Entries.find(Object);
			if (Projected == Entries.end() || !Projected->second.Visible || !Projected->second.Mesh) continue;
			const auto Mesh = Meshes.find(*Projected->second.Mesh);
			if (Mesh == Meshes.end()) {
				VisibleObjects.push_back(Object);
				continue;
			}
			bool AllLeft = true;
			bool AllRight = true;
			bool AllBottom = true;
			bool AllTop = true;
			bool AllNear = true;
			bool AllFar = true;
			bool Finite = true;
			auto IncludeCorner = [&](const glm::mat4 &PoseMatrix, const glm::vec3 &Corner) {
				const auto Clip = Frame.Camera.ViewProjectionMatrix * Projected->second.Item.ModelMatrix * PoseMatrix *
								  glm::vec4(Corner, 1.0f);
				if (!IsFinite(Clip)) {
					Finite = false;
					return;
				}
				AllLeft = AllLeft && Clip.x < -Clip.w;
				AllRight = AllRight && Clip.x > Clip.w;
				AllBottom = AllBottom && Clip.y < -Clip.w;
				AllTop = AllTop && Clip.y > Clip.w;
				AllNear = AllNear && Clip.z < -Clip.w;
				AllFar = AllFar && Clip.z > Clip.w;
			};
			auto IncludeBounds = [&](const glm::mat4 &PoseMatrix) {
				for (std::uint32_t CornerIndex = 0; CornerIndex < 8; ++CornerIndex) {
					const glm::vec3 Corner{
						(CornerIndex & 1u) ? Mesh->second.Bounds.Maximum.x : Mesh->second.Bounds.Minimum.x,
						(CornerIndex & 2u) ? Mesh->second.Bounds.Maximum.y : Mesh->second.Bounds.Minimum.y,
						(CornerIndex & 4u) ? Mesh->second.Bounds.Maximum.z : Mesh->second.Bounds.Minimum.z,
					};
					IncludeCorner(PoseMatrix, Corner);
				}
			};
			if (Pose.Mode == RenderAnimationSkinningMode::GpuPalette && Pose.Palette.Entries)
				for (const auto &Entry : *Pose.Palette.Entries)
					IncludeBounds(Entry.PositionMatrix);
			else
				IncludeBounds(glm::mat4(1.0f));
			if (!Finite || !(AllLeft || AllRight || AllBottom || AllTop || AllNear || AllFar))
				VisibleObjects.push_back(Object);
		}
		std::ranges::sort(VisibleObjects);
		return true;
	}

	RenderSnapshotPtr RenderProjection::BuildCompatibilitySnapshot() const {
		auto Snapshot = std::make_shared<RenderSnapshot>();
		Snapshot->Id = LastPublicationId;
		Snapshot->ViewportWidth = Frame.ViewportWidth;
		Snapshot->ViewportHeight = Frame.ViewportHeight;
		Snapshot->Camera = Frame.Camera;
		Snapshot->Environment = Frame.Environment;
		std::vector<const RenderProjectedObject *> Ordered;
		Ordered.reserve(Entries.size());
		for (const auto &[Object, EntryValue] : Entries) {
			(void)Object;
			if (EntryValue.Visible) Ordered.push_back(&EntryValue);
		}
		std::ranges::sort(Ordered, {}, [](const RenderProjectedObject *EntryValue) { return EntryValue->Item.Object; });
		Snapshot->Items.reserve(Ordered.size());
		for (const auto *EntryValue : Ordered) Snapshot->Items.push_back(EntryValue->Item);
		return std::shared_ptr<const RenderSnapshot>(std::move(Snapshot));
	}
}

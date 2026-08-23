// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderProjection.hpp"

#include <algorithm>
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
		bool IsValid(const RenderVertex &Vertex) {
			return IsFinite(Vertex.Position) && IsFinite(Vertex.Normal) && IsFinite(Vertex.Tangent) &&
				IsFinite(Vertex.TextureCoordinate);
		}
		bool IsValid(const RenderFrameState &Frame) {
			const auto &Camera = Frame.Camera;
			return Frame.ViewportWidth != 0 && Frame.ViewportHeight != 0 && std::isfinite(Frame.DpiScale) &&
				Frame.DpiScale > 0.0f && IsFinite(Frame.LightDirection) &&
				(Frame.LightDirection.x != 0.0f || Frame.LightDirection.y != 0.0f || Frame.LightDirection.z != 0.0f) &&
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
			!Publication.TextureUpdates.empty() || !Publication.TextureRemoves.empty()))
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
			if (!IsValid(Create.Item) || !IsValid(Create.Material) || (Create.Mesh && !Create.Mesh->IsValid()) ||
				!TouchedObjects.insert(Create.Item.Object).second)
				throw std::invalid_argument("Render publication contains an invalid or duplicate object operation");
		for (const auto &Update : Publication.Updates) {
			constexpr auto ValidUpdateDomains = static_cast<std::uint32_t>(RenderUpdateDomain::Transform) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Material) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Visibility) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Geometry) |
				static_cast<std::uint32_t>(RenderUpdateDomain::DeformableVertices) |
				static_cast<std::uint32_t>(RenderUpdateDomain::Hierarchy);
			if (!Update.Object.IsValid() || Update.Item.Object != Update.Object || !IsValid(Update.Item) ||
				!IsValid(Update.Material) || (Update.Mesh && !Update.Mesh->IsValid()) ||
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
		const auto MaterialTexturesExist = [&](const RenderMaterialState &Material) {
			return (!Material.BaseColorTexture || TextureExistsAfterPublication(*Material.BaseColorTexture)) &&
				(!Material.NormalTexture || TextureExistsAfterPublication(*Material.NormalTexture));
		};
		for (const auto &Create : Publication.Creates) if (!MaterialTexturesExist(Create.Material))
			throw std::invalid_argument("Render publication material references a texture without residency");
		for (const auto &Update : Publication.Updates) if (!MaterialTexturesExist(Update.Material))
			throw std::invalid_argument("Render publication material references a texture without residency");
		if (!Publication.FullResync && !Publication.TextureRemoves.empty()) {
			for (const auto &[Object, Existing] : Entries) {
				if (TouchedObjects.contains(Object)) continue;
				if (!MaterialTexturesExist(Existing.Material))
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
		for (const auto &Create : Publication.Creates)
			if (Create.Mesh && !MeshExistsAfterPublication(*Create.Mesh))
				throw std::invalid_argument("Render publication object creation references a missing mesh");
		for (const auto &Update : Publication.Updates)
			if (Update.Mesh && !MeshExistsAfterPublication(*Update.Mesh))
				throw std::invalid_argument("Render publication object update references a missing mesh");

		if (!Publication.FullResync && !Publication.MeshRemoves.empty()) {
			for (const auto &[Object, Existing] : Entries) {
				if (!Existing.Mesh || TouchedObjects.contains(Object)) continue;
				if (!MeshExistsAfterPublication(*Existing.Mesh))
					throw std::invalid_argument("Render publication removes a mesh that remains referenced");
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
			Entries.clear();
			Meshes.clear();
			Textures.clear();
		}
		for (const auto &Remove : Publication.Removes) { Entries.erase(Remove.Object); ++Changes.Removed; }
		for (const auto &Remove : Publication.MeshRemoves) { Meshes.erase(Remove.Mesh); ++Changes.MeshesRemoved; }
		Entries.reserve(Entries.size() + Publication.Creates.size());
		for (const auto &Create : Publication.Creates) {
			Entries.emplace(Create.Item.Object, RenderProjectedObject{Create.Item, Create.Mesh, Create.Material, Create.Visible});
			++Changes.Created;
		}
		for (const auto &Update : Publication.Updates) {
			auto &EntryValue = Entries.at(Update.Object);
			EntryValue = RenderProjectedObject{Update.Item, Update.Mesh, Update.Material, Update.Visible};
			++Changes.Updated;
		}
		Meshes.reserve(Meshes.size() + Publication.MeshCreates.size());
		for (const auto &Create : Publication.MeshCreates) {
			Meshes.emplace(Create.Mesh, MeshEntry{Create.TopologyRevision, Create.VertexRevision, Create.Vertices->size(), Create.Indices->size(), Create.Bounds});
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
		Frame.LightDirection = Snapshot.LightDirection;
		Ui = std::make_shared<const RenderUiFrame>();
		return Changes;
	}

	void RenderProjection::Clear() {
		Entries.clear();
		Meshes.clear();
		Textures.clear();
		LastPublicationId = InvalidRenderPublicationId;
		Frame = {};
		Ui = std::make_shared<const RenderUiFrame>();
	}

	const RenderItem *RenderProjection::GetItem(ObjectId Object) const {
		const auto Existing = Entries.find(Object);
		return Existing == Entries.end() ? nullptr : &Existing->second.Item;
	}

	RenderSnapshotPtr RenderProjection::BuildCompatibilitySnapshot() const {
		auto Snapshot = std::make_shared<RenderSnapshot>();
		Snapshot->Id = LastPublicationId;
		Snapshot->ViewportWidth = Frame.ViewportWidth;
		Snapshot->ViewportHeight = Frame.ViewportHeight;
		Snapshot->Camera = Frame.Camera;
		Snapshot->LightDirection = Frame.LightDirection;
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

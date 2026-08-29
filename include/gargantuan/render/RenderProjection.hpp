// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"
#include "gargantuan/render/RenderPublication.hpp"

#include <cstddef>
#include <unordered_map>

namespace gargantuan {
	struct RenderProjectedObject {
		RenderItem Item;
		std::optional<RenderMeshIdentity> Mesh;
		RenderMaterialState Material;
		bool Visible = true;
		std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
	};

	struct RenderProjectionChanges {
		std::size_t Created = 0;
		std::size_t Updated = 0;
		std::size_t Removed = 0;
		std::size_t Unchanged = 0;
		std::size_t MeshesCreated = 0;
		std::size_t MeshesUpdated = 0;
		std::size_t MeshesRemoved = 0;
		std::size_t TexturesCreated = 0;
		std::size_t TexturesUpdated = 0;
		std::size_t TexturesRemoved = 0;
		std::size_t VertexUploadBytes = 0;
		std::size_t TextureUploadBytes = 0;
		std::size_t PaletteUploadBytes = 0;
		std::size_t AnimationPosesUpdated = 0;
		std::size_t AnimationPosesRemoved = 0;
		std::size_t EnvironmentsUpdated = 0;
		std::size_t UiBatches = 0;
		std::size_t UiVertices = 0;
		std::size_t UiIndices = 0;
	};

	class RenderProjection final {
	  public:
		[[nodiscard]] RenderProjectionChanges Apply(const RenderSnapshot &Snapshot);
		[[nodiscard]] RenderProjectionChanges Apply(const RenderPublication &Publication);
		void Clear();

		[[nodiscard]] const RenderItem *GetItem(ObjectId Object) const;
		[[nodiscard]] const RenderAnimationPoseUpdate *GetAnimationPose(ObjectId Object) const;
		[[nodiscard]] const std::unordered_map<ObjectId, RenderAnimationPoseUpdate> &GetAnimationPoses() const {
			return AnimationPoses;
		}
		[[nodiscard]] std::size_t GetSize() const { return Entries.size(); }
		[[nodiscard]] std::size_t GetMeshCount() const { return Meshes.size(); }
		[[nodiscard]] std::size_t GetTextureCount() const { return Textures.size(); }
		[[nodiscard]] RenderPublicationId GetLastPublicationId() const { return LastPublicationId; }
		[[nodiscard]] const RenderFrameState &GetFrame() const { return Frame; }
		[[nodiscard]] const RenderUiFrame &GetUi() const { return *Ui; }
		[[nodiscard]] const std::unordered_map<ObjectId, RenderProjectedObject> &GetObjects() const { return Entries; }
		[[nodiscard]] RenderSnapshotPtr BuildCompatibilitySnapshot() const;

	  private:
		std::unordered_map<ObjectId, RenderProjectedObject> Entries;
		struct MeshEntry {
			std::uint64_t TopologyRevision = 0;
			std::uint64_t VertexRevision = 0;
			std::size_t VertexCount = 0;
			std::size_t IndexCount = 0;
			RenderBounds Bounds;
			std::shared_ptr<const std::vector<RenderSkinInfluence>> SkinInfluences;
			RenderSkeletonIdentity Skeleton;
			std::uint32_t SkeletonJointCount = 0;
		};
		std::unordered_map<RenderMeshIdentity, MeshEntry, RenderMeshIdentityHash> Meshes;
		struct TextureEntry {
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			RenderTextureFormat Format = RenderTextureFormat::Rgba8Unorm;
		};
		std::unordered_map<RenderTextureIdentity, TextureEntry, RenderTextureIdentityHash> Textures;
		std::unordered_map<ObjectId, RenderAnimationPoseUpdate> AnimationPoses;
		// Validation scratch is retained so a steady pose-only publication does not
		// allocate merely to detect duplicate pose operations.
		std::vector<ObjectId> TouchedPoseScratch;
		RenderPublicationId LastPublicationId = InvalidRenderPublicationId;
		RenderFrameState Frame;
		std::shared_ptr<const RenderUiFrame> Ui = std::make_shared<const RenderUiFrame>();
	};
} // namespace gargantuan

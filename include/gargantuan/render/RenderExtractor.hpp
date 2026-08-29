// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderDirtyAccumulator.hpp"
#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>

#include <glm/glm.hpp>

namespace gargantuan {
	class Camera;
	class WorldRoot;

	struct RenderCameraInput {
		glm::vec3 Position{0.0f};
		glm::vec3 LookDirection{0.0f, 0.0f, -1.0f};
		glm::vec3 UpDirection{0.0f, 1.0f, 0.0f};
		float VerticalFieldOfView = 70.0f;
		float NearPlane = 0.1f;
		float FarPlane = 100000.0f;
	};

	[[nodiscard]] RenderCameraInput MakeRenderCameraInput(const Camera &camera);
	[[nodiscard]] RenderCameraInput MakeLookAtRenderCameraInput(
		glm::vec3 position,
		glm::vec3 target,
		glm::vec3 up,
		float verticalFieldOfView = 70.0f
	);

	class RenderExtractor {
	  public:
		[[nodiscard]] RenderSnapshotPtr Extract(
			const WorldRoot &world,
			const RenderCameraInput &camera,
			std::uint32_t viewportWidth,
			std::uint32_t viewportHeight
		);

		[[nodiscard]] RenderSnapshotId GetLastSnapshotId() const { return LastSnapshotId; }

	  private:
		RenderSnapshotId LastSnapshotId = InvalidRenderSnapshotId;
	};

	struct RenderPublisherProfile {
		std::uint64_t DirtyCaptureNanoseconds = 0;
		std::uint64_t DirtyExpansionNanoseconds = 0;
		std::uint64_t FinalStateExtractionNanoseconds = 0;
		std::uint64_t PublicationConstructionNanoseconds = 0;
		std::uint64_t StateCacheReconciliationNanoseconds = 0;
	};

	class RenderPublisher final {
	  public:
		explicit RenderPublisher(RenderDirtyAccumulator *DirtyAccumulator = nullptr);
		~RenderPublisher();
		RenderPublisher(const RenderPublisher &) = delete;
		RenderPublisher &operator=(const RenderPublisher &) = delete;
		[[nodiscard]] RenderPublicationPtr Publish(
			const WorldRoot &World,
			const RenderCameraInput &Camera,
			std::uint32_t ViewportWidth,
			std::uint32_t ViewportHeight
		);
		void RequestFullResync() { FullResyncRequested = true; }
		void SetUiFrame(RenderUiFrame UiFrame, ObjectId Source = {}, std::uint64_t SourceGeneration = 0);
		void SetUiFrame(
			std::shared_ptr<const RenderUiFrame> UiFrame,
			ObjectId Source = {},
			std::uint64_t SourceGeneration = 0
		);
		[[nodiscard]] bool HasUiFrame(ObjectId Source, std::uint64_t SourceGeneration) const {
			return UiSource == Source && UiSourceGeneration == SourceGeneration;
		}
		void SetUiTextureChanges(
			std::vector<RenderTextureCreate> Creates,
			std::vector<RenderTextureUpdate> Updates,
			std::vector<RenderTextureRemove> Removes
		);
		void SetAssetMeshChanges(std::vector<RenderMeshCreate> Creates, std::vector<RenderMeshRemove> Removes);
		void SetAnimationPoseChanges(
			std::span<const RenderAnimationPoseState> Updates,
			std::span<const RenderAnimationPoseRemove> Removes
		);
		[[nodiscard]] RenderPublicationId GetLastPublicationId() const { return LastPublicationId; }
		[[nodiscard]] std::size_t GetPublishedObjectCount() const { return PublishedItems.size(); }
		[[nodiscard]] std::size_t GetFullResyncCount() const { return FullResyncCount; }
		void SetProfilingEnabled(bool Enabled) { ProfilingEnabled = Enabled; }
		[[nodiscard]] RenderPublisherProfile GetLastProfile() const { return LastProfile; }

	  private:
		struct PublishedDeformable {
			RenderMeshIdentity Mesh;
			std::uint64_t TopologyRevision = 0;
			std::uint64_t VertexRevision = 0;
		};
		struct PublishedTexture {
			std::uint64_t Revision = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			RenderTextureFormat Format = RenderTextureFormat::Rgba8Unorm;
			std::shared_ptr<const std::vector<std::uint8_t>> Pixels;
		};
		struct PublishedAssetMesh {
			std::uint64_t TopologyRevision = 0;
			std::uint64_t VertexRevision = 0;
			std::shared_ptr<const std::vector<RenderVertex>> Vertices;
			std::shared_ptr<const std::vector<std::uint32_t>> Indices;
			RenderBounds Bounds;
			std::shared_ptr<const std::vector<RenderSkinInfluence>> SkinInfluences;
			RenderSkeletonIdentity Skeleton;
			std::uint32_t SkeletonJointCount = 0;
		};
		struct PublishedSky {
			ObjectId Source;
			RenderSkyState State;
		};

		[[nodiscard]] RenderEnvironmentState BuildEnvironmentState(
			const WorldRoot &World,
			std::vector<RenderExtractionDiagnostic> &Diagnostics
		);

		RenderExtractor FullExtractor;
		RenderDirtyAccumulator *Dirty = nullptr;
		RenderDirtyConsumerId DirtyConsumer = InvalidRenderDirtyConsumerId;
		RenderPublicationId LastPublicationId = InvalidRenderPublicationId;
		ObjectId Scope;
		const WorldRoot *PublishedWorld = nullptr;
		std::unordered_map<ObjectId, RenderItem> PublishedItems;
		std::unordered_map<ObjectId, PublishedDeformable> PublishedDeformables;
		std::unordered_map<RenderTextureIdentity, PublishedTexture, RenderTextureIdentityHash> PublishedTextures;
		std::unordered_map<RenderMeshIdentity, PublishedAssetMesh, RenderMeshIdentityHash> PublishedAssetMeshes;
		std::unordered_map<ObjectId, RenderAnimationPoseState> AnimationPoses;
		std::unordered_map<ObjectId, RenderAnimationPoseState> CommittedAnimationPoses;
		std::vector<ObjectId> PendingAnimationObjects;
		RenderEnvironmentState PublishedEnvironment;
		std::optional<PublishedSky> LastKnownGoodSky;
		bool HasPublishedEnvironment = false;
		std::shared_ptr<const RenderUiFrame> PendingUi;
		std::shared_ptr<const RenderUiFrame> CommittedUi = std::make_shared<const RenderUiFrame>();
		ObjectId UiSource;
		std::uint64_t UiSourceGeneration = 0;
		std::vector<RenderTextureCreate> PendingTextureCreates;
		std::vector<RenderTextureUpdate> PendingTextureUpdates;
		std::vector<RenderTextureRemove> PendingTextureRemoves;
		std::vector<RenderMeshCreate> PendingAssetMeshCreates;
		std::vector<RenderMeshRemove> PendingAssetMeshRemoves;
		std::size_t PendingUiGeometryBytes = 0;
		std::size_t PendingTextureBytes = 0;
		std::size_t FullResyncCount = 0;
		bool FullResyncRequested = true;
		bool ProfilingEnabled = false;
		RenderPublisherProfile LastProfile;
	};
}

// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <glm/gtc/quaternion.hpp>

namespace gargantuan {
	enum class AssetKind : std::uint8_t { Image, Mesh, Font, Material, Audio, Animation };
	enum class AssetState : std::uint8_t { Missing, Importing, Ready, Failed, Stale };
	enum class AssetMaterialAlphaMode : std::uint8_t { Opaque, Mask, Blend };
	enum class AssetAnimationInterpolation : std::uint8_t { Step, Linear };
	enum class AssetChangeKind : std::uint8_t {
		Added,
		Removed,
		ContentChanged,
		MetadataChanged,
		DependencyChanged,
		StateChanged,
	};

	[[nodiscard]] std::string_view GetAssetKindName(AssetKind Kind);
	[[nodiscard]] std::optional<AssetKind> ParseAssetKind(std::string_view Value);
	[[nodiscard]] std::string_view GetAssetStateName(AssetState State);
	[[nodiscard]] std::optional<AssetState> ParseAssetState(std::string_view Value);

	struct AssetId {
		std::uint64_t High = 0;
		std::uint64_t Low = 0;

		[[nodiscard]] bool IsValid() const { return High != 0 || Low != 0; }
		[[nodiscard]] std::string ToString() const;
		[[nodiscard]] static std::optional<AssetId> Parse(std::string_view Value);
		[[nodiscard]] static AssetId New();
		[[nodiscard]] static AssetId FromBuiltInName(std::string_view Name);
		auto operator<=>(const AssetId &) const = default;
	};

	struct AssetIdHash {
		[[nodiscard]] std::size_t operator()(AssetId Value) const noexcept {
			return std::hash<std::uint64_t>{}(Value.High) ^ (std::hash<std::uint64_t>{}(Value.Low) << 1);
		}
	};

	struct AssetContentId {
		std::array<std::uint8_t, 32> Bytes{};

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] std::string ToString() const;
		[[nodiscard]] static std::optional<AssetContentId> Parse(std::string_view Value);
		[[nodiscard]] static AssetContentId Hash(std::span<const std::uint8_t> Bytes);
		auto operator<=>(const AssetContentId &) const = default;
	};

	struct AssetReference {
		std::string Value;
		std::optional<AssetId> ProjectAsset;
		bool BuiltIn = false;

		[[nodiscard]] bool IsValid() const { return ProjectAsset.has_value() || BuiltIn; }
		[[nodiscard]] static std::optional<AssetReference> Parse(std::string_view Value);
		[[nodiscard]] static AssetReference FromAssetId(AssetId Id);
	};

	struct ImportedImage {
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::shared_ptr<const std::vector<std::uint8_t>> Rgba8;
	};

	struct ImportedMeshPrimitive {
		std::uint32_t FirstIndex = 0;
		std::uint32_t IndexCount = 0;
		std::optional<AssetId> Material;
	};

	struct ImportedSkeletonJoint {
		std::string Path;
		std::int32_t Parent = -1;
		glm::vec3 BindTranslation{0.0f};
		glm::quat BindRotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 BindScale{1.0f};
		glm::mat4 InverseBindMatrix{1.0f};
	};

	struct ImportedSkeleton {
		AssetContentId CompatibilityId;
		std::shared_ptr<const std::vector<ImportedSkeletonJoint>> Joints;
	};

	struct ImportedSkinInfluence {
		std::array<std::uint16_t, 4> Joints{};
		glm::vec4 Weights{0.0f};
	};

	struct ImportedMesh {
		std::shared_ptr<const std::vector<RenderVertex>> Vertices;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
		RenderBounds Bounds;
		std::uint32_t SubmeshCount = 1;
		std::shared_ptr<const std::vector<ImportedMeshPrimitive>> Primitives;
		std::shared_ptr<const ImportedSkeleton> Skeleton;
		std::shared_ptr<const std::vector<ImportedSkinInfluence>> SkinInfluences;
	};

	struct ImportedAnimationVectorKey {
		float Time = 0.0f;
		glm::vec3 Value{0.0f};
	};

	struct ImportedAnimationRotationKey {
		float Time = 0.0f;
		glm::quat Value{1.0f, 0.0f, 0.0f, 0.0f};
	};

	struct ImportedAnimationTrack {
		std::string JointPath;
		AssetAnimationInterpolation TranslationInterpolation = AssetAnimationInterpolation::Linear;
		AssetAnimationInterpolation RotationInterpolation = AssetAnimationInterpolation::Linear;
		AssetAnimationInterpolation ScaleInterpolation = AssetAnimationInterpolation::Linear;
		std::shared_ptr<const std::vector<ImportedAnimationVectorKey>> TranslationKeys;
		std::shared_ptr<const std::vector<ImportedAnimationRotationKey>> RotationKeys;
		std::shared_ptr<const std::vector<ImportedAnimationVectorKey>> ScaleKeys;
	};

	struct ImportedAnimation {
		float Duration = 0.0f;
		AssetContentId SkeletonCompatibilityId;
		std::optional<AssetId> SkeletonAsset;
		std::shared_ptr<const std::vector<ImportedAnimationTrack>> Tracks;
	};

	struct ImportedFont {
		std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
		std::uint32_t FaceCount = 1;
	};

	struct ImportedMaterial {
		glm::vec4 BaseColorFactor{1.0f};
		std::optional<AssetId> BaseColorTexture;
		float MetallicFactor = 1.0f;
		float RoughnessFactor = 1.0f;
		std::optional<AssetId> NormalTexture;
		AssetMaterialAlphaMode AlphaMode = AssetMaterialAlphaMode::Opaque;
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
	};

	struct ImportedAudio {
		std::uint32_t SampleRate = 0;
		std::uint8_t Channels = 0;
		std::uint32_t FrameCount = 0;
		std::shared_ptr<const std::vector<std::int16_t>> Pcm16;
	};

	using ImportedAsset = std::variant<ImportedImage, ImportedMesh, ImportedFont, ImportedMaterial, ImportedAudio,
		ImportedAnimation>;

	struct AssetDiagnostic {
		std::string Code;
		std::string Message;
	};

	struct AssetRecord {
		AssetId Id;
		AssetReference Reference;
		AssetKind Kind = AssetKind::Image;
		std::string Name;
		std::string Source;
		AssetContentId ContentId;
		std::uint64_t ContentRevision = 0;
		AssetState State = AssetState::Missing;
		std::optional<AssetDiagnostic> Diagnostic;
		std::vector<AssetId> Dependencies;
		std::shared_ptr<const ImportedAsset> Asset;
		bool BuiltIn = false;
		AssetId SourceGroupId;
		std::string LogicalKey = "asset";
		bool PrimarySourceAsset = true;
	};

	struct AssetImageResource {
		RenderTextureIdentity Texture;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint64_t ContentRevision = 0;
	};

	struct AssetFontResource {
		std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
		std::uint64_t ContentRevision = 0;
		std::uint32_t FaceCount = 0;
	};

	struct AssetMeshResource {
		RenderMeshIdentity Mesh;
		ImportedMesh Value;
		std::uint64_t ContentRevision = 0;
	};

	struct AssetMaterialResource {
		ImportedMaterial Value;
		RenderMaterialState RenderState;
		std::uint64_t ContentRevision = 0;
	};

	struct AssetAudioResource {
		ImportedAudio Value;
		std::uint64_t ContentRevision = 0;
	};

	struct AssetAnimationResource {
		ImportedAnimation Value;
		std::uint64_t ContentRevision = 0;
	};

	struct AssetTextureChanges {
		std::vector<RenderTextureCreate> Creates;
		std::vector<RenderTextureUpdate> Updates;
		std::vector<RenderTextureRemove> Removes;
		std::size_t UploadBytes = 0;
	};

	struct AssetMeshChanges {
		std::vector<RenderMeshCreate> Creates;
		std::vector<RenderMeshRemove> Removes;
	};

	struct AssetChange {
		std::uint64_t Sequence = 0;
		std::string Reference;
		AssetKind Kind = AssetKind::Image;
		std::uint64_t ContentRevision = 0;
		AssetState State = AssetState::Missing;
		AssetChangeKind ChangeKind = AssetChangeKind::StateChanged;
	};

	struct AssetChangeBatch {
		std::uint64_t NextSequence = 1;
		bool RescanRequired = false;
		std::vector<AssetChange> Changes;
	};

	struct AssetArtifactFile {
		std::string RelativePath;
		std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
	};

	struct AssetProjectSnapshot {
		std::string CatalogJson;
		std::vector<AssetArtifactFile> Artifacts;
	};

	struct AssetRuntimeSnapshot {
		std::string CatalogJson;
		std::vector<AssetArtifactFile> Artifacts;
	};

	struct AssetLimits {
		static constexpr std::size_t MaximumCatalogRecords = 4096;
		static constexpr std::size_t MaximumSourceBytes = 8 * 1024 * 1024;
		static constexpr std::size_t MaximumArtifactBytes = 64 * 1024 * 1024;
		static constexpr std::size_t MaximumCpuCacheBytes = 64 * 1024 * 1024;
		static constexpr std::size_t MaximumNameBytes = 256;
		static constexpr std::size_t MaximumSourcePathBytes = 4096;
		static constexpr std::size_t MaximumDiagnosticBytes = 1024;
		static constexpr std::size_t MaximumImportDiagnostics = 64;
		static constexpr std::size_t MaximumDependencies = 256;
		static constexpr std::size_t MaximumGraphDependencies = 8192;
		static constexpr std::size_t MaximumGeneratedAssets = 1024;
		static constexpr std::size_t MaximumExternalResources = 256;
		static constexpr std::size_t MaximumGltfJsonBytes = 4 * 1024 * 1024;
		static constexpr std::size_t MaximumGltfBuffers = 256;
		static constexpr std::size_t MaximumGltfBufferViews = 4096;
		static constexpr std::size_t MaximumGltfAccessors = 4096;
		static constexpr std::size_t MaximumGltfMeshes = 1024;
		static constexpr std::size_t MaximumGltfPrimitives = 4096;
		static constexpr std::size_t MaximumGltfMaterials = 1024;
		static constexpr std::size_t MaximumGltfImages = 1024;
		static constexpr std::size_t MaximumGltfTextures = 1024;
		static constexpr std::size_t MaximumGltfNodes = 4096;
		static constexpr std::size_t MaximumGltfSkins = 256;
		static constexpr std::size_t MaximumGltfAnimations = 256;
		static constexpr std::size_t MaximumGltfAnimationChannels = 4096;
		static constexpr std::size_t MaximumGltfChunks = 8;
		static constexpr std::size_t MaximumGltfDataUriBytes = 8 * 1024 * 1024;
		static constexpr std::size_t MaximumChangeRecords = 512;
		static constexpr std::size_t MaximumInFlightImports = 4;
		static constexpr std::uint32_t MaximumImageDimension = 1024;
		static constexpr std::size_t MaximumImageBytes = 4 * 1024 * 1024;
		static constexpr std::size_t MaximumMeshVertices = 262144;
		static constexpr std::size_t MaximumMeshIndices = 1048576;
		static constexpr std::size_t MaximumMeshSubmeshes = 256;
		static constexpr std::size_t MaximumSkeletonBones = 256;
		static constexpr std::size_t MaximumJointPathBytes = 256;
		static constexpr std::size_t MaximumAnimationTracks = 256;
		static constexpr std::size_t MaximumAnimationKeyframesPerTrack = 65'536;
		static constexpr std::size_t MaximumAnimationKeyframes = 1'048'576;
		static constexpr float MaximumAnimationDurationSeconds = 3'600.0f;
		static constexpr std::size_t MaximumFontBytes = 8 * 1024 * 1024;
		static constexpr std::uint32_t MinimumAudioSampleRate = 8'000;
		static constexpr std::uint32_t MaximumAudioSampleRate = 48'000;
		static constexpr std::uint8_t MaximumAudioChannels = 2;
		static constexpr std::uint32_t MaximumAudioDurationSeconds = 30;
		static constexpr std::uint32_t MaximumAudioFrames = MaximumAudioSampleRate * MaximumAudioDurationSeconds;
		static constexpr std::size_t MaximumAudioPcmBytes = static_cast<std::size_t>(MaximumAudioFrames) *
			MaximumAudioChannels * sizeof(std::int16_t);
		static constexpr std::size_t MaximumWaveChunks = 128;
	};

	struct AssetCancellationToken {
		std::shared_ptr<std::atomic_bool> Cancelled = std::make_shared<std::atomic_bool>(false);
		void Cancel() const { Cancelled->store(true, std::memory_order_release); }
		[[nodiscard]] bool IsCancelled() const { return Cancelled->load(std::memory_order_acquire); }
	};

	struct AssetOperationResult {
		bool Ok = false;
		std::optional<AssetRecord> Record;
		AssetDiagnostic Diagnostic;
		std::vector<AssetRecord> Records;
	};
}

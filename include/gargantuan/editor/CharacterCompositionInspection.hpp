#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gargantuan {
	class AssetService;
	class Character;

	enum class CharacterCompositionIssueCode : std::uint8_t {
		InvalidCharacter, WrongExecutionDomain, IdentityUnavailable, LimitReached, MissingRootPart, RootPartOutsideCharacter,
		RootPartCollidesIndependently, MissingAssetService, MissingMeshReference, InvalidMeshReference,
		MeshUnavailable, MeshWrongKind, MissingMaterialReference, InvalidMaterialReference,
		MaterialUnavailable, MaterialWrongKind, AnimatorWithoutSkinnedMesh, MultipleAnimators,
		AnimatorWithoutMeshParent, ClipCompatibilityNotAuthored, JointPathWithoutMesh,
		JointPathWithoutSkeleton, JointPathUnavailable, VisualPartNotRootSynchronized,
		VisualPartUnderAttachmentUnsupported, StaticMeshCollisionFallsBackToBox,
	};

	enum class CharacterCompositionSeverity : std::uint8_t { Info, Warning, Error };

	struct CharacterCompositionIssue {
		CharacterCompositionIssueCode Code;
		CharacterCompositionSeverity Severity;
		ObjectId Object;
		std::string Reference;
	};

	struct CharacterCompositionAsset {
		std::string Reference;
		std::optional<AssetId> Id;
		std::uint64_t ContentRevision = 0;
		AssetContentId ContentId;
		AssetState State = AssetState::Missing;
		bool Available = false;
	};

	struct CharacterCompositionMesh {
		ObjectId Object;
		CharacterCompositionAsset Mesh;
		std::optional<CharacterCompositionAsset> Material;
		AssetContentId SkeletonCompatibilityId;
		std::size_t JointCount = 0;
		std::size_t AnimatorCount = 0;
		bool IsRootPart = false;
		bool Anchored = false;
		bool CanCollide = false;
		bool StaticMeshCollision = false;
	};

	struct CharacterCompositionAttachment {
		ObjectId Object;
		ObjectId OwnerMesh;
		std::string JointPath;
		bool JointResolved = false;
	};

	struct CharacterCompositionLimits {
		std::size_t MaximumObjects = 512;
		std::size_t MaximumMeshes = 128;
		std::size_t MaximumAttachments = 256;
		std::size_t MaximumIssues = 128;
	};

	struct CharacterCompositionResult {
		ObjectId Character;
		std::optional<ObjectId> RootPart;
		bool Complete = true;
		bool Valid = true;
		std::size_t ObjectsVisited = 0;
		std::vector<CharacterCompositionMesh> Meshes;
		std::vector<CharacterCompositionAttachment> Attachments;
		std::vector<CharacterCompositionIssue> Issues;
	};

	// The caller owns Main-thread, scope, and read-capability admission. This function does not
	// publish ObjectIds, create assets, resolve paths, or mutate the graph or its journal.
	[[nodiscard]] CharacterCompositionResult InspectCharacterComposition(
		const std::shared_ptr<Character> &CharacterValue,
		const std::shared_ptr<AssetService> &Assets,
		CharacterCompositionLimits Limits = {}
	);
}

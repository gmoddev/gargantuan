#pragma once

#include "gargantuan/content/ContentAvailability.hpp"

namespace gargantuan {
	inline constexpr std::uint32_t ResidencySpatialMetadataVersion = 1;
	inline constexpr std::size_t MaximumResidencyMetadataIssues = 64;

	// Native adapter boundary only. The caller must obtain conservative unit
	// aggregates from a qualified metadata owner. These values do not establish
	// producer trust, calculate geometry or become a serialized package extension.
	struct ResidencyUnitBoundsMetadata final {
		std::string Key;
		AssetContentId ContentDigest;
		std::string PackageSpaceKey = "default";
		PackageCoarseBounds ConservativeBounds;
	};

	struct ResidencySpatialMetadata final {
		std::uint32_t Version = ResidencySpatialMetadataVersion;
		PackageContentNamespace Package;
		std::vector<ResidencyUnitBoundsMetadata> Units;
	};

	enum class ResidencyMetadataStatus : std::uint8_t { Compatible, Incomplete, Rejected };
	enum class ResidencyMetadataIssueCode : std::uint8_t {
		UnsupportedVersion, NamespaceMismatch, ResourceLimit, InvalidManifest,
		InvalidMetadata, UnknownUnit, ContentMismatch, SpaceMismatch,
		MissingManifestBounds, UnderboundedManifest, MissingMetadata,
	};

	struct ResidencyMetadataIssue final {
		std::string Key;
		ResidencyMetadataIssueCode Code = ResidencyMetadataIssueCode::InvalidMetadata;
	};

	struct ResidencyMetadataReport final {
		ResidencyMetadataStatus Status = ResidencyMetadataStatus::Rejected;
		std::size_t SpatialUnits = 0;
		std::size_t CompatibleUnits = 0;
		std::size_t UnverifiedUnits = 0;
		std::size_t RejectedRecords = 0;
		bool IssuesTruncated = false;
		std::vector<ResidencyMetadataIssue> Issues;
	};

	// One bounded detached merge over a 3L-validated immutable manifest and sorted
	// native metadata. Missing evidence never means conservative geometry. The
	// report is compatibility evidence only: it never requests or admits content.
	[[nodiscard]] ResidencyMetadataReport InspectResidencySpatialMetadata(
		const PackageContentManifest &Manifest,
		const ResidencySpatialMetadata &Metadata
	);
	[[nodiscard]] const char *GetResidencyMetadataIssueName(ResidencyMetadataIssueCode Code);
}

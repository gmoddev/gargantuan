#include "gargantuan/content/ResidencySpatialMetadata.hpp"

#include <cmath>

namespace gargantuan {
	namespace {
		bool ValidBounds(const PackageCoarseBounds &Bounds) {
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				if (!std::isfinite(Bounds.Minimum[Axis]) || !std::isfinite(Bounds.Maximum[Axis]) ||
					Bounds.Minimum[Axis] > Bounds.Maximum[Axis]) return false;
			return true;
		}

		bool Contains(const PackageCoarseBounds &Outer, const PackageCoarseBounds &Inner) {
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				if (Outer.Minimum[Axis] > Inner.Minimum[Axis] || Outer.Maximum[Axis] < Inner.Maximum[Axis]) return false;
			return true;
		}
	}

	ResidencyMetadataReport InspectResidencySpatialMetadata(
		const PackageContentManifest &Manifest, const ResidencySpatialMetadata &Metadata
	) {
		ResidencyMetadataReport Report;
		const auto Issue = [&](std::string_view Key, ResidencyMetadataIssueCode Code, bool Rejected) {
			if (Rejected) ++Report.RejectedRecords;
			if (Report.Issues.size() == MaximumResidencyMetadataIssues) Report.IssuesTruncated = true;
			else Report.Issues.push_back({IsValidPackageContentKey(Key) ? std::string(Key) : std::string{}, Code});
		};
		if (Metadata.Version != ResidencySpatialMetadataVersion) {
			Issue({}, ResidencyMetadataIssueCode::UnsupportedVersion, true);
			return Report;
		}
		if (Metadata.Package != Manifest.Package) {
			Issue({}, ResidencyMetadataIssueCode::NamespaceMismatch, true);
			return Report;
		}
		if (Manifest.Entries.size() > MaximumPackageContentUnits || Metadata.Units.size() > MaximumPackageContentUnits) {
			Issue({}, ResidencyMetadataIssueCode::ResourceLimit, true);
			return Report;
		}
		std::string_view Previous;
		for (const auto &Entry : Manifest.Entries) {
			if (!IsValidPackageContentKey(Entry.Key) || (!Previous.empty() && Entry.Key <= Previous) ||
				Entry.PackageSpaceKey.empty() || Entry.PackageSpaceKey.size() > MaximumPackageSpaceKeyBytes ||
				(Entry.CoarseBounds && !ValidBounds(*Entry.CoarseBounds))) {
				Issue(Entry.Key, ResidencyMetadataIssueCode::InvalidManifest, true);
				return Report;
			}
			Previous = Entry.Key;
		}
		Previous = {};
		for (const auto &Unit : Metadata.Units) {
			if (!IsValidPackageContentKey(Unit.Key) || (!Previous.empty() && Unit.Key <= Previous) ||
				Unit.PackageSpaceKey.empty() || Unit.PackageSpaceKey.size() > MaximumPackageSpaceKeyBytes ||
				!ValidBounds(Unit.ConservativeBounds)) {
				Issue(Unit.Key, ResidencyMetadataIssueCode::InvalidMetadata, true);
				return Report;
			}
			Previous = Unit.Key;
		}
		std::size_t Index = 0;
		for (const auto &Entry : Manifest.Entries) {
			while (Index < Metadata.Units.size() && Metadata.Units[Index].Key < Entry.Key) {
				Issue(Metadata.Units[Index++].Key, ResidencyMetadataIssueCode::UnknownUnit, true);
			}
			const bool Present = Index < Metadata.Units.size() && Metadata.Units[Index].Key == Entry.Key;
			if (Entry.CoarseBounds) ++Report.SpatialUnits;
			if (!Present) {
				if (Entry.CoarseBounds) {
					++Report.UnverifiedUnits;
					Issue(Entry.Key, ResidencyMetadataIssueCode::MissingMetadata, false);
				}
				continue;
			}
			const auto &Unit = Metadata.Units[Index++];
			if (Unit.ContentDigest != Entry.Digest) Issue(Entry.Key, ResidencyMetadataIssueCode::ContentMismatch, true);
			else if (Unit.PackageSpaceKey != Entry.PackageSpaceKey) Issue(Entry.Key, ResidencyMetadataIssueCode::SpaceMismatch, true);
			else if (!Entry.CoarseBounds) Issue(Entry.Key, ResidencyMetadataIssueCode::MissingManifestBounds, true);
			else if (!Contains(*Entry.CoarseBounds, Unit.ConservativeBounds))
				Issue(Entry.Key, ResidencyMetadataIssueCode::UnderboundedManifest, true);
			else ++Report.CompatibleUnits;
		}
		while (Index < Metadata.Units.size()) Issue(Metadata.Units[Index++].Key, ResidencyMetadataIssueCode::UnknownUnit, true);
		Report.Status = Report.RejectedRecords ? ResidencyMetadataStatus::Rejected :
			Report.UnverifiedUnits ? ResidencyMetadataStatus::Incomplete : ResidencyMetadataStatus::Compatible;
		return Report;
	}

	const char *GetResidencyMetadataIssueName(ResidencyMetadataIssueCode Code) {
		switch (Code) {
		case ResidencyMetadataIssueCode::UnsupportedVersion: return "UnsupportedVersion";
		case ResidencyMetadataIssueCode::NamespaceMismatch: return "NamespaceMismatch";
		case ResidencyMetadataIssueCode::ResourceLimit: return "ResourceLimit";
		case ResidencyMetadataIssueCode::InvalidManifest: return "InvalidManifest";
		case ResidencyMetadataIssueCode::InvalidMetadata: return "InvalidMetadata";
		case ResidencyMetadataIssueCode::UnknownUnit: return "UnknownUnit";
		case ResidencyMetadataIssueCode::ContentMismatch: return "ContentMismatch";
		case ResidencyMetadataIssueCode::SpaceMismatch: return "SpaceMismatch";
		case ResidencyMetadataIssueCode::MissingManifestBounds: return "MissingManifestBounds";
		case ResidencyMetadataIssueCode::UnderboundedManifest: return "UnderboundedManifest";
		case ResidencyMetadataIssueCode::MissingMetadata: return "MissingMetadata";
		}
		return "InvalidMetadata";
	}
}

#include "gargantuan/content/ResidencySpatialMetadata.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace gargantuan;

namespace {
	void Check(bool Value, const char *Message) { if (!Value) throw std::runtime_error(Message); }

	PackageContentManifest Manifest() {
		PackageContentManifest Result;
		Result.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 12};
		PackageContentEntry Entry;
		Entry.Key = "region/a";
		Entry.Digest = AssetContentId::Hash(std::array<std::uint8_t, 1>{1});
		Entry.CoarseBounds = PackageCoarseBounds{{-20, -20, -20}, {20, 20, 20}};
		Result.Entries.push_back(Entry);
		Entry.Key = "shared";
		Entry.CoarseBounds.reset();
		Result.Entries.push_back(Entry);
		return Result;
	}

	ResidencySpatialMetadata Metadata(const PackageContentManifest &Source) {
		return {ResidencySpatialMetadataVersion, Source.Package,
			{{Source.Entries[0].Key, Source.Entries[0].Digest, "default", {{-10, -1, -1}, {10, 1, 1}}}}};
	}
}

int main() {
	try {
		const auto Source = Manifest();
		auto Proof = Metadata(Source);
		const auto Good = InspectResidencySpatialMetadata(Source, Proof);
		Check(Good.Status == ResidencyMetadataStatus::Compatible && Good.CompatibleUnits == 1 && Good.SpatialUnits == 1,
			"qualified conservative unit aggregate was not compatible");
		Proof.Units.clear();
		const auto Missing = InspectResidencySpatialMetadata(Source, Proof);
		Check(Missing.Status == ResidencyMetadataStatus::Incomplete && Missing.UnverifiedUnits == 1,
			"missing proof became conservative metadata");
		Proof = Metadata(Source);
		auto Small = Source;
		Small.Entries[0].CoarseBounds = PackageCoarseBounds{{-.5f, -.5f, -.5f}, {.5f, .5f, .5f}};
		Check(InspectResidencySpatialMetadata(Small, Proof).Issues[0].Code == ResidencyMetadataIssueCode::UnderboundedManifest,
			"unit-box approximation accepted independently supplied non-normalized geometry extent");
		Proof.Version = 2;
		Check(InspectResidencySpatialMetadata(Source, Proof).Status == ResidencyMetadataStatus::Rejected, "unknown version accepted");
		Proof = Metadata(Source);
		++Proof.Package.PackageVersion;
		Check(InspectResidencySpatialMetadata(Source, Proof).Issues[0].Code == ResidencyMetadataIssueCode::NamespaceMismatch, "old package accepted");
		Proof = Metadata(Source);
		Proof.Units[0].ContentDigest = AssetContentId::Hash(std::array<std::uint8_t, 1>{2});
		Check(InspectResidencySpatialMetadata(Source, Proof).Issues[0].Code == ResidencyMetadataIssueCode::ContentMismatch, "changed payload accepted");
		Proof = Metadata(Source);
		Proof.Units[0].PackageSpaceKey = "other";
		Check(InspectResidencySpatialMetadata(Source, Proof).Issues[0].Code == ResidencyMetadataIssueCode::SpaceMismatch, "wrong space accepted");
		Proof = Metadata(Source);
		Proof.Units[0].ConservativeBounds.Minimum[0] = std::numeric_limits<float>::quiet_NaN();
		Check(InspectResidencySpatialMetadata(Source, Proof).Status == ResidencyMetadataStatus::Rejected, "NaN metadata accepted");
		Proof = Metadata(Source);
		Proof.Units.push_back(Proof.Units.front());
		Check(InspectResidencySpatialMetadata(Source, Proof).Status == ResidencyMetadataStatus::Rejected, "duplicate metadata accepted");
		Proof = Metadata(Source);
		Proof.Units[0].Key = "unknown";
		const auto Unknown = InspectResidencySpatialMetadata(Source, Proof);
		Check(Unknown.Status == ResidencyMetadataStatus::Rejected && Unknown.UnverifiedUnits == 1, "unknown unit accepted");
		Proof = Metadata(Source);
		Proof.Units[0].Key = "shared";
		Check(InspectResidencySpatialMetadata(Source, Proof).Issues.back().Code == ResidencyMetadataIssueCode::MissingManifestBounds,
			"metadata created spatial bounds for a manifest unit without them");
		auto Many = Source;
		Many.Entries.clear();
		for (std::size_t Index = 0; Index < 100; ++Index) {
			auto Entry = Source.Entries.front();
			Entry.Key = "unit/" + std::to_string(1000 + Index);
			Many.Entries.push_back(std::move(Entry));
		}
		Proof.Units.clear();
		const auto Bounded = InspectResidencySpatialMetadata(Many, Proof);
		Check(Bounded.UnverifiedUnits == 100 && Bounded.Issues.size() == MaximumResidencyMetadataIssues && Bounded.IssuesTruncated,
			"diagnostic issue list exceeded its cap or lost aggregate counts");
		Proof.Units.resize(MaximumPackageContentUnits + 1);
		Check(InspectResidencySpatialMetadata(Source, Proof).Issues[0].Code == ResidencyMetadataIssueCode::ResourceLimit,
			"oversized metadata accepted");
		std::cout << "[Content:ResidencyMetadata] conservative metadata compatibility and bounds passed\n";
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ResidencyMetadata] " << Error.what() << '\n';
		return 1;
	}
}

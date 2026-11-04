#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
	using namespace gargantuan;
	using namespace std::chrono_literals;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	struct ManifestProvider final : IContentAvailabilityProvider {
		PackageContentNamespace Package;
		std::string Encoded;

		ManifestProvider(PackageContentNamespace PackageValue, std::string EncodedValue)
			: Package(PackageValue), Encoded(std::move(EncodedValue)) {}
		std::string_view Name() const override { return "startup-manifest-fixture"; }
		ContentManifestProviderResult GetManifest(
			const ContentRequestContext &, const PackageContentNamespace &Requested
		) override {
			if (Requested != Package)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound, "package mismatch"});
			return Encoded;
		}
		ContentPayloadProviderResult GetContent(
			const ContentRequestContext &, const PackageContentIdentity &
		) override {
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "fixture has no payload"});
		}
	};

	PackageContentManifest MakeManifest(std::size_t Count) {
		PackageContentManifest Manifest;
		Manifest.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 47};
		const std::array<std::uint8_t, 1> OneByte{42};
		const auto Digest = AssetContentId::Hash(std::span<const std::uint8_t>(OneByte));
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto Number = std::to_string(Index);
			const auto Suffix = std::string(4 - Number.size(), '0') + Number;
			PackageContentEntry Entry;
			Entry.Key = "unit/" + Suffix;
			Entry.BlobReference = "content/" + Suffix + ".bin";
			Entry.Digest = Digest;
			Entry.CompressedBytes = Entry.UncompressedBytes = 1;
			Entry.ObjectCount = 1;
			Manifest.Entries.push_back(std::move(Entry));
		}
		return Manifest;
	}

	struct ServiceFixture final {
		PackageContentManifest Input = MakeManifest(128);
		std::string Encoded = EncodePackageContentManifest(Input);
		std::shared_ptr<DataModel> World = std::make_shared<DataModel>();
		std::shared_ptr<Workspace> Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		std::unique_ptr<ContentAvailabilityService> Service;

		ServiceFixture() {
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size());
			ContentAvailabilityConfiguration Configuration;
			Configuration.Provider = std::make_shared<ManifestProvider>(Input.Package, Encoded);
			Configuration.Package = Input.Package;
			Configuration.ManifestDigest = AssetContentId::Hash(Bytes);
			Configuration.Mode = ContentResidencyMode::OnDemand;
			Service = std::make_unique<ContentAvailabilityService>(World, Space, std::move(Configuration));
			const auto Deadline = std::chrono::steady_clock::now() + 10s;
			while (!Service->IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
				Service->Step();
				std::this_thread::yield();
			}
			Check(Service->IsManifestAvailable(), "manifest did not become available");
		}
		~ServiceFixture() {
			if (Service) Service->Stop();
			if (World) World->Destroy();
		}
	};

	void ExactSnapshotBindingAndValueCompatibility() {
		ServiceFixture Fixture;
		auto Snapshot = Fixture.Service->GetManifestSnapshot();
		Check(Snapshot && Snapshot->Entries.size() == 128, "validated snapshot missing");
		{
			ResidencyDemandPolicy SharedPolicy(Snapshot);
			Check(SharedPolicy.SetRequirement("unit/0000", ResidencyDemandClass::Critical),
				"shared snapshot requirement rejected");
			SharedPolicy.Step(Fixture.Service.get(), 1);
			const auto Metrics = SharedPolicy.GetMetrics();
			Check(Metrics.Requests == 1 && Metrics.ManifestBindingEntriesCompared == 0 &&
				Metrics.ServiceBindingFailures == 0, "exact snapshot binding performed a manifest scan");
			SharedPolicy.Stop(Fixture.Service.get());
		}
		{
			auto DistinctSnapshot = std::make_shared<const PackageContentManifest>(*Snapshot);
			ResidencyDemandPolicy EqualPolicy(DistinctSnapshot);
			Check(EqualPolicy.SetRequirement("unit/0001", ResidencyDemandClass::Gameplay),
				"equal snapshot requirement rejected");
			EqualPolicy.Step(Fixture.Service.get(), 1);
			const auto Metrics = EqualPolicy.GetMetrics();
			Check(Metrics.Requests == 0 && Metrics.ManifestBindingEntriesCompared == 0 &&
				Metrics.ServiceBindingFailures == 1, "distinct equal snapshot bound to another service lifetime");
			EqualPolicy.Stop(nullptr);
		}
		{
			PackageContentManifest Mutable = *Snapshot;
			ResidencyDemandPolicy ValuePolicy(Mutable);
			Mutable.Entries.front().Key = "unit/changed";
			Check(ValuePolicy.SetRequirement("unit/0002", ResidencyDemandClass::Presentation) &&
				!ValuePolicy.SetRequirement("unit/changed", ResidencyDemandClass::Critical),
				"value constructor retained caller mutation");
			ValuePolicy.Step(Fixture.Service.get(), 1);
			const auto Metrics = ValuePolicy.GetMetrics();
			Check(Metrics.Requests == 1 && Metrics.ManifestBindingEntriesCompared == Snapshot->Entries.size(),
				"value constructor did not compare the exact service manifest");
			ValuePolicy.Stop(Fixture.Service.get());
		}
		{
			auto Changed = std::make_shared<PackageContentManifest>(*Snapshot);
			const std::array<std::uint8_t, 1> DifferentByte{7};
			Changed->Entries.front().Digest = AssetContentId::Hash(std::span<const std::uint8_t>(DifferentByte));
			ResidencyDemandPolicy Mismatch{std::shared_ptr<const PackageContentManifest>(Changed)};
			Check(Mismatch.SetRequirement("unit/0000", ResidencyDemandClass::Critical),
				"mismatch policy failed to stage its native requirement");
			Mismatch.Step(Fixture.Service.get(), 1);
			const auto Metrics = Mismatch.GetMetrics();
			Check(Metrics.Requests == 0 && Metrics.ServiceBindingFailures == 1 &&
				Metrics.ManifestBindingEntriesCompared == 0, "different digest bound to service");
			Mismatch.Stop(nullptr);
		}
	}

	void SnapshotStopAndReplacementLifetime() {
		ServiceFixture Original;
		auto Snapshot = Original.Service->GetManifestSnapshot();
		std::weak_ptr<const PackageContentManifest> Weak = Snapshot;
		ResidencyDemandPolicy Prepared(Snapshot);
		Snapshot.reset();
		Original.Service->Stop();
		Check(!Original.Service->GetManifestSnapshot() && !Weak.expired(),
			"stopped service invalidated detached immutable policy preparation");
		Check(Prepared.SetRequirement("unit/0000", ResidencyDemandClass::Critical),
			"detached prepared policy lost immutable manifest keys");
		Prepared.Step(Original.Service.get(), 1);
		Check(Prepared.GetMetrics().Requests == 0, "stopped service accepted detached policy demand");
		ServiceFixture Replacement;
		Prepared.Step(Replacement.Service.get(), 2);
		Check(Prepared.GetMetrics().Requests == 0 && Prepared.GetMetrics().ServiceBindingFailures == 1 &&
			Prepared.GetMetrics().ManifestBindingEntriesCompared == 0,
			"equal replacement manifest accepted an old session's prepared policy");
		Prepared.Stop(nullptr);
	}
}

int main() {
	try {
		ExactSnapshotBindingAndValueCompatibility();
		SnapshotStopAndReplacementLifetime();
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ResidencyStartup] " << Error.what() << '\n';
		return 1;
	} catch (...) {
		std::cerr << "[Content:ResidencyStartup] unexpected exception\n";
		return 1;
	}
}

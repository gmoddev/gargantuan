#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

namespace {
	using namespace gargantuan;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Predicate> bool Pump(ContentAvailabilityService &Service, Predicate &&Complete) {
		const auto Deadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < Deadline) {
			Service.Step();
			if (Complete()) return true;
			std::this_thread::yield();
		}
		return false;
	}

	class VectorContentProvider final : public IContentAvailabilityProvider {
	  public:
		std::string Manifest;
		std::map<std::string, ContentPayload> Payloads;
		std::atomic<std::size_t> ManifestCalls{0};
		std::atomic<std::size_t> ContentCalls{0};
		bool Corrupt = false;
		std::chrono::milliseconds Delay{0};

		[[nodiscard]] std::string_view Name() const override { return "vector-content"; }
		[[nodiscard]] ContentManifestProviderResult GetManifest(
			const ContentRequestContext &Context,
			const PackageContentNamespace &
		) override {
			++ManifestCalls;
			if (Context.IsCancelled())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled});
			return Manifest;
		}
		[[nodiscard]] ContentPayloadProviderResult GetContent(
			const ContentRequestContext &Context,
			const PackageContentIdentity &Identity
		) override {
			++ContentCalls;
			if (Context.IsCancelled())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled});
			const auto ReadyAt = std::chrono::steady_clock::now() + Delay;
			while (std::chrono::steady_clock::now() < ReadyAt) {
				if (Context.IsCancelled())
					return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled});
				std::this_thread::yield();
			}
			auto Found = Payloads.find(Identity.Key);
			if (Found == Payloads.end())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound});
			auto Result = Found->second;
			if (Corrupt) {
				auto Bytes = std::make_shared<std::vector<std::uint8_t>>(*Result.Bytes);
				(*Bytes)[0] ^= 0x01;
				Result.Bytes = std::move(Bytes);
			}
			return Result;
		}
	};

	struct Fixture final {
		PackageContentNamespace Package{*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17};
		PackageContentManifest Manifest;
		std::shared_ptr<VectorContentProvider> Provider = std::make_shared<VectorContentProvider>();
		AssetContentId ManifestDigest;

		Fixture() {
			auto Region = std::static_pointer_cast<Instance>(std::make_shared<Folder>());
			Region->SetName("StreamedRegion");
			Region->SetArchivable(true);
			auto Child = std::make_shared<Folder>();
			Child->SetName("AuthoredChild");
			Child->SetArchivable(true);
			Child->SetParent(Region);
			auto Payload = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Region);
			auto Bytes = std::make_shared<std::vector<std::uint8_t>>(Payload.begin(), Payload.end());
			const auto Digest = AssetContentId::Hash(*Bytes);
			Manifest = {
				.Package = Package,
				.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
				.Entries = {{
					.Key = "workspace/00000000",
					.BlobReference = "content/regions/00000000.instance.json",
					.Digest = Digest,
					.CompressedBytes = Bytes->size(),
					.UncompressedBytes = Bytes->size(),
					.ObjectCount = 2,
					.Dependencies = {},
					.PackageSpaceKey = "default",
					.CoarseBounds = PackageCoarseBounds{{-10.0f, -10.0f, -10.0f}, {10.0f, 10.0f, 10.0f}},
					.Flags = PackageContentFlags::ImmutableBaseline,
				}},
			};
			Provider->Manifest = EncodePackageContentManifest(Manifest);
			ManifestDigest = AssetContentId::Hash(std::span(
				reinterpret_cast<const std::uint8_t *>(Provider->Manifest.data()), Provider->Manifest.size()
			));
			Provider->Payloads.emplace(
				"workspace/00000000",
				ContentPayload{{Package, "workspace/00000000"}, Digest, std::move(Bytes)}
			);
		}
	};
}

int main() {
	using namespace gargantuan;
	BootstrapNativeRuntimeSchema();

	Fixture Data;
	auto Parsed = ParsePackageContentManifest(Data.Provider->Manifest);
	const auto RoundTrippedManifest = Parsed ? EncodePackageContentManifest(*Parsed) : std::string{};
	if (!Parsed) std::cerr << "Manifest parse error: " << Parsed.error() << '\n';
	if (Parsed && RoundTrippedManifest != Data.Provider->Manifest)
		std::cerr << "Expected manifest: " << Data.Provider->Manifest << "\nActual manifest:   " << RoundTrippedManifest << '\n';
	Check(Parsed.has_value() && RoundTrippedManifest == Data.Provider->Manifest,
		"content manifest did not round-trip deterministically");
	PackageContentCoarseIndex Coarse(*Parsed, 64.0f);
	auto Query = Coarse.Query("default", {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}}, 8);
	Check(Query.size() == 1 && Query.front() == 0 && Coarse.GetMembershipCount() != 0,
		"package coarse index did not return the independent content unit");
	auto Cycle = Data.Manifest;
	Cycle.Entries.push_back(Cycle.Entries.front());
	Cycle.Entries[0].Key = "a";
	Cycle.Entries[0].Dependencies = {"b"};
	Cycle.Entries[1].Key = "b";
	Cycle.Entries[1].Dependencies = {"a"};
	Cycle.Entries[0].BlobReference = "content/a";
	Cycle.Entries[1].BlobReference = "content/b";
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(Cycle)),
		"cyclic package dependencies were accepted");

	auto World = std::make_shared<DataModel>();
	auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
	ContentAvailabilityService Service(
		World,
		WorkspaceValue,
		{
			.Provider = Data.Provider,
			.Package = Data.Package,
			.ManifestDigest = Data.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		},
		[](std::string Code, std::string Message) {
			std::cerr << "[Content:AvailabilityTest] " << Code << ": " << Message << '\n';
		}
	);
	Check(Pump(Service, [&] { return Service.IsManifestAvailable(); }), "manifest acquisition did not complete");
	for (std::size_t Peer = 0; Peer < 500; ++Peer)
		Check(Service.RequestContent("workspace/00000000"), "coalesced peer demand was rejected");
	Check(Pump(Service, [&] {
		return Service.GetState("workspace/00000000") == ContentResidencyState::Resident;
	}), "requested package content did not become Resident");
	Check(Data.Provider->ContentCalls == 1, "500 peer demands caused duplicate provider acquisition");
	auto FirstRegion = WorkspaceValue->FindFirstChild("StreamedRegion", false);
	Check(FirstRegion && FirstRegion->GetDataModel() == World,
		"admission did not create an authoritative Workspace lifetime");
	if (!FirstRegion) {
		Service.Stop();
		return 1;
	}
	const auto FirstObject = FirstRegion->GetObjectId();

	auto RuntimeChild = std::make_shared<Folder>();
	RuntimeChild->SetName("RuntimeChild");
	RuntimeChild->SetParent(FirstRegion);
	for (std::size_t Peer = 0; Peer < 500; ++Peer)
		Check(Service.ReleaseContent("workspace/00000000"), "coalesced peer demand release was rejected");
	for (std::size_t Attempt = 0; Attempt < 32; ++Attempt) Service.Step();
	Check(Service.GetState("workspace/00000000") == ContentResidencyState::Resident,
		"runtime-created child did not pin immutable baseline eviction");
	RuntimeChild->Destroy();
	Check(Pump(Service, [&] {
		return Service.GetState("workspace/00000000") == ContentResidencyState::Unavailable;
	}), "unpinned package content did not evict");
	Check(FirstRegion->GetDestroyed(), "eviction did not end the authoritative runtime lifetime");

	Check(Service.RequestContent("workspace/00000000"), "reload demand was rejected");
	Check(Pump(Service, [&] {
		return Service.GetState("workspace/00000000") == ContentResidencyState::Resident;
	}), "package content did not reload");
	auto Reloaded = WorkspaceValue->FindFirstChild("StreamedRegion", false);
	Check(Reloaded && Reloaded->GetObjectId() != FirstObject,
		"reload reused a stale runtime ObjectId lifetime");
	const auto Metrics = Service.GetMetrics();
	Check(Metrics.AcquisitionDeduplications >= 499 && Metrics.Admissions == 2 && Metrics.Evictions == 1,
		"content availability metrics did not record deduplication/admission/eviction");
	for (std::size_t Cycle = 0; Cycle < 100; ++Cycle) {
		Check(Service.ReleaseContent("workspace/00000000"), "lifecycle stress release failed");
		Check(Pump(Service, [&] {
			return Service.GetState("workspace/00000000") == ContentResidencyState::Unavailable;
		}), "lifecycle stress eviction failed");
		Check(Service.RequestContent("workspace/00000000"), "lifecycle stress request failed");
		Check(Pump(Service, [&] {
			return Service.GetState("workspace/00000000") == ContentResidencyState::Resident;
		}), "lifecycle stress reload failed");
	}
	const auto StressMetrics = Service.GetMetrics();
	Check(StressMetrics.Admissions == 102 && StressMetrics.Evictions == 101,
		"100-cycle lifecycle stress accounting was incorrect");
	Service.Stop();
	Check(Service.GetActiveRequestCount() == 0, "session teardown retained content requests");
	Check(!WorkspaceValue->FindFirstChild("StreamedRegion", false),
		"session teardown retained a package-owned authoritative Instance");

	Fixture DependencyData;
	auto DependentEntry = DependencyData.Manifest.Entries.front();
	auto DependencyEntry = DependentEntry;
	DependentEntry.Key = "a";
	DependentEntry.BlobReference = "content/a";
	DependentEntry.Dependencies = {"z"};
	DependencyEntry.Key = "z";
	DependencyEntry.BlobReference = "content/z";
	DependencyEntry.Dependencies.clear();
	DependencyData.Manifest.Entries = {DependentEntry, DependencyEntry};
	const auto SharedBytes = DependencyData.Provider->Payloads.at("workspace/00000000").Bytes;
	DependencyData.Provider->Payloads.clear();
	DependencyData.Provider->Payloads.emplace(
		"a", ContentPayload{{DependencyData.Package, "a"}, DependentEntry.Digest, SharedBytes}
	);
	DependencyData.Provider->Payloads.emplace(
		"z", ContentPayload{{DependencyData.Package, "z"}, DependencyEntry.Digest, SharedBytes}
	);
	DependencyData.Provider->Manifest = EncodePackageContentManifest(DependencyData.Manifest);
	DependencyData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(DependencyData.Provider->Manifest.data()),
		DependencyData.Provider->Manifest.size()
	));
	auto DependencyWorld = std::make_shared<DataModel>();
	auto DependencyWorkspace = std::dynamic_pointer_cast<Workspace>(DependencyWorld->GetService("Workspace"));
	ContentAvailabilityService DependencyService(
		DependencyWorld,
		DependencyWorkspace,
		{
			.Provider = DependencyData.Provider,
			.Package = DependencyData.Package,
			.ManifestDigest = DependencyData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(DependencyService, [&] { return DependencyService.IsManifestAvailable(); }),
		"dependency fixture manifest did not load");
	Check(DependencyService.RequestContent("a"), "dependent content request failed");
	Check(Pump(DependencyService, [&] {
		return DependencyService.GetState("a") == ContentResidencyState::Resident &&
			DependencyService.GetState("z") == ContentResidencyState::Resident;
	}), "hard dependency closure did not become resident");
	Check(DependencyService.ReleaseContent("a"), "dependent content release failed");
	DependencyService.Step();
	Check(DependencyService.GetState("a") == ContentResidencyState::Unavailable &&
		DependencyService.GetState("z") == ContentResidencyState::Resident,
		"hard dependency was evicted before its resident dependent");
	Check(Pump(DependencyService, [&] {
		return DependencyService.GetState("z") == ContentResidencyState::Unavailable;
	}), "released dependency did not eventually evict");
	DependencyService.Stop();

	Fixture CancellationData;
	CancellationData.Provider->Delay = 2ms;
	auto CancellationWorld = std::make_shared<DataModel>();
	auto CancellationWorkspace = std::dynamic_pointer_cast<Workspace>(CancellationWorld->GetService("Workspace"));
	ContentAvailabilityService CancellationService(
		CancellationWorld,
		CancellationWorkspace,
		{
			.Provider = CancellationData.Provider,
			.Package = CancellationData.Package,
			.ManifestDigest = CancellationData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(CancellationService, [&] { return CancellationService.IsManifestAvailable(); }),
		"cancellation fixture manifest did not load");
	for (std::size_t Cycle = 0; Cycle < 100; ++Cycle) {
		Check(CancellationService.RequestContent("workspace/00000000"), "cancellation stress request failed");
		Check(Pump(CancellationService, [&] {
			return CancellationService.GetState("workspace/00000000") == ContentResidencyState::Acquiring;
		}), "cancellation stress acquisition did not start");
		Check(CancellationService.ReleaseContent("workspace/00000000"), "cancellation stress release failed");
		Check(Pump(CancellationService, [&] {
			return CancellationService.GetState("workspace/00000000") == ContentResidencyState::Unavailable;
		}), "cancelled acquisition did not return to Unavailable");
	}
	Check(CancellationService.GetMetrics().Cancellations >= 100 &&
		CancellationData.Provider->ContentCalls == 100,
		"100-cycle cancellation stress accounting was incorrect");
	Check(!CancellationWorkspace->FindFirstChild("StreamedRegion", false),
		"cancelled content became authoritative");
	CancellationService.Stop();

	Fixture CorruptData;
	CorruptData.Provider->Corrupt = true;
	auto CorruptWorld = std::make_shared<DataModel>();
	auto CorruptWorkspace = std::dynamic_pointer_cast<Workspace>(CorruptWorld->GetService("Workspace"));
	ContentAvailabilityService CorruptService(
		CorruptWorld,
		CorruptWorkspace,
		{
			.Provider = CorruptData.Provider,
			.Package = CorruptData.Package,
			.ManifestDigest = CorruptData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(CorruptService, [&] { return CorruptService.IsManifestAvailable(); }),
		"corruption fixture manifest did not load");
	Check(CorruptService.RequestContent("workspace/00000000"), "corruption fixture request failed");
	Check(Pump(CorruptService, [&] {
		return CorruptService.GetState("workspace/00000000") == ContentResidencyState::Failed;
	}), "corrupt provider payload was not rejected");
	Check(!CorruptWorkspace->FindFirstChild("StreamedRegion", false), "corrupt payload became authoritative");
	CorruptService.Stop();

	if (Failures == 0) std::cout << "content availability foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/RemoteEvent.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <random>
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
		std::map<std::string, std::chrono::milliseconds> ContentDelays;
		std::atomic_bool ManifestRanOnWorker{false};
		std::atomic_bool ContentRanOnWorker{false};

		[[nodiscard]] std::string_view Name() const override { return "vector-content"; }
		[[nodiscard]] ContentManifestProviderResult GetManifest(
			const ContentRequestContext &Context,
			const PackageContentNamespace &
		) override {
			++ManifestCalls;
			ManifestRanOnWorker.store(GetCurrentExecutionDomain() == ExecutionDomain::Worker, std::memory_order_release);
			if (Context.IsCancelled())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled});
			return Manifest;
		}
		[[nodiscard]] ContentPayloadProviderResult GetContent(
			const ContentRequestContext &Context,
			const PackageContentIdentity &Identity
		) override {
			++ContentCalls;
			ContentRanOnWorker.store(GetCurrentExecutionDomain() == ExecutionDomain::Worker, std::memory_order_release);
			if (Context.IsCancelled())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled});
			const auto FoundDelay = ContentDelays.find(Identity.Key);
			const auto ReadyAt = std::chrono::steady_clock::now() +
				(FoundDelay == ContentDelays.end() ? Delay : FoundDelay->second);
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
	auto ThreeCycle = Data.Manifest;
	ThreeCycle.Entries = {ThreeCycle.Entries.front(), ThreeCycle.Entries.front(), ThreeCycle.Entries.front()};
	for (std::size_t Index = 0; Index < ThreeCycle.Entries.size(); ++Index) {
		ThreeCycle.Entries[Index].Key = std::string(1, static_cast<char>('a' + Index));
		ThreeCycle.Entries[Index].BlobReference = "content/" + ThreeCycle.Entries[Index].Key;
		ThreeCycle.Entries[Index].Dependencies = {
			std::string(1, static_cast<char>('a' + (Index + 1) % ThreeCycle.Entries.size()))
		};
	}
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(ThreeCycle)),
		"three-unit dependency cycle was accepted");
	auto DuplicateManifest = Data.Manifest;
	DuplicateManifest.Entries.push_back(DuplicateManifest.Entries.front());
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(DuplicateManifest)),
		"duplicate manifest key was accepted");
	auto UnsortedManifest = Data.Manifest;
	UnsortedManifest.Entries.push_back(UnsortedManifest.Entries.front());
	UnsortedManifest.Entries[0].Key = "z";
	UnsortedManifest.Entries[0].BlobReference = "content/z";
	UnsortedManifest.Entries[1].Key = "a";
	UnsortedManifest.Entries[1].BlobReference = "content/a";
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(UnsortedManifest)),
		"unsorted manifest keys were accepted");
	auto OversizedKeyManifest = Data.Manifest;
	OversizedKeyManifest.Entries.front().Key = std::string(MaximumPackageContentKeyBytes + 1, 'k');
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(OversizedKeyManifest)),
		"oversized content key was accepted");
	auto ZeroSizeManifest = Data.Manifest;
	ZeroSizeManifest.Entries.front().CompressedBytes = 0;
	ZeroSizeManifest.Entries.front().UncompressedBytes = 0;
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(ZeroSizeManifest)),
		"zero-sized content payload was accepted");
	for (const auto &Malformed : std::vector<std::string>{
			 "", "[]", "{}", Data.Provider->Manifest.substr(0, Data.Provider->Manifest.size() / 2),
			 std::string("{\"Entries\":[\"") + static_cast<char>(0xff) + "\"]}"})
		Check(!ParsePackageContentManifest(Malformed), "malformed manifest corpus entry was accepted");
	auto EscapedEntriesManifest = Data.Provider->Manifest;
	const auto EntriesKey = EscapedEntriesManifest.find("\"Entries\"");
	Check(EntriesKey != std::string::npos, "encoded manifest omitted Entries key");
	if (EntriesKey != std::string::npos)
		EscapedEntriesManifest.replace(EntriesKey, std::string_view("\"Entries\"").size(), "\"\\u0045ntries\"");
	Check(ParsePackageContentManifest(EscapedEntriesManifest).has_value(),
		"escaped top-level Entries key was not handled consistently");
	auto BoundaryManifest = Data.Manifest;
	BoundaryManifest.Entries.front().ObjectCount = MaximumPackageContentObjectsPerUnit;
	BoundaryManifest.Entries.front().CompressedBytes = MaximumPackageContentPayloadBytes;
	BoundaryManifest.Entries.front().UncompressedBytes = MaximumPackageContentPayloadBytes;
	Check(ParsePackageContentManifest(EncodePackageContentManifest(BoundaryManifest)).has_value(),
		"exact package object/payload maxima were rejected");
	BoundaryManifest.Entries.front().ObjectCount = MaximumPackageContentObjectsPerUnit - 1;
	Check(ParsePackageContentManifest(EncodePackageContentManifest(BoundaryManifest)).has_value(),
		"package object maximum minus one was rejected");
	BoundaryManifest.Entries.front().ObjectCount = MaximumPackageContentObjectsPerUnit + 1;
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(BoundaryManifest)),
		"package object maximum plus one was accepted");
	BoundaryManifest.Entries.front().ObjectCount = MaximumPackageContentObjectsPerUnit;
	BoundaryManifest.Entries.front().CompressedBytes = MaximumPackageContentPayloadBytes + 1;
	BoundaryManifest.Entries.front().UncompressedBytes = MaximumPackageContentPayloadBytes + 1;
	Check(!ParsePackageContentManifest(EncodePackageContentManifest(BoundaryManifest)),
		"package payload maximum plus one was accepted");

	auto LimitsAccepted = [&](ContentAvailabilityLimits Limits) {
		auto LimitWorld = std::make_shared<DataModel>();
		auto LimitWorkspace = std::dynamic_pointer_cast<Workspace>(LimitWorld->GetService("Workspace"));
		try {
			ContentAvailabilityService Candidate(
				LimitWorld,
				LimitWorkspace,
				{
					.Provider = Data.Provider,
					.Package = Data.Package,
					.ManifestDigest = Data.ManifestDigest,
					.Mode = ContentResidencyMode::OnDemand,
					.Limits = Limits,
				}
			);
			Candidate.Stop();
			return true;
		} catch (const std::invalid_argument &) {
			return false;
		}
	};
	ContentAvailabilityLimits Limits;
	Limits.MaximumPendingRequests = 1023;
	Check(LimitsAccepted(Limits), "pending request limit 1023 was rejected");
	Limits.MaximumPendingRequests = 1024;
	Check(LimitsAccepted(Limits), "pending request limit 1024 was rejected");
	Limits.MaximumPendingRequests = 1025;
	Check(!LimitsAccepted(Limits), "pending request limit 1025 was accepted");
	Limits = {};
	Limits.WorkerCount = MaximumContentAvailabilityWorkers + 1;
	Check(!LimitsAccepted(Limits), "worker count above the hard maximum was accepted");
	Limits = {};
	Limits.MaximumInFlight = MaximumContentAvailabilityInFlight + 1;
	Check(!LimitsAccepted(Limits), "in-flight count above the hard maximum was accepted");
	Limits = {};
	Limits.MaximumCompletedPayloadBytes = MaximumContentAvailabilityCompletedPayloadBytes - 1;
	Check(LimitsAccepted(Limits), "completed payload byte ceiling minus one was rejected");
	Limits.MaximumCompletedPayloadBytes = MaximumContentAvailabilityCompletedPayloadBytes;
	Check(LimitsAccepted(Limits), "exact completed payload byte ceiling was rejected");
	Limits = {};
	Limits.MaximumCompletedPayloadBytes = MaximumContentAvailabilityCompletedPayloadBytes + 1;
	Check(!LimitsAccepted(Limits), "completed payload bytes above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumCachedPayloadBytes = MaximumContentAvailabilityCachedPayloadBytes + 1;
	Check(!LimitsAccepted(Limits), "cache bytes above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumAdmissionUnitsPerTick = MaximumContentAvailabilityAdmissionUnitsPerTick + 1;
	Check(!LimitsAccepted(Limits), "admission units above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumAdmissionObjectsPerTick = MaximumContentAvailabilityAdmissionObjectsPerTick + 1;
	Check(!LimitsAccepted(Limits), "admission objects above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumAdmissionBytesPerTick = MaximumContentAvailabilityAdmissionBytesPerTick + 1;
	Check(!LimitsAccepted(Limits), "admission bytes above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumEvictionUnitsPerTick = MaximumContentAvailabilityEvictionUnitsPerTick + 1;
	Check(!LimitsAccepted(Limits), "eviction units above the hard maximum were accepted");
	Limits = {};
	Limits.MaximumEvictionObjectsPerTick = MaximumContentAvailabilityEvictionObjectsPerTick + 1;
	Check(!LimitsAccepted(Limits), "eviction objects above the hard maximum were accepted");
	Limits = {};
	Limits.RequestTimeout = 9ms;
	Check(!LimitsAccepted(Limits), "request timeout below the hard minimum was accepted");
	Limits = {};
	Limits.RequestTimeout = 10s + 1ms;
	Check(!LimitsAccepted(Limits), "request timeout above the hard maximum was accepted");

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
	FirstRegion->SetName("MutatedRegion");

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
	Check(!ObjectRegistry::Get().Lookup(FirstObject), "eviction left the stale runtime ObjectId resolvable");

	const auto AcquisitionsBeforeReload = Data.Provider->ContentCalls.load(std::memory_order_acquire);
	Check(Service.RequestContent("workspace/00000000"), "reload demand was rejected");
	Check(Pump(Service, [&] {
		return Service.GetState("workspace/00000000") == ContentResidencyState::Resident;
	}), "package content did not reload");
	auto Reloaded = WorkspaceValue->FindFirstChild("StreamedRegion", false);
	Check(Reloaded && Reloaded->GetObjectId() != FirstObject && Reloaded->GetName() == "StreamedRegion",
		"reload did not reconstruct the immutable baseline with a fresh ObjectId lifetime");
	const auto Metrics = Service.GetMetrics();
	Check(Metrics.AcquisitionDeduplications >= 499 && Metrics.Admissions == 2 && Metrics.Evictions == 1,
		"content availability metrics did not record deduplication/admission/eviction");
	Check(Metrics.CacheHits == 1 && Data.Provider->ContentCalls == AcquisitionsBeforeReload &&
		Metrics.CachedPayloadBytes != 0 && Metrics.CachedPayloadBytes <= MaximumContentAvailabilityCachedPayloadBytes,
		"load/evict/reload did not reuse only the bounded immutable byte cache");
	Check(Data.Provider->ManifestRanOnWorker.load(std::memory_order_acquire) &&
		Data.Provider->ContentRanOnWorker.load(std::memory_order_acquire),
		"content provider work escaped the Worker execution domain");
	Check(Metrics.Timing.EndToEnd.Samples == 2 && Metrics.Timing.Provider.Samples == 2 &&
		Metrics.Timing.Preparation.Samples == 2 && Metrics.Timing.Commit.Samples == 2 &&
		Metrics.Timing.Step.Samples != 0,
		"T0-T8 content timing instrumentation did not record successful lifetimes");
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
	auto SharedDependencyEntry = DependentEntry;
	auto LeafDependencyEntry = DependentEntry;
	DependentEntry.Key = "a";
	DependentEntry.BlobReference = "content/a";
	DependentEntry.Dependencies = {"b", "c"};
	DependencyEntry.Key = "b";
	DependencyEntry.BlobReference = "content/b";
	DependencyEntry.Dependencies = {"z"};
	SharedDependencyEntry.Key = "c";
	SharedDependencyEntry.BlobReference = "content/c";
	SharedDependencyEntry.Dependencies = {"z"};
	LeafDependencyEntry.Key = "z";
	LeafDependencyEntry.BlobReference = "content/z";
	LeafDependencyEntry.Dependencies.clear();
	DependencyData.Manifest.Entries = {DependentEntry, DependencyEntry, SharedDependencyEntry, LeafDependencyEntry};
	const auto SharedBytes = DependencyData.Provider->Payloads.at("workspace/00000000").Bytes;
	DependencyData.Provider->Payloads.clear();
	DependencyData.Provider->Payloads.emplace(
		"a", ContentPayload{{DependencyData.Package, "a"}, DependentEntry.Digest, SharedBytes}
	);
	DependencyData.Provider->Payloads.emplace(
		"b", ContentPayload{{DependencyData.Package, "b"}, DependencyEntry.Digest, SharedBytes}
	);
	DependencyData.Provider->Payloads.emplace(
		"c", ContentPayload{{DependencyData.Package, "c"}, SharedDependencyEntry.Digest, SharedBytes}
	);
	DependencyData.Provider->Payloads.emplace(
		"z", ContentPayload{{DependencyData.Package, "z"}, LeafDependencyEntry.Digest, SharedBytes}
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
			.Limits = ContentAvailabilityLimits{.MaximumAdmissionUnitsPerTick = 1},
		}
	);
	Check(Pump(DependencyService, [&] { return DependencyService.IsManifestAvailable(); }),
		"dependency fixture manifest did not load");
	Check(DependencyService.RequestContent("a"), "dependent content request failed");
	Check(Pump(DependencyService, [&] {
		const auto A = DependencyService.GetState("a");
		const auto B = DependencyService.GetState("b");
		const auto C = DependencyService.GetState("c");
		const auto Z = DependencyService.GetState("z");
		Check(B != ContentResidencyState::Resident || Z == ContentResidencyState::Resident,
			"dependency B admitted before leaf Z");
		Check(C != ContentResidencyState::Resident || Z == ContentResidencyState::Resident,
			"dependency C admitted before shared leaf Z");
		Check(A != ContentResidencyState::Resident ||
			(B == ContentResidencyState::Resident && C == ContentResidencyState::Resident &&
				Z == ContentResidencyState::Resident),
			"dependent A admitted before its complete dependency closure");
		return A == ContentResidencyState::Resident;
	}), "branched/shared hard dependency closure did not become resident");
	Check(DependencyService.ReleaseContent("a"), "dependent content release failed");
	DependencyService.Step();
	Check(DependencyService.GetState("a") == ContentResidencyState::Unavailable &&
		DependencyService.GetState("z") == ContentResidencyState::Resident,
		"hard dependency was evicted before its resident dependent");
	Check(Pump(DependencyService, [&] {
		return DependencyService.GetState("z") == ContentResidencyState::Unavailable;
	}), "released dependency did not eventually evict");
	Check(DependencyService.PinContent("a"), "trusted dependency pin was rejected");
	Check(Pump(DependencyService, [&] {
		return DependencyService.GetState("a") == ContentResidencyState::Resident &&
			DependencyService.GetState("b") == ContentResidencyState::Resident &&
			DependencyService.GetState("c") == ContentResidencyState::Resident &&
			DependencyService.GetState("z") == ContentResidencyState::Resident;
	}), "trusted pin did not acquire its dependency closure");
	Check(DependencyService.UnpinContent("a"), "trusted dependency unpin was rejected");
	Check(Pump(DependencyService, [&] {
		return DependencyService.GetState("a") == ContentResidencyState::Unavailable &&
			DependencyService.GetState("b") == ContentResidencyState::Unavailable &&
			DependencyService.GetState("c") == ContentResidencyState::Unavailable &&
			DependencyService.GetState("z") == ContentResidencyState::Unavailable;
	}), "trusted dependency unpin did not release the closure");
	DependencyService.Stop();

	Fixture DependencyFailureData;
	auto FailureDependent = DependencyFailureData.Manifest.Entries.front();
	auto FailureLeaf = FailureDependent;
	FailureDependent.Key = "a";
	FailureDependent.BlobReference = "content/a";
	FailureDependent.Dependencies = {"z"};
	FailureLeaf.Key = "z";
	FailureLeaf.BlobReference = "content/z";
	FailureLeaf.Dependencies.clear();
	DependencyFailureData.Manifest.Entries = {FailureDependent, FailureLeaf};
	const auto FailureBytes = DependencyFailureData.Provider->Payloads.at("workspace/00000000").Bytes;
	DependencyFailureData.Provider->Payloads.clear();
	DependencyFailureData.Provider->Payloads.emplace(
		"a", ContentPayload{{DependencyFailureData.Package, "a"}, FailureDependent.Digest, FailureBytes}
	);
	DependencyFailureData.Provider->Manifest = EncodePackageContentManifest(DependencyFailureData.Manifest);
	DependencyFailureData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(DependencyFailureData.Provider->Manifest.data()),
		DependencyFailureData.Provider->Manifest.size()
	));
	auto DependencyFailureWorld = std::make_shared<DataModel>();
	auto DependencyFailureWorkspace = std::dynamic_pointer_cast<Workspace>(
		DependencyFailureWorld->GetService("Workspace")
	);
	ContentAvailabilityService DependencyFailureService(
		DependencyFailureWorld,
		DependencyFailureWorkspace,
		{
			.Provider = DependencyFailureData.Provider,
			.Package = DependencyFailureData.Package,
			.ManifestDigest = DependencyFailureData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(DependencyFailureService, [&] { return DependencyFailureService.IsManifestAvailable(); }),
		"dependency-failure fixture manifest did not load");
	Check(DependencyFailureService.RequestContent("a"), "dependency-failure request was rejected");
	Check(Pump(DependencyFailureService, [&] {
		return DependencyFailureService.GetState("z") == ContentResidencyState::Failed;
	}), "missing dependency did not fail in bounded time");
	Check(Pump(DependencyFailureService, [&] {
		return DependencyFailureService.GetActiveRequestCount() == 0;
	}), "unrelated dependent payload acquisition did not finish after dependency failure");
	for (std::size_t Tick = 0; Tick < 32; ++Tick) DependencyFailureService.Step();
	Check(DependencyFailureService.GetState("a") != ContentResidencyState::Resident &&
		DependencyFailureService.GetActiveRequestCount() == 0,
		"failed dependency permitted dependent admission or spun acquisition");
	DependencyFailureData.Provider->Payloads.emplace(
		"z", ContentPayload{{DependencyFailureData.Package, "z"}, FailureLeaf.Digest, FailureBytes}
	);
	Check(DependencyFailureService.RetryContent("z"), "trusted dependency retry was rejected");
	Check(Pump(DependencyFailureService, [&] {
		return DependencyFailureService.GetState("a") == ContentResidencyState::Resident &&
			DependencyFailureService.GetState("z") == ContentResidencyState::Resident;
	}), "dependency recovery did not unblock the dependent");
	DependencyFailureService.Stop();

	Fixture FairnessData;
	const auto FairnessBytes = FairnessData.Provider->Payloads.at("workspace/00000000").Bytes;
	const auto FairnessDigest = FairnessData.Manifest.Entries.front().Digest;
	FairnessData.Manifest.Entries.clear();
	FairnessData.Provider->Payloads.clear();
	for (const auto &Key : {std::string("a"), std::string("b"), std::string("c")}) {
		auto Entry = Data.Manifest.Entries.front();
		Entry.Key = Key;
		Entry.BlobReference = "content/" + Key;
		Entry.Digest = FairnessDigest;
		Entry.CompressedBytes = FairnessBytes->size();
		Entry.UncompressedBytes = FairnessBytes->size();
		Entry.Dependencies.clear();
		FairnessData.Manifest.Entries.push_back(Entry);
		FairnessData.Provider->Payloads.emplace(
			Key, ContentPayload{{FairnessData.Package, Key}, FairnessDigest, FairnessBytes}
		);
	}
	FairnessData.Provider->ContentDelays.emplace("a", 500ms);
	FairnessData.Provider->Manifest = EncodePackageContentManifest(FairnessData.Manifest);
	FairnessData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(FairnessData.Provider->Manifest.data()),
		FairnessData.Provider->Manifest.size()
	));
	auto FairnessWorld = std::make_shared<DataModel>();
	auto FairnessWorkspace = std::dynamic_pointer_cast<Workspace>(FairnessWorld->GetService("Workspace"));
	ContentAvailabilityService FairnessService(
		FairnessWorld,
		FairnessWorkspace,
		{
			.Provider = FairnessData.Provider,
			.Package = FairnessData.Package,
			.ManifestDigest = FairnessData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
			.Limits = ContentAvailabilityLimits{.WorkerCount = 2, .MaximumInFlight = 2},
		}
	);
	Check(Pump(FairnessService, [&] { return FairnessService.IsManifestAvailable(); }),
		"fairness fixture manifest did not load");
	for (const auto &Key : {"a", "b", "c"})
		Check(FairnessService.RequestContent(Key), "fairness request was rejected");
	Check(Pump(FairnessService, [&] {
		return FairnessService.GetState("b") == ContentResidencyState::Resident &&
			FairnessService.GetState("c") == ContentResidencyState::Resident;
	}), "one slow acquisition blocked unrelated ready content");
	Check(FairnessService.GetState("a") == ContentResidencyState::Acquiring,
		"slow acquisition completed before the fairness ordering was observed");
	Check(Pump(FairnessService, [&] {
		return FairnessService.GetState("a") == ContentResidencyState::Resident;
	}), "oldest slow desired content did not eventually progress");
	FairnessService.Stop();

	Fixture CacheData;
	auto LargeBytes = std::make_shared<std::vector<std::uint8_t>>(
		*CacheData.Provider->Payloads.at("workspace/00000000").Bytes
	);
	LargeBytes->resize(MaximumPackageContentPayloadBytes, static_cast<std::uint8_t>(' '));
	const auto LargeDigest = AssetContentId::Hash(*LargeBytes);
	auto BarrierEntry = CacheData.Manifest.Entries.front();
	BarrierEntry.Key = "z";
	BarrierEntry.BlobReference = "content/z";
	BarrierEntry.Dependencies.clear();
	std::vector<PackageContentEntry> CacheEntries;
	constexpr std::size_t CacheUnitCount =
		MaximumContentAvailabilityCachedPayloadBytes / MaximumPackageContentPayloadBytes + 1;
	for (std::size_t Index = 0; Index < CacheUnitCount; ++Index) {
		auto Entry = CacheData.Manifest.Entries.front();
		Entry.Key = "a" + std::format("{:02}", Index);
		Entry.BlobReference = "content/" + Entry.Key;
		Entry.Digest = LargeDigest;
		Entry.CompressedBytes = LargeBytes->size();
		Entry.UncompressedBytes = LargeBytes->size();
		Entry.Dependencies = {"z"};
		CacheEntries.push_back(Entry);
		CacheData.Provider->Payloads.emplace(
			Entry.Key, ContentPayload{{CacheData.Package, Entry.Key}, LargeDigest, LargeBytes}
		);
	}
	CacheEntries.push_back(BarrierEntry);
	CacheData.Manifest.Entries = std::move(CacheEntries);
	CacheData.Provider->Payloads.emplace(
		"z",
		ContentPayload{
			{CacheData.Package, "z"},
			BarrierEntry.Digest,
			CacheData.Provider->Payloads.at("workspace/00000000").Bytes,
		}
	);
	CacheData.Provider->ContentDelays.emplace("z", 2s);
	CacheData.Provider->Manifest = EncodePackageContentManifest(CacheData.Manifest);
	CacheData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(CacheData.Provider->Manifest.data()),
		CacheData.Provider->Manifest.size()
	));
	auto CacheWorld = std::make_shared<DataModel>();
	auto CacheWorkspace = std::dynamic_pointer_cast<Workspace>(CacheWorld->GetService("Workspace"));
	ContentAvailabilityService CacheService(
		CacheWorld,
		CacheWorkspace,
		{
			.Provider = CacheData.Provider,
			.Package = CacheData.Package,
			.ManifestDigest = CacheData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(CacheService, [&] { return CacheService.IsManifestAvailable(); }),
		"cache-pressure fixture manifest did not load");
	for (std::size_t Index = 0; Index < CacheUnitCount; ++Index)
		Check(CacheService.RequestContent("a" + std::format("{:02}", Index)), "cache-pressure request failed");
	Check(Pump(CacheService, [&] {
		return CacheService.GetState("a00") == ContentResidencyState::Available;
	}), "cache-pressure fixture did not retain a prepared unit");
	Check(CacheService.ReleaseContent("a00"), "cache-pressure release failed");
	Check(Pump(CacheService, [&] {
		return CacheService.GetMetrics().CacheEvictions != 0;
	}), "32 MiB prepared cache did not evict an undemanded unit under pressure");
	Check(CacheService.GetMetrics().CachedPayloadBytesHighWater <= MaximumContentAvailabilityCachedPayloadBytes,
		"prepared cache exceeded its exact byte ceiling");
	CacheService.Stop();

	Fixture CompletionData;
	auto CompletionBytes = std::make_shared<std::vector<std::uint8_t>>(
		*CompletionData.Provider->Payloads.at("workspace/00000000").Bytes
	);
	CompletionBytes->resize(MaximumPackageContentPayloadBytes, static_cast<std::uint8_t>(' '));
	const auto CompletionDigest = AssetContentId::Hash(*CompletionBytes);
	CompletionData.Manifest.Entries.clear();
	CompletionData.Provider->Payloads.clear();
	CompletionData.Provider->Delay = 50ms;
	constexpr std::size_t CompletionUnitCount =
		MaximumContentAvailabilityCompletedPayloadBytes / MaximumPackageContentPayloadBytes + 1;
	constexpr std::size_t CompletionCapacityUnitCount = CompletionUnitCount - 1;
	for (std::size_t Index = 0; Index < CompletionUnitCount; ++Index) {
		const auto Key = "completion/" + std::format("{:02}", Index);
		auto Entry = Data.Manifest.Entries.front();
		Entry.Key = Key;
		Entry.BlobReference = "content/" + Key;
		Entry.Digest = CompletionDigest;
		Entry.CompressedBytes = CompletionBytes->size();
		Entry.UncompressedBytes = CompletionBytes->size();
		Entry.Dependencies.clear();
		CompletionData.Manifest.Entries.push_back(Entry);
		CompletionData.Provider->Payloads.emplace(
			Key, ContentPayload{{CompletionData.Package, Key}, CompletionDigest, CompletionBytes}
		);
	}
	CompletionData.Provider->Manifest = EncodePackageContentManifest(CompletionData.Manifest);
	CompletionData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(CompletionData.Provider->Manifest.data()),
		CompletionData.Provider->Manifest.size()
	));
	auto CompletionWorld = std::make_shared<DataModel>();
	auto CompletionWorkspace = std::dynamic_pointer_cast<Workspace>(CompletionWorld->GetService("Workspace"));
	ContentAvailabilityService CompletionService(
		CompletionWorld,
		CompletionWorkspace,
		{
			.Provider = CompletionData.Provider,
			.Package = CompletionData.Package,
			.ManifestDigest = CompletionData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
			.Limits = ContentAvailabilityLimits{
				.WorkerCount = 8,
				.MaximumInFlight = MaximumContentAvailabilityInFlight,
				.MaximumCompletedPayloadBytes = MaximumContentAvailabilityCompletedPayloadBytes,
			},
		}
	);
	Check(Pump(CompletionService, [&] { return CompletionService.IsManifestAvailable(); }),
		"completion-capacity fixture manifest did not load");
	for (std::size_t Index = 0; Index < CompletionUnitCount; ++Index)
		Check(CompletionService.RequestContent("completion/" + std::format("{:02}", Index)),
			"completion-capacity demand was rejected");
	CompletionService.Step();
	const auto CompletionStartDeadline = std::chrono::steady_clock::now() + 2s;
	while (CompletionData.Provider->ContentCalls.load(std::memory_order_acquire) < CompletionCapacityUnitCount &&
		std::chrono::steady_clock::now() < CompletionStartDeadline) std::this_thread::yield();
	while (CompletionService.GetMetrics().CompletedPayloadBytes < MaximumContentAvailabilityCompletedPayloadBytes &&
		std::chrono::steady_clock::now() < CompletionStartDeadline) std::this_thread::yield();
	const auto ReservedMetrics = CompletionService.GetMetrics();
	Check(CompletionData.Provider->ContentCalls == CompletionCapacityUnitCount &&
		ReservedMetrics.AcquiringUnits == CompletionCapacityUnitCount &&
		ReservedMetrics.RequestedUnits == 1 &&
		ReservedMetrics.ReservedCompletionPayloadBytes == MaximumContentAvailabilityCompletedPayloadBytes &&
		ReservedMetrics.CompletedPayloadBytes == MaximumContentAvailabilityCompletedPayloadBytes &&
		ReservedMetrics.CompletionCapacityDeferrals != 0,
		"16 MiB completion capacity was not reserved before provider work began");
	Check(Pump(CompletionService, [&] {
		for (std::size_t Index = 0; Index < CompletionUnitCount; ++Index)
			if (CompletionService.GetState("completion/" + std::format("{:02}", Index)) !=
				ContentResidencyState::Resident) return false;
		return true;
	}), "completion-capacity backlog did not drain incrementally");
	const auto CompletionMetrics = CompletionService.GetMetrics();
	Check(CompletionMetrics.CompletedPayloadBytesHighWater == MaximumContentAvailabilityCompletedPayloadBytes &&
		CompletionMetrics.CompletedPayloadBytes == 0 &&
		CompletionMetrics.ReservedCompletionPayloadBytes == 0 &&
		CompletionData.Provider->ContentCalls == CompletionUnitCount,
		"completion queue exceeded, silently discarded, or failed to drain its exact byte boundary");
	CompletionService.Stop();

	Fixture QueueData;
	const auto QueueBytes = QueueData.Provider->Payloads.at("workspace/00000000").Bytes;
	const auto QueueDigest = QueueData.Manifest.Entries.front().Digest;
	QueueData.Manifest.Entries.clear();
	QueueData.Provider->Payloads.clear();
	QueueData.Provider->Delay = 100ms;
	for (std::size_t Index = 0; Index < 1026; ++Index) {
		const auto Key = "queue/" + std::format("{:04}", Index);
		auto Entry = Data.Manifest.Entries.front();
		Entry.Key = Key;
		Entry.BlobReference = "content/" + Key;
		Entry.Digest = QueueDigest;
		Entry.CompressedBytes = QueueBytes->size();
		Entry.UncompressedBytes = QueueBytes->size();
		Entry.Dependencies.clear();
		QueueData.Manifest.Entries.push_back(Entry);
		QueueData.Provider->Payloads.emplace(
			Key, ContentPayload{{QueueData.Package, Key}, QueueDigest, QueueBytes}
		);
	}
	QueueData.Provider->Manifest = EncodePackageContentManifest(QueueData.Manifest);
	QueueData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(QueueData.Provider->Manifest.data()),
		QueueData.Provider->Manifest.size()
	));
	auto QueueWorld = std::make_shared<DataModel>();
	auto QueueWorkspace = std::dynamic_pointer_cast<Workspace>(QueueWorld->GetService("Workspace"));
	ContentAvailabilityService QueueService(
		QueueWorld,
		QueueWorkspace,
		{
			.Provider = QueueData.Provider,
			.Package = QueueData.Package,
			.ManifestDigest = QueueData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
			.Limits = ContentAvailabilityLimits{.WorkerCount = 1, .MaximumInFlight = 1},
		}
	);
	Check(Pump(QueueService, [&] { return QueueService.IsManifestAvailable(); }),
		"queue-pressure fixture manifest did not load");
	for (std::size_t Index = 0; Index < 1026; ++Index)
		Check(QueueService.RequestContent("queue/" + std::format("{:04}", Index)),
			"queue-pressure demand was rejected");
	QueueService.Step();
	const auto QueueMetrics = QueueService.GetMetrics();
	Check(QueueMetrics.PendingHighWater == MaximumContentAvailabilityPendingRequests &&
		QueueMetrics.RequestedUnits == MaximumContentAvailabilityPendingRequests &&
		QueueMetrics.AcquiringUnits == 1 &&
		QueueService.GetState("queue/1025") == ContentResidencyState::Unavailable,
		"1023/1024/1025 queue pressure did not remain explicitly bounded");
	QueueService.Stop();

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

	Fixture PreparingCancellationData;
	auto PreparingBytes = std::make_shared<std::vector<std::uint8_t>>(
		*PreparingCancellationData.Provider->Payloads.at("workspace/00000000").Bytes
	);
	PreparingBytes->resize(MaximumPackageContentPayloadBytes, static_cast<std::uint8_t>(' '));
	const auto PreparingDigest = AssetContentId::Hash(*PreparingBytes);
	PreparingCancellationData.Manifest.Entries.front().Digest = PreparingDigest;
	PreparingCancellationData.Manifest.Entries.front().CompressedBytes = PreparingBytes->size();
	PreparingCancellationData.Manifest.Entries.front().UncompressedBytes = PreparingBytes->size();
	PreparingCancellationData.Provider->Payloads["workspace/00000000"] = ContentPayload{
		{PreparingCancellationData.Package, "workspace/00000000"}, PreparingDigest, PreparingBytes
	};
	PreparingCancellationData.Provider->Manifest = EncodePackageContentManifest(PreparingCancellationData.Manifest);
	PreparingCancellationData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(PreparingCancellationData.Provider->Manifest.data()),
		PreparingCancellationData.Provider->Manifest.size()
	));
	auto PreparingWorld = std::make_shared<DataModel>();
	auto PreparingWorkspace = std::dynamic_pointer_cast<Workspace>(PreparingWorld->GetService("Workspace"));
	ContentAvailabilityService PreparingCancellationService(
		PreparingWorld,
		PreparingWorkspace,
		{
			.Provider = PreparingCancellationData.Provider,
			.Package = PreparingCancellationData.Package,
			.ManifestDigest = PreparingCancellationData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(PreparingCancellationService, [&] { return PreparingCancellationService.IsManifestAvailable(); }),
		"prepare-cancellation fixture manifest did not load");
	Check(PreparingCancellationService.RequestContent("workspace/00000000"),
		"prepare-cancellation request was rejected");
	Check(Pump(PreparingCancellationService, [&] {
		return PreparingCancellationService.GetState("workspace/00000000") == ContentResidencyState::Acquiring;
	}), "prepare-cancellation acquisition did not start");
	Check(PreparingCancellationService.ReleaseContent("workspace/00000000"),
		"prepare-cancellation release was rejected");
	Check(Pump(PreparingCancellationService, [&] {
		return PreparingCancellationService.GetState("workspace/00000000") == ContentResidencyState::Unavailable;
	}), "cancellation during legal-max worker preparation did not converge");
	Check(!PreparingWorkspace->FindFirstChild("StreamedRegion", false) &&
		PreparingCancellationService.GetMetrics().Cancellations != 0,
		"cancelled legal-max preparation committed obsolete authoritative content");
	PreparingCancellationService.Stop();

	Fixture WrongVersionData;
	auto WrongVersionPayload = WrongVersionData.Provider->Payloads.at("workspace/00000000");
	++WrongVersionPayload.Identity.Package.PackageVersion;
	WrongVersionData.Provider->Payloads["workspace/00000000"] = std::move(WrongVersionPayload);
	auto WrongVersionWorld = std::make_shared<DataModel>();
	auto WrongVersionWorkspace = std::dynamic_pointer_cast<Workspace>(WrongVersionWorld->GetService("Workspace"));
	ContentAvailabilityService WrongVersionService(
		WrongVersionWorld,
		WrongVersionWorkspace,
		{
			.Provider = WrongVersionData.Provider,
			.Package = WrongVersionData.Package,
			.ManifestDigest = WrongVersionData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(WrongVersionService, [&] { return WrongVersionService.IsManifestAvailable(); }),
		"wrong-package-version fixture manifest did not load");
	Check(WrongVersionService.RequestContent("workspace/00000000"),
		"wrong-package-version request was rejected before acquisition");
	Check(Pump(WrongVersionService, [&] {
		return WrongVersionService.GetState("workspace/00000000") == ContentResidencyState::Failed;
	}) && !WrongVersionWorkspace->FindFirstChild("StreamedRegion", false),
		"a completion from the wrong package version became authoritative");
	WrongVersionService.Stop();

	for (std::size_t Cycle = 0; Cycle < 10; ++Cycle) {
		Fixture StopData;
		StopData.Provider->Delay = 100ms;
		auto StopWorld = std::make_shared<DataModel>();
		auto StopWorkspace = std::dynamic_pointer_cast<Workspace>(StopWorld->GetService("Workspace"));
		ContentAvailabilityService StopService(
			StopWorld,
			StopWorkspace,
			{
				.Provider = StopData.Provider,
				.Package = StopData.Package,
				.ManifestDigest = StopData.ManifestDigest,
				.Mode = ContentResidencyMode::OnDemand,
			}
		);
		Check(Pump(StopService, [&] { return StopService.IsManifestAvailable(); }),
			"Stop-during-request manifest did not load");
		Check(StopService.RequestContent("workspace/00000000"), "Stop-during-request demand was rejected");
		Check(Pump(StopService, [&] {
			return StopService.GetState("workspace/00000000") == ContentResidencyState::Acquiring;
		}), "Stop-during-request acquisition did not start");
		StopService.Stop();
		Check(StopService.GetActiveRequestCount() == 0 &&
			!StopWorkspace->FindFirstChild("StreamedRegion", false),
			"Stop-during-request retained acquisition or authoritative content");
	}

	Fixture RandomData;
	const auto RandomBytes = RandomData.Provider->Payloads.at("workspace/00000000").Bytes;
	const auto RandomDigest = RandomData.Manifest.Entries.front().Digest;
	RandomData.Manifest.Entries.clear();
	RandomData.Provider->Payloads.clear();
	for (std::size_t Index = 0; Index < 16; ++Index) {
		const auto Key = "random/" + std::format("{:02}", Index);
		auto Entry = Data.Manifest.Entries.front();
		Entry.Key = Key;
		Entry.BlobReference = "content/" + Key;
		Entry.Digest = RandomDigest;
		Entry.CompressedBytes = RandomBytes->size();
		Entry.UncompressedBytes = RandomBytes->size();
		Entry.Dependencies.clear();
		RandomData.Manifest.Entries.push_back(Entry);
		RandomData.Provider->Payloads.emplace(
			Key, ContentPayload{{RandomData.Package, Key}, RandomDigest, RandomBytes}
		);
	}
	RandomData.Provider->Manifest = EncodePackageContentManifest(RandomData.Manifest);
	RandomData.ManifestDigest = AssetContentId::Hash(std::span(
		reinterpret_cast<const std::uint8_t *>(RandomData.Provider->Manifest.data()),
		RandomData.Provider->Manifest.size()
	));
	auto RandomWorld = std::make_shared<DataModel>();
	auto RandomWorkspace = std::dynamic_pointer_cast<Workspace>(RandomWorld->GetService("Workspace"));
	ContentAvailabilityService RandomService(
		RandomWorld,
		RandomWorkspace,
		{
			.Provider = RandomData.Provider,
			.Package = RandomData.Package,
			.ManifestDigest = RandomData.ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		}
	);
	Check(Pump(RandomService, [&] { return RandomService.IsManifestAvailable(); }),
		"randomized model fixture manifest did not load");
	std::array<std::size_t, 16> RandomDemand{};
	std::mt19937 Random(0x3'4c'1u);
	for (std::size_t Operation = 0; Operation < 10'000; ++Operation) {
		const auto Index = static_cast<std::size_t>(Random() % RandomDemand.size());
		const auto Key = "random/" + std::format("{:02}", Index);
		if ((Random() & 1u) != 0) {
			Check(RandomService.RequestContent(Key), "randomized request was rejected");
			++RandomDemand[Index];
		} else if (RandomDemand[Index] == 0) {
			Check(!RandomService.ReleaseContent(Key), "randomized zero-demand release was accepted");
		} else {
			Check(RandomService.ReleaseContent(Key), "randomized release was rejected");
			--RandomDemand[Index];
		}
		RandomService.Step();
		const auto State = RandomService.GetState(Key);
		Check(State.has_value() && *State >= ContentResidencyState::Unavailable &&
			*State <= ContentResidencyState::Failed, "randomized operation produced an invalid state");
	}
	for (std::size_t Index = 0; Index < RandomDemand.size(); ++Index) {
		const auto Key = "random/" + std::format("{:02}", Index);
		while (RandomDemand[Index] != 0) {
			Check(RandomService.ReleaseContent(Key), "randomized final demand release failed");
			--RandomDemand[Index];
		}
	}
	Check(Pump(RandomService, [&] {
		for (std::size_t Index = 0; Index < RandomDemand.size(); ++Index)
			if (RandomService.GetState("random/" + std::format("{:02}", Index)) !=
				ContentResidencyState::Unavailable) return false;
		return true;
	}), "randomized model did not converge to the empty residency set");
	RandomService.Stop();

	auto RunRejectedPayload = [&](std::string Encoded, std::uint32_t DeclaredObjects, const char *Message) {
		Fixture InvalidData;
		auto Bytes = std::make_shared<std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		const auto Digest = AssetContentId::Hash(*Bytes);
		InvalidData.Manifest.Entries.front().Digest = Digest;
		InvalidData.Manifest.Entries.front().CompressedBytes = Bytes->size();
		InvalidData.Manifest.Entries.front().UncompressedBytes = Bytes->size();
		InvalidData.Manifest.Entries.front().ObjectCount = DeclaredObjects;
		InvalidData.Provider->Payloads["workspace/00000000"] = ContentPayload{
			{InvalidData.Package, "workspace/00000000"}, Digest, Bytes
		};
		InvalidData.Provider->Manifest = EncodePackageContentManifest(InvalidData.Manifest);
		InvalidData.ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(InvalidData.Provider->Manifest.data()),
			InvalidData.Provider->Manifest.size()
		));
		auto InvalidWorld = std::make_shared<DataModel>();
		auto InvalidWorkspace = std::dynamic_pointer_cast<Workspace>(InvalidWorld->GetService("Workspace"));
		ContentAvailabilityService InvalidService(
			InvalidWorld,
			InvalidWorkspace,
			{
				.Provider = InvalidData.Provider,
				.Package = InvalidData.Package,
				.ManifestDigest = InvalidData.ManifestDigest,
				.Mode = ContentResidencyMode::OnDemand,
			}
		);
		Check(Pump(InvalidService, [&] { return InvalidService.IsManifestAvailable(); }),
			"malformed-payload fixture manifest did not load");
		Check(InvalidService.RequestContent("workspace/00000000"),
			"malformed-payload request was rejected before validation");
		Check(Pump(InvalidService, [&] {
			return InvalidService.GetState("workspace/00000000") == ContentResidencyState::Failed;
		}) && !InvalidWorkspace->FindFirstChild("StreamedRegion", false), Message);
		InvalidService.Stop();
	};
	const auto ValidPayload = std::string(
		Data.Provider->Payloads.at("workspace/00000000").Bytes->begin(),
		Data.Provider->Payloads.at("workspace/00000000").Bytes->end()
	);
	RunRejectedPayload("{\"Version\":4", 2, "truncated payload became authoritative");
	RunRejectedPayload("{\"Version\":4}", 2, "invalid-schema payload became authoritative");
	RunRejectedPayload("[1,2,3]", 2, "invalid payload shape became authoritative");
	RunRejectedPayload(ValidPayload, 1, "incorrect declared object count became authoritative");

	auto DetachedBoundary = std::static_pointer_cast<Instance>(std::make_shared<Folder>());
	DetachedBoundary->SetArchivable(true);
	auto DetachedScript = std::make_shared<Script>();
	DetachedScript->SetArchivable(true);
	DetachedScript->SetSource("error('detached script executed')");
	DetachedScript->SetParent(DetachedBoundary);
	auto DetachedRemote = std::make_shared<RemoteEvent>();
	DetachedRemote->SetArchivable(true);
	DetachedRemote->SetParent(DetachedBoundary);
	const auto DetachedEncoded = InstanceSerialization::Serialize(
		InstanceSerialization::InstanceFormat::Json, DetachedBoundary
	);
	std::istringstream DetachedInput(DetachedEncoded);
	auto DetachedPrepared = InstanceSerialization::DeserializeDetached(
		InstanceSerialization::InstanceFormat::Json, DetachedInput
	);
	Check(DetachedPrepared.Ok && DetachedPrepared.Instance &&
		!DetachedPrepared.Instance->GetDataModel() &&
		DetachedPrepared.Instance->FindFirstChildWhichIsA("Script", true) &&
		!DetachedPrepared.Instance->FindFirstChildWhichIsA("Script", true)->GetDataModel() &&
		DetachedPrepared.Instance->FindFirstChildWhichIsA("RemoteEvent", true) &&
		!DetachedPrepared.Instance->FindFirstChildWhichIsA("RemoteEvent", true)->GetDataModel(),
		"detached Script or Remote crossed the authoritative visibility boundary");

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

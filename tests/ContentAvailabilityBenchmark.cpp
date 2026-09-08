#include "gargantuan/content/ContentAvailability.hpp"
#include "assets/PreparedInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__linux__)
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {
	struct TrackedAllocationHeader final {
		void *Raw = nullptr;
		std::size_t Bytes = 0;
		std::uint64_t Generation = 0;
	};

	std::atomic<std::uint64_t> AllocationGeneration{0};
	std::atomic<std::uint64_t> ActiveAllocationGeneration{0};
	std::atomic<std::uint64_t> TrackedAllocationCount{0};
	std::atomic<std::size_t> TrackedAllocationBytes{0};
	std::atomic<std::size_t> TrackedAllocationPeakBytes{0};

	void UpdateAllocationPeak(std::size_t Candidate) {
		auto Peak = TrackedAllocationPeakBytes.load(std::memory_order_relaxed);
		while (Peak < Candidate && !TrackedAllocationPeakBytes.compare_exchange_weak(
			Peak, Candidate, std::memory_order_relaxed
		)) {}
	}

	void *AllocateTracked(std::size_t Bytes, std::size_t Alignment) {
		Alignment = std::max(Alignment, alignof(TrackedAllocationHeader));
		if (Bytes > std::numeric_limits<std::size_t>::max() - sizeof(TrackedAllocationHeader) - Alignment)
			throw std::bad_alloc();
		void *Raw = std::malloc(Bytes + sizeof(TrackedAllocationHeader) + Alignment - 1);
		if (!Raw) throw std::bad_alloc();
		const auto Start = reinterpret_cast<std::uintptr_t>(Raw) + sizeof(TrackedAllocationHeader);
		const auto Aligned = (Start + Alignment - 1) & ~(static_cast<std::uintptr_t>(Alignment) - 1);
		auto *Header = reinterpret_cast<TrackedAllocationHeader *>(Aligned) - 1;
		const auto Generation = ActiveAllocationGeneration.load(std::memory_order_acquire);
		*Header = {Raw, Bytes, Generation};
		if (Generation != 0) {
			TrackedAllocationCount.fetch_add(1, std::memory_order_relaxed);
			const auto Current = TrackedAllocationBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
			UpdateAllocationPeak(Current);
		}
		return reinterpret_cast<void *>(Aligned);
	}

	void FreeTracked(void *Pointer) noexcept {
		if (!Pointer) return;
		auto *Header = reinterpret_cast<TrackedAllocationHeader *>(Pointer) - 1;
		if (Header->Generation != 0 &&
			Header->Generation == ActiveAllocationGeneration.load(std::memory_order_acquire))
			TrackedAllocationBytes.fetch_sub(Header->Bytes, std::memory_order_relaxed);
		std::free(Header->Raw);
	}
}

void *operator new(std::size_t Bytes) { return AllocateTracked(Bytes, alignof(std::max_align_t)); }
void *operator new[](std::size_t Bytes) { return AllocateTracked(Bytes, alignof(std::max_align_t)); }
void *operator new(std::size_t Bytes, std::align_val_t Alignment) {
	return AllocateTracked(Bytes, static_cast<std::size_t>(Alignment));
}
void *operator new[](std::size_t Bytes, std::align_val_t Alignment) {
	return AllocateTracked(Bytes, static_cast<std::size_t>(Alignment));
}
void *operator new(std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return AllocateTracked(Bytes, alignof(std::max_align_t)); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return AllocateTracked(Bytes, alignof(std::max_align_t)); } catch (...) { return nullptr; }
}
void *operator new(std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return AllocateTracked(Bytes, static_cast<std::size_t>(Alignment)); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return AllocateTracked(Bytes, static_cast<std::size_t>(Alignment)); } catch (...) { return nullptr; }
}
void operator delete(void *Pointer) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer) noexcept { FreeTracked(Pointer); }
void operator delete(void *Pointer, std::size_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer, std::size_t) noexcept { FreeTracked(Pointer); }
void operator delete(void *Pointer, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete(void *Pointer, std::size_t, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer, std::size_t, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete(void *Pointer, const std::nothrow_t &) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer, const std::nothrow_t &) noexcept { FreeTracked(Pointer); }
void operator delete(void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { FreeTracked(Pointer); }
void operator delete[](void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { FreeTracked(Pointer); }

namespace {
	using namespace gargantuan;

	struct Timings final {
		double Mean = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
		double Maximum = 0.0;
	};

	void PrintProcessMemory(std::string_view Phase) {
		std::cout << "[Content:PressureMemory] phase=" << Phase;
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters), sizeof(Counters)))
			throw std::runtime_error("pressure process memory query failed");
		std::cout << " rssBytes=" << Counters.WorkingSetSize << " rssHighWaterBytes=" << Counters.PeakWorkingSetSize
			<< " privateBytes=" << Counters.PrivateUsage;
#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t Pages = 0, Resident = 0;
		Input >> Pages >> Resident;
		rusage Usage{};
		if (!Input || getrusage(RUSAGE_SELF, &Usage) != 0) throw std::runtime_error("pressure process memory query failed");
		std::cout << " rssBytes=" << Resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE))
			<< " rssHighWaterBytes=" << static_cast<std::uint64_t>(Usage.ru_maxrss) * 1024;
#endif
		std::cout << '\n';
	}

	struct AllocationSample final {
		std::uint64_t Allocations = 0;
		std::size_t PeakBytes = 0;
		std::size_t RetainedBytes = 0;
	};

	void StartAllocationSample() {
		TrackedAllocationCount.store(0, std::memory_order_relaxed);
		TrackedAllocationBytes.store(0, std::memory_order_relaxed);
		TrackedAllocationPeakBytes.store(0, std::memory_order_relaxed);
		const auto Generation = AllocationGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
		ActiveAllocationGeneration.store(Generation, std::memory_order_release);
	}

	AllocationSample FinishAllocationSample() {
		AllocationSample Result{
			.Allocations = TrackedAllocationCount.load(std::memory_order_relaxed),
			.PeakBytes = TrackedAllocationPeakBytes.load(std::memory_order_relaxed),
			.RetainedBytes = TrackedAllocationBytes.load(std::memory_order_relaxed),
		};
		ActiveAllocationGeneration.store(0, std::memory_order_release);
		return Result;
	}

	Timings Summarize(std::vector<double> Samples) {
		std::ranges::sort(Samples);
		double Total = 0.0;
		for (const auto Sample : Samples) Total += Sample;
		auto Percentile = [&](std::size_t Percent) { return Samples[(Samples.size() - 1) * Percent / 100]; };
		return {
			.Mean = Total / Samples.size(),
			.P50 = Percentile(50),
			.P95 = Percentile(95),
			.P99 = Percentile(99),
			.Maximum = Samples.back(),
		};
	}

	PackageContentManifest MakeManifest(std::size_t Count) {
		const std::array<std::uint8_t, 1> DigestInput{0x3a};
		const auto Digest = AssetContentId::Hash(DigestInput);
		PackageContentManifest Manifest{
			.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17},
			.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
		};
		Manifest.Entries.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto Coordinate = static_cast<float>(Index);
			Manifest.Entries.push_back({
				.Key = std::format("region/{:07}", Index),
				.BlobReference = std::format("content/regions/{:07}.instance.json", Index),
				.Digest = Digest,
				.CompressedBytes = 1,
				.UncompressedBytes = 1,
				.ObjectCount = 1,
				.Dependencies = {},
				.PackageSpaceKey = "default",
				.CoarseBounds = PackageCoarseBounds{{Coordinate, 0.0f, 0.0f}, {Coordinate, 0.0f, 0.0f}},
				.Flags = PackageContentFlags::ImmutableBaseline,
			});
		}
		return Manifest;
	}

	void RunParsedManifestCase(std::size_t Count) {
		auto Manifest = MakeManifest(Count);
		const auto Encoded = EncodePackageContentManifest(Manifest);
		const auto StartedAt = std::chrono::steady_clock::now();
		auto Parsed = ParsePackageContentManifest(Encoded);
		const auto ParseMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - StartedAt
		).count();
		if (!Parsed || Parsed->Entries.size() != Count) throw std::runtime_error("benchmark manifest parse failed");
		const auto IndexStartedAt = std::chrono::steady_clock::now();
		PackageContentCoarseIndex Index(*Parsed, 1.0f);
		const auto IndexMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - IndexStartedAt
		).count();
		std::mt19937_64 Random(0x3'4c'17);
		std::uniform_int_distribution<std::size_t> Distribution(0, Count - 1);
		std::vector<double> LookupMicroseconds;
		LookupMicroseconds.reserve(2048);
		for (std::size_t QueryNumber = 0; QueryNumber < 2048; ++QueryNumber) {
			const auto Coordinate = static_cast<float>(Distribution(Random));
			const auto QueryStartedAt = std::chrono::steady_clock::now();
			auto Result = Index.Query(
				"default", {{Coordinate, 0.0f, 0.0f}, {Coordinate, 0.0f, 0.0f}}, 8
			);
			LookupMicroseconds.push_back(std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - QueryStartedAt
			).count());
			if (Result.size() != 1) throw std::runtime_error("benchmark coarse lookup failed");
		}
		const auto Lookup = Summarize(std::move(LookupMicroseconds));
		std::cout << "[Content:Benchmark] manifest entries=" << Count << " encodedBytes=" << Encoded.size()
				  << " parseMs=" << ParseMilliseconds << " indexMs=" << IndexMilliseconds
				  << " memberships=" << Index.GetMembershipCount() << " lookupMeanUs=" << Lookup.Mean
				  << " lookupP50Us=" << Lookup.P50 << " lookupP95Us=" << Lookup.P95
				  << " lookupP99Us=" << Lookup.P99 << " lookupMaxUs=" << Lookup.Maximum << '\n';
	}

	void RunRejectedManifestCase(std::size_t Count, bool EscapeEntriesKey = false) {
		std::string Encoded = EscapeEntriesKey
			? R"({"Format":"GargantuanPackageContent","Version":1,"ProjectId":"0123456789abcdef0123456789abcdef","PackageVersion":17,"InstanceSchemaVersion":4,"\u0045ntries":[)"
			: R"({"Format":"GargantuanPackageContent","Version":1,"ProjectId":"0123456789abcdef0123456789abcdef","PackageVersion":17,"InstanceSchemaVersion":4,"Entries":[)";
		Encoded.reserve(Encoded.size() + Count * 3 + 2);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			if (Index != 0) Encoded.push_back(',');
			Encoded += "{}";
		}
		Encoded += "]}";
		const auto StartedAt = std::chrono::steady_clock::now();
		auto Parsed = ParsePackageContentManifest(Encoded);
		const auto RejectMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - StartedAt
		).count();
		if (Parsed) throw std::runtime_error("oversized manifest entry count was accepted");
		std::cout << "[Content:Benchmark] manifest entries=" << Count << " encodedBytes=" << Encoded.size()
				  << " boundedReject=true rejectMs=" << RejectMilliseconds
				  << " escapedEntriesKey=" << (EscapeEntriesKey ? "true" : "false")
				  << " maximum=" << MaximumPackageContentUnits << '\n';
	}

	std::string MakeContentPayload(
		std::size_t ObjectCount,
		std::size_t TargetBytes = 0,
		bool RepresentativeParts = false,
		bool DeepHierarchy = false
	) {
		if (ObjectCount == 0) throw std::invalid_argument("content benchmark requires at least one object");
		auto Root = std::static_pointer_cast<Instance>(std::make_shared<Folder>());
		Root->SetArchivable(true);
		std::vector<std::shared_ptr<Instance>> Objects{Root};
		Objects.reserve(ObjectCount);
		for (std::size_t Index = 1; Index < ObjectCount; ++Index) {
			auto Child = RepresentativeParts
				? std::static_pointer_cast<Instance>(std::make_shared<Part>())
				: std::static_pointer_cast<Instance>(std::make_shared<Folder>());
			Child->SetArchivable(true);
			if (auto PartValue = std::dynamic_pointer_cast<Part>(Child)) {
				PartValue->SetAnchored(true);
				PartValue->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index % 32), 0.0f, static_cast<float>(Index / 32))));
				PartValue->SetSize(glm::vec3(2.0f, 1.0f, 3.0f));
			}
			Child->SetParent(DeepHierarchy && Index < 64 ? Objects.back() : Root);
			Objects.push_back(std::move(Child));
		}
		for (auto &Object : Objects) Object->SetName("");
		auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		if (TargetBytes == 0 || Encoded.size() >= TargetBytes) return Encoded;
		auto Remaining = TargetBytes - Encoded.size();
		for (auto &Object : Objects) {
			const auto NameBytes = std::min(Remaining, MaximumProtocolStringBytes);
			Object->SetName(std::string(NameBytes, 'x'));
			Remaining -= NameBytes;
			if (Remaining == 0) break;
		}
		if (Remaining != 0) throw std::length_error("content benchmark payload target exceeds legal string capacity");
		Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		if (Encoded.size() != TargetBytes) throw std::runtime_error(std::format(
			"content benchmark payload size was not exact: expected {}, got {}", TargetBytes, Encoded.size()
		));
		return Encoded;
	}

	void RunContentPreparationCase(
		std::string_view Profile,
		std::size_t ObjectCount,
		std::size_t TargetBytes,
		std::size_t Iterations,
		bool RepresentativeParts = false,
		bool DeepHierarchy = false
	) {
		const auto Encoded = MakeContentPayload(ObjectCount, TargetBytes, RepresentativeParts, DeepHierarchy);
		const auto Bytes = std::span(
			reinterpret_cast<const std::uint8_t *>(Encoded.data()), Encoded.size()
		);
		std::vector<double> PreparationSamples;
		std::vector<double> MaterializationSamples;
		std::vector<double> CommitSamples;
		std::vector<double> WorkerAllocationSamples;
		std::vector<double> WorkerPeakByteSamples;
		std::vector<double> WorkerRetainedByteSamples;
		std::vector<double> MainAllocationSamples;
		std::vector<double> MainPeakByteSamples;
		std::vector<double> MainRetainedByteSamples;
		std::vector<double> CommitAllocationSamples;
		std::vector<double> CommitPeakByteSamples;
		std::vector<double> CommitRetainedByteSamples;
		PreparationSamples.reserve(Iterations);
		MaterializationSamples.reserve(Iterations);
		CommitSamples.reserve(Iterations);
		for (auto *Samples : {&WorkerAllocationSamples, &WorkerPeakByteSamples, &WorkerRetainedByteSamples,
			&MainAllocationSamples, &MainPeakByteSamples, &MainRetainedByteSamples,
			&CommitAllocationSamples, &CommitPeakByteSamples, &CommitRetainedByteSamples})
			Samples->reserve(Iterations);
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration) {
			const auto StartedAt = std::chrono::steady_clock::now();
			StartAllocationSample();
			auto Document = InstanceSerialization::Internal::PrepareDetachedJson(Bytes);
			const auto WorkerAllocation = FinishAllocationSample();
			const auto PreparedAt = std::chrono::steady_clock::now();
			PreparationSamples.push_back(std::chrono::duration<double, std::milli>(
				PreparedAt - StartedAt
			).count());
			if (!Document) throw std::runtime_error("content benchmark document preparation failed: " + Document.error());
			StartAllocationSample();
			auto Prepared = InstanceSerialization::Internal::MaterializeDetachedJson(*Document);
			const auto MainAllocation = FinishAllocationSample();
			MaterializationSamples.push_back(std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - PreparedAt
			).count());
			if (!Prepared.Ok || !Prepared.Instance || Prepared.ObjectsDecoded != ObjectCount)
				throw std::runtime_error("content benchmark detached preparation failed");
			const auto CommitStartedAt = std::chrono::steady_clock::now();
			StartAllocationSample();
			Prepared.Instance->SetParent(WorkspaceValue);
			const auto CommitAllocation = FinishAllocationSample();
			CommitSamples.push_back(std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - CommitStartedAt
			).count());
			Prepared.Instance->Destroy();
			WorkerAllocationSamples.push_back(static_cast<double>(WorkerAllocation.Allocations));
			WorkerPeakByteSamples.push_back(static_cast<double>(WorkerAllocation.PeakBytes));
			WorkerRetainedByteSamples.push_back(static_cast<double>(WorkerAllocation.RetainedBytes));
			MainAllocationSamples.push_back(static_cast<double>(MainAllocation.Allocations));
			MainPeakByteSamples.push_back(static_cast<double>(MainAllocation.PeakBytes));
			MainRetainedByteSamples.push_back(static_cast<double>(MainAllocation.RetainedBytes));
			CommitAllocationSamples.push_back(static_cast<double>(CommitAllocation.Allocations));
			CommitPeakByteSamples.push_back(static_cast<double>(CommitAllocation.PeakBytes));
			CommitRetainedByteSamples.push_back(static_cast<double>(CommitAllocation.RetainedBytes));
		}
		const auto Preparation = Summarize(std::move(PreparationSamples));
		const auto Materialization = Summarize(std::move(MaterializationSamples));
		const auto Commit = Summarize(std::move(CommitSamples));
		const auto WorkerAllocations = Summarize(std::move(WorkerAllocationSamples));
		const auto WorkerPeakBytes = Summarize(std::move(WorkerPeakByteSamples));
		const auto WorkerRetainedBytes = Summarize(std::move(WorkerRetainedByteSamples));
		const auto MainAllocations = Summarize(std::move(MainAllocationSamples));
		const auto MainPeakBytes = Summarize(std::move(MainPeakByteSamples));
		const auto MainRetainedBytes = Summarize(std::move(MainRetainedByteSamples));
		const auto CommitAllocations = Summarize(std::move(CommitAllocationSamples));
		const auto CommitPeakBytes = Summarize(std::move(CommitPeakByteSamples));
		const auto CommitRetainedBytes = Summarize(std::move(CommitRetainedByteSamples));
		std::cout << "[Content:Benchmark] preparation profile=" << Profile << " objects=" << ObjectCount << " payloadBytes=" << Encoded.size()
				  << " samples=" << Iterations << " workerMeanMs=" << Preparation.Mean
				  << " workerP50Ms=" << Preparation.P50 << " workerP95Ms=" << Preparation.P95
				  << " workerP99Ms=" << Preparation.P99 << " workerMaxMs=" << Preparation.Maximum
				  << " mainMeanMs=" << Materialization.Mean << " mainP50Ms=" << Materialization.P50
				  << " mainP95Ms=" << Materialization.P95 << " mainP99Ms=" << Materialization.P99
				  << " mainMaxMs=" << Materialization.Maximum << " commitMeanMs=" << Commit.Mean
				  << " commitP50Ms=" << Commit.P50 << " commitP95Ms=" << Commit.P95
				  << " commitP99Ms=" << Commit.P99 << " commitMaxMs=" << Commit.Maximum << '\n';
		auto PrintAllocationSummary = [&](std::string_view Stage, const Timings &Allocations,
			const Timings &PeakBytes, const Timings &RetainedBytes) {
			std::cout << "[Content:Allocations] profile=" << Profile << " objects=" << ObjectCount
				<< " payloadBytes=" << Encoded.size() << " stage=" << Stage << " samples=" << Iterations
				<< " allocationsMean=" << Allocations.Mean << " allocationsP50=" << Allocations.P50
				<< " allocationsP95=" << Allocations.P95 << " allocationsP99=" << Allocations.P99
				<< " allocationsMax=" << Allocations.Maximum
				<< " peakBytesMean=" << PeakBytes.Mean << " peakBytesP50=" << PeakBytes.P50
				<< " peakBytesP95=" << PeakBytes.P95 << " peakBytesP99=" << PeakBytes.P99
				<< " peakBytesMax=" << PeakBytes.Maximum
				<< " retainedBytesMean=" << RetainedBytes.Mean << " retainedBytesP50=" << RetainedBytes.P50
				<< " retainedBytesP95=" << RetainedBytes.P95 << " retainedBytesP99=" << RetainedBytes.P99
				<< " retainedBytesMax=" << RetainedBytes.Maximum << '\n';
		};
		PrintAllocationSummary("worker-prepare", WorkerAllocations, WorkerPeakBytes, WorkerRetainedBytes);
		PrintAllocationSummary("main-materialize", MainAllocations, MainPeakBytes, MainRetainedBytes);
		PrintAllocationSummary("main-commit", CommitAllocations, CommitPeakBytes, CommitRetainedBytes);
	}

	class BenchmarkContentProvider final : public IContentAvailabilityProvider {
	  public:
		std::string Manifest;
		ContentPayload Payload;
		bool MissingDependency = false;

		[[nodiscard]] std::string_view Name() const override { return "benchmark-content"; }
		[[nodiscard]] ContentManifestProviderResult GetManifest(
			const ContentRequestContext &, const PackageContentNamespace &
		) override { return Manifest; }
		[[nodiscard]] ContentPayloadProviderResult GetContent(
			const ContentRequestContext &, const PackageContentIdentity &Identity
		) override {
			if (MissingDependency && Identity.Key == "z")
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound});
			auto Result = Payload;
			Result.Identity = Identity;
			return Result;
		}
	};

	// Isolate retained service allocations from fixture construction and process RSS.
	// The missing dependency prevents admission without pausing workers or bypassing
	// ContentAvailability. Every dependent is a legal 512-object immutable unit.
	void RunDecodedPressureCase(bool BlockDependency = true) {
		const auto Encoded = MakeContentPayload(512, 0, true);
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		auto Manifest = MakeManifest(64);
		const auto Digest = AssetContentId::Hash(*Bytes);
		for (auto &Entry : Manifest.Entries) {
			Entry.Digest = Digest;
			Entry.CompressedBytes = Bytes->size();
			Entry.UncompressedBytes = Bytes->size();
			Entry.ObjectCount = 512;
			if (BlockDependency) Entry.Dependencies = {"z"};
		}
		auto Dependency = Manifest.Entries.back();
		Dependency.Key = "z";
		Dependency.BlobReference = "content/z";
		Dependency.Dependencies.clear();
		if (BlockDependency) Manifest.Entries.push_back(Dependency);
		auto Provider = std::make_shared<BenchmarkContentProvider>();
		Provider->Manifest = EncodePackageContentManifest(Manifest);
		Provider->Payload = {{Manifest.Package, ""}, Digest, Bytes};
		Provider->MissingDependency = true;
		const auto ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Provider->Manifest.data()), Provider->Manifest.size()));
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		StartAllocationSample();
		{
			ContentAvailabilityService Service(World, WorkspaceValue, {
				.Provider = Provider, .Package = Manifest.Package, .ManifestDigest = ManifestDigest,
				.Mode = ContentResidencyMode::OnDemand,
				.Limits = {.WorkerCount = 8, .MaximumInFlight = 16, .MaximumCompletionsPerTick = 16,
					.MaximumAdmissionUnitsPerTick = 1},
			});
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!Service.IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
				Service.Step();
				std::this_thread::yield();
			}
			if (!Service.IsManifestAvailable()) throw std::runtime_error("pressure manifest unavailable");
			for (const auto &Entry : Manifest.Entries)
				if (Entry.Key != "z" && !Service.RequestContent(Entry.Key))
					throw std::runtime_error("pressure demand rejected");
			const auto DrainStarted = std::chrono::steady_clock::now();
			std::uint64_t DrainTicks = 0;
			do {
				Service.Step();
				++DrainTicks;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			} while ((BlockDependency ? Service.GetMetrics().RequestedUnits != 0 || Service.GetActiveRequestCount() != 0 :
				Service.GetMetrics().ResidentUnits != 64) &&
				std::chrono::steady_clock::now() < Deadline);
			const auto Metrics = Service.GetMetrics();
			std::cout << "[Content:Pressure] units=64 objectsPerUnit=512 encodedPerUnit=" << Encoded.size()
				<< " available=" << Metrics.PreparedUnits << " resident=" << Metrics.ResidentUnits
				<< " cacheBytes=" << Metrics.CachedPayloadBytes << " completionBytes=" << Metrics.CompletedPayloadBytes
				<< " decodedBytes=" << Metrics.DecodedDocumentBytes
				<< " decodedHighWater=" << Metrics.DecodedDocumentBytesHighWater
				<< " decodedDeferrals=" << Metrics.DecodedCapacityDeferrals
				<< " pendingHighWater=" << Metrics.PendingHighWater << " inFlightHighWater=" << Metrics.InFlightHighWater
				<< " completionHighWaterBytes=" << Metrics.CompletedPayloadBytesHighWater
				<< " cacheHighWaterBytes=" << Metrics.CachedPayloadBytesHighWater
				<< " availableDecodedHighWaterBytes=" << Metrics.AvailableDecodedBytesHighWater
				<< " detachedObjectsHighWater=" << Metrics.DetachedObjectsHighWater
				<< " residentOriginObjectsHighWater=" << Metrics.ResidentPackageObjectsHighWater
				<< " drainTicks=" << DrainTicks
				<< " drainMs=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - DrainStarted).count()
				<< " mainStepMaxUs=" << Metrics.Timing.Step.MaximumMicroseconds
				<< " trackedRetainedBytes=" << TrackedAllocationBytes.load()
				<< " trackedPeakBytes=" << TrackedAllocationPeakBytes.load() << '\n';
			PrintProcessMemory("pressure-converged");
			if (Metrics.ResidentUnits != (BlockDependency ? 0u : 64u)) throw std::runtime_error("pressure admission did not converge");
			if (!BlockDependency && (Metrics.Acquisitions != 64 || Metrics.Admissions != 64 ||
				Metrics.DecodedCapacityDeferrals == 0 || Metrics.Failures != 0))
				throw std::runtime_error("decoded overload did not defer and drain without refetch/failure");
			if (Metrics.DecodedDocumentBytesHighWater > MaximumContentAvailabilityDecodedDocumentBytes)
				throw std::runtime_error("decoded documents exceeded capacity");
			Service.Stop();
			if (Service.GetMetrics().DecodedDocumentBytes != 0 || Service.GetMetrics().RetainedRecordCount != 0)
				throw std::runtime_error("Stop retained decoded/session state");
			std::cout << "[Content:Pressure] afterStopTrackedBytes=" << TrackedAllocationBytes.load() << '\n';
			PrintProcessMemory("after-stop");
		}
		std::cout << "[Content:Pressure] afterServiceDestructionTrackedBytes=" << TrackedAllocationBytes.load() << '\n';
		World->Destroy();
		WorkspaceValue.reset();
		World.reset();
		std::cout << "[Content:Pressure] afterWorldDestructionTrackedBytes=" << TrackedAllocationBytes.load() << '\n';
		PrintProcessMemory("after-world-destruction");
		// Attribution only, after every fixture consumer has stopped. Production
		// lifecycle must not globally clear another live world's journal.
		ChangeJournal::Get().Clear();
		std::cout << "[Content:Pressure] afterJournalClearTrackedBytes=" << TrackedAllocationBytes.load() << '\n';
		const auto Allocations = FinishAllocationSample();
		(void)Allocations;
	}

	void RunServiceAdmissionCase(
		std::size_t ObjectCount,
		std::size_t PayloadBytes,
		std::size_t Iterations,
		bool RepresentativeParts = false,
		bool TrackLifecycle = false
	) {
		const auto Encoded = MakeContentPayload(ObjectCount, PayloadBytes, RepresentativeParts);
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		const PackageContentNamespace Package{*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17};
		const auto Digest = AssetContentId::Hash(*Bytes);
		PackageContentManifest Manifest{
			.Package = Package,
			.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
			.Entries = {{
				.Key = "workspace/00000000",
				.BlobReference = "content/regions/00000000.instance.json",
				.Digest = Digest,
				.CompressedBytes = Bytes->size(),
				.UncompressedBytes = Bytes->size(),
				.ObjectCount = static_cast<std::uint32_t>(ObjectCount),
				.Dependencies = {},
				.PackageSpaceKey = "default",
				.CoarseBounds = std::nullopt,
				.Flags = PackageContentFlags::ImmutableBaseline,
			}},
		};
		auto Provider = std::make_shared<BenchmarkContentProvider>();
		Provider->Manifest = EncodePackageContentManifest(Manifest);
		Provider->Payload = {{Package, "workspace/00000000"}, Digest, Bytes};
		const auto ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Provider->Manifest.data()), Provider->Manifest.size()
		));
		std::vector<double> WorkerPreparationSamples;
		std::vector<double> TotalPreparationSamples;
		std::vector<double> CommitSamples;
		std::vector<double> ResidentSamples;
		std::vector<double> MainStepSamples;
		std::vector<double> DrainedBytes;
		DrainedBytes.reserve(Iterations);
		if (TrackLifecycle) StartAllocationSample();
		for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration) {
			if (TrackLifecycle && Iteration != 0) {
				DrainedBytes.push_back(static_cast<double>(TrackedAllocationBytes.load()));
				std::cout << "[Content:Lifecycle] cycle=" << Iteration << " drainedTrackedBytes=" << DrainedBytes.back() << '\n';
			}
			auto World = std::make_shared<DataModel>();
			auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
			ContentAvailabilityService Service(World, WorkspaceValue, {
				.Provider = Provider,
				.Package = Package,
				.ManifestDigest = ManifestDigest,
				.Mode = ContentResidencyMode::OnDemand,
			});
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Service.IsManifestAvailable() && std::chrono::steady_clock::now() < Deadline) {
				Service.Step();
				std::this_thread::yield();
			}
			if (!Service.IsManifestAvailable() || !Service.RequestContent("workspace/00000000"))
				throw std::runtime_error("content benchmark service did not accept its manifest/request");
			while (Service.GetState("workspace/00000000") != ContentResidencyState::Resident &&
				std::chrono::steady_clock::now() < Deadline) {
				Service.Step();
				std::this_thread::yield();
			}
			if (Service.GetState("workspace/00000000") != ContentResidencyState::Resident)
				throw std::runtime_error("content benchmark service did not reach Resident");
			const auto Metrics = Service.GetMetrics();
			auto Milliseconds = [](const ContentAvailabilityDurationMetric &Metric) {
				return static_cast<double>(Metric.MaximumMicroseconds) / 1000.0;
			};
			WorkerPreparationSamples.push_back(Milliseconds(Metrics.Timing.WorkerPreparation));
			TotalPreparationSamples.push_back(Milliseconds(Metrics.Timing.Preparation));
			CommitSamples.push_back(Milliseconds(Metrics.Timing.Commit));
			ResidentSamples.push_back(Milliseconds(Metrics.Timing.EndToEnd));
			MainStepSamples.push_back(Milliseconds(Metrics.Timing.Step));
			Service.Stop();
		}
		if (TrackLifecycle) {
			DrainedBytes.push_back(static_cast<double>(TrackedAllocationBytes.load()));
			const auto Allocation = FinishAllocationSample();
			std::vector<double> Tail(DrainedBytes.begin() + DrainedBytes.size() / 2, DrainedBytes.end());
			const auto Minimum = *std::ranges::min_element(Tail);
			const auto Maximum = *std::ranges::max_element(Tail);
			std::cout << "[Content:Lifecycle] cycles=" << Iterations << " tailMinBytes=" << Minimum
				<< " tailMaxBytes=" << Maximum << " finalBytes=" << Allocation.RetainedBytes
				<< " peakBytes=" << Allocation.PeakBytes << '\n';
			if (Maximum - Minimum > 256 * 1024) throw std::runtime_error("same-process content lifecycle retained memory grows");
		}
		const auto WorkerPreparation = Summarize(std::move(WorkerPreparationSamples));
		const auto TotalPreparation = Summarize(std::move(TotalPreparationSamples));
		const auto Commit = Summarize(std::move(CommitSamples));
		const auto Resident = Summarize(std::move(ResidentSamples));
		const auto MainStep = Summarize(std::move(MainStepSamples));
		std::cout << "[Content:Benchmark] serviceAdmission objects=" << ObjectCount
				  << " payloadBytes=" << PayloadBytes << " samples=" << Iterations
				  << " workerPreparationMeanMs=" << WorkerPreparation.Mean
				  << " workerPreparationP50Ms=" << WorkerPreparation.P50
				  << " workerPreparationP95Ms=" << WorkerPreparation.P95
				  << " workerPreparationP99Ms=" << WorkerPreparation.P99
				  << " workerPreparationMaxMs=" << WorkerPreparation.Maximum
				  << " totalPreparationMeanMs=" << TotalPreparation.Mean
				  << " totalPreparationP50Ms=" << TotalPreparation.P50
				  << " totalPreparationP95Ms=" << TotalPreparation.P95
				  << " totalPreparationP99Ms=" << TotalPreparation.P99
				  << " totalPreparationMaxMs=" << TotalPreparation.Maximum
				  << " commitMeanMs=" << Commit.Mean << " commitP50Ms=" << Commit.P50
				  << " commitP95Ms=" << Commit.P95 << " commitP99Ms=" << Commit.P99
				  << " commitMaxMs=" << Commit.Maximum
				  << " residentMeanMs=" << Resident.Mean << " residentP50Ms=" << Resident.P50
				  << " residentP95Ms=" << Resident.P95 << " residentP99Ms=" << Resident.P99
				  << " residentMaxMs=" << Resident.Maximum
				  << " mainStepMeanMs=" << MainStep.Mean << " mainStepP50Ms=" << MainStep.P50
				  << " mainStepP95Ms=" << MainStep.P95 << " mainStepP99Ms=" << MainStep.P99
				  << " mainStepMaxMs=" << MainStep.Maximum << '\n';
	}

}

int main(int ArgumentCount, char **Arguments) {
	try {
		BootstrapNativeRuntimeSchema();
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--lifecycle") {
			RunServiceAdmissionCase(512, 1024 * 1024, 100, true, true);
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--decoded-pressure") {
			RunDecodedPressureCase();
			return 0;
		}
		if (ArgumentCount > 1 && std::string_view(Arguments[1]) == "--decoded-overload") {
			RunDecodedPressureCase(false);
			return 0;
		}
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		for (const auto Count : Quick ? std::vector<std::size_t>{100, 1'000, 10'000}
									  : std::vector<std::size_t>{100, 1'000, 10'000, 32'768, 65'535, 65'536}) {
			RunParsedManifestCase(Count);
			std::cout.flush();
		}
		if (!Quick) for (const auto Count : std::vector<std::size_t>{65'537, 100'000, 1'000'000})
			RunRejectedManifestCase(Count);
		if (!Quick) RunRejectedManifestCase(65'537, true);
		for (const auto Objects : std::vector<std::size_t>{10, 100, 512, 1'000, 10'000})
			std::cout << "[Content:Benchmark] offeredObjects=" << Objects
					  << " admittedByUnitLimit=" << (Objects <= MaximumPackageContentObjectsPerUnit ? "true" : "false")
					  << " maximum=" << MaximumPackageContentObjectsPerUnit << '\n';
		for (const auto Objects : std::vector<std::size_t>{1, 10, 100, 256, 511, 512})
			RunContentPreparationCase("light-wide", Objects, 0, Quick ? 3 : 10);
		RunContentPreparationCase("deep-legal", 30, 0, Quick ? 3 : 10, false, true);
		RunContentPreparationCase("representative-parts", 512, 0, Quick ? 3 : 10, true);
		if (!Quick) {
			for (const auto [Objects, PayloadBytes] : std::vector<std::pair<std::size_t, std::size_t>>{
					 {128, 128 * 1024}, {128, 256 * 1024}, {256, 128 * 1024}, {256, 256 * 1024}, {256, 512 * 1024}})
				RunContentPreparationCase("mixed-string", Objects, PayloadBytes, 5, true);
			for (const auto PayloadBytes : std::vector<std::size_t>{1, 2, 4, 8})
				RunContentPreparationCase(
					PayloadBytes <= MaximumPackageContentPayloadBytes / (1024 * 1024)
						? "legal-max-string" : "rejected-legacy-candidate",
					512, PayloadBytes * 1024 * 1024, 3, true
				);
			auto OversizedManifest = MakeManifest(1);
			OversizedManifest.Entries.front().CompressedBytes = MaximumPackageContentPayloadBytes + 1;
			OversizedManifest.Entries.front().UncompressedBytes = MaximumPackageContentPayloadBytes + 1;
			auto Rejected = ParsePackageContentManifest(EncodePackageContentManifest(OversizedManifest));
			if (Rejected) throw std::runtime_error("content benchmark accepted the package payload maximum plus one");
			std::cout << "[Content:Benchmark] packagePayloadBytes=" << MaximumPackageContentPayloadBytes + 1
					  << " boundedReject=true maximum=" << MaximumPackageContentPayloadBytes << '\n';
			for (const auto PayloadBytes :
				std::vector<std::size_t>{256 * 1024, 512 * 1024, 1024 * 1024})
				RunServiceAdmissionCase(512, PayloadBytes, 10, true);
			for (const auto Objects : std::vector<std::size_t>{256, 384})
				RunServiceAdmissionCase(Objects, 1024 * 1024, 10, true);
		}
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:Benchmark] failed: " << Error.what() << '\n';
		return 1;
	}
}

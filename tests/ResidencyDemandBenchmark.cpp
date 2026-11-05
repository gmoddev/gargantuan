#include "gargantuan/content/ResidencyDemandPolicy.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
	struct AllocationHeader final {
		void *Raw = nullptr;
		std::size_t Bytes = 0;
		std::uint64_t Generation = 0;
	};

	std::atomic<std::uint64_t> AllocationGeneration{0};
	std::atomic<std::uint64_t> ActiveAllocationGeneration{0};
	std::atomic<std::uint64_t> AllocationCount{0};
	std::atomic<std::size_t> LiveAllocationBytes{0};
	std::atomic<std::size_t> PeakAllocationBytes{0};

	void *AllocateTracked(std::size_t Bytes, std::size_t Alignment) {
		Alignment = std::max(Alignment, alignof(AllocationHeader));
		if (Bytes > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) - Alignment)
			throw std::bad_alloc();
		void *Raw = std::malloc(Bytes + sizeof(AllocationHeader) + Alignment - 1);
		if (!Raw) throw std::bad_alloc();
		const auto Address = reinterpret_cast<std::uintptr_t>(Raw) + sizeof(AllocationHeader);
		const auto Aligned = (Address + Alignment - 1) & ~(static_cast<std::uintptr_t>(Alignment) - 1);
		auto *Header = reinterpret_cast<AllocationHeader *>(Aligned) - 1;
		const auto Generation = ActiveAllocationGeneration.load(std::memory_order_acquire);
		*Header = {Raw, Bytes, Generation};
		if (Generation != 0) {
			AllocationCount.fetch_add(1, std::memory_order_relaxed);
			const auto Live = LiveAllocationBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
			auto Peak = PeakAllocationBytes.load(std::memory_order_relaxed);
			while (Peak < Live && !PeakAllocationBytes.compare_exchange_weak(Peak, Live, std::memory_order_relaxed)) {}
		}
		return reinterpret_cast<void *>(Aligned);
	}

	void FreeTracked(void *Pointer) noexcept {
		if (!Pointer) return;
		auto *Header = reinterpret_cast<AllocationHeader *>(Pointer) - 1;
		if (Header->Generation != 0 &&
			Header->Generation == ActiveAllocationGeneration.load(std::memory_order_acquire))
			LiveAllocationBytes.fetch_sub(Header->Bytes, std::memory_order_relaxed);
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
	using Clock = std::chrono::steady_clock;

	struct AllocationSample final {
		std::uint64_t Count = 0;
		std::size_t PeakBytes = 0;
		std::size_t RetainedBytes = 0;
	};

	void StartAllocationSample() {
		AllocationCount.store(0, std::memory_order_relaxed);
		LiveAllocationBytes.store(0, std::memory_order_relaxed);
		PeakAllocationBytes.store(0, std::memory_order_relaxed);
		ActiveAllocationGeneration.store(AllocationGeneration.fetch_add(1, std::memory_order_relaxed) + 1,
			std::memory_order_release);
	}

	AllocationSample SnapshotAllocationSample() {
		return {
			.Count = AllocationCount.load(std::memory_order_relaxed),
			.PeakBytes = PeakAllocationBytes.load(std::memory_order_relaxed),
			.RetainedBytes = LiveAllocationBytes.load(std::memory_order_relaxed),
		};
	}

	AllocationSample FinishAllocationSample() {
		const auto Result = SnapshotAllocationSample();
		ActiveAllocationGeneration.store(0, std::memory_order_release);
		return Result;
	}

	struct Timings final {
		double Mean = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
		double Maximum = 0.0;
	};

	Timings Summarize(std::vector<double> Samples) {
		if (Samples.empty()) throw std::invalid_argument("[Content:ResidencyBenchmark] no samples");
		std::ranges::sort(Samples);
		const auto Percentile = [&](std::size_t Percent) { return Samples[(Samples.size() - 1) * Percent / 100]; };
		return {
			.Mean = std::accumulate(Samples.begin(), Samples.end(), 0.0) / Samples.size(),
			.P50 = Percentile(50), .P95 = Percentile(95), .P99 = Percentile(99), .Maximum = Samples.back(),
		};
	}

	double Microseconds(Clock::time_point StartedAt) {
		return std::chrono::duration<double, std::micro>(Clock::now() - StartedAt).count();
	}

	std::uint64_t ProcessResidentBytes() {
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX Counters{};
		Counters.cb = sizeof(Counters);
		if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&Counters), sizeof(Counters)))
			throw std::runtime_error("[Content:ResidencyReadiness] process memory query failed");
		return Counters.WorkingSetSize;
#elif defined(__linux__)
		const int File = ::open("/proc/self/statm", O_RDONLY);
		if (File < 0) throw std::runtime_error("[Content:ResidencyReadiness] process memory query failed");
		char Buffer[128]{};
		const auto Count = ::read(File, Buffer, sizeof(Buffer) - 1);
		::close(File);
		if (Count <= 0) throw std::runtime_error("[Content:ResidencyReadiness] process memory query failed");
		char *Next = nullptr;
		(void)std::strtoull(Buffer, &Next, 10);
		if (!Next || *Next != ' ') throw std::runtime_error("[Content:ResidencyReadiness] process memory query failed");
		const auto ResidentPages = std::strtoull(Next, nullptr, 10);
		return ResidentPages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#else
		return 0;
#endif
	}

	PackageContentManifest MakeManifest(std::size_t Count) {
		const std::array<std::uint8_t, 1> DigestInput{0x3a};
		const auto Digest = AssetContentId::Hash(DigestInput);
		PackageContentManifest Manifest{
			.Package = {*ProjectId::Parse("0123456789abcdef0123456789abcdef"), 17},
			.InstanceSchemaVersion = PackageContentInstanceSchemaVersion,
		};
		Manifest.Entries.reserve(Count);
		const auto Width = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto X = static_cast<float>(Index % Width) * 256.0f + 500.0f;
			const auto Z = static_cast<float>(Index / Width) * 256.0f + 500.0f;
			Manifest.Entries.push_back({
				.Key = std::format("region/{:05}", Index),
				.BlobReference = std::format("content/regions/{:05}.instance.json", Index),
				.Digest = Digest,
				.CompressedBytes = 1,
				.UncompressedBytes = 1,
				.ObjectCount = 1,
				.Dependencies = {},
				.PackageSpaceKey = "default",
				.CoarseBounds = PackageCoarseBounds{{X, 0.0f, Z}, {X + 16.0f, 0.0f, Z + 16.0f}},
				.Flags = PackageContentFlags::ImmutableBaseline,
			});
		}
		return Manifest;
	}

	glm::vec3 FocusPosition(std::size_t Focus, std::size_t FocusCount, std::size_t EntryCount) {
		const auto Index = Focus * EntryCount / FocusCount;
		const auto Width = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(EntryCount))));
		return {static_cast<float>(Index % Width) * 256.0f + 508.0f, 0.0f,
			static_cast<float>(Index / Width) * 256.0f + 508.0f};
	}

	void PrintEmptyColumns(std::size_t Count) {
		for (std::size_t Index = 0; Index < Count; ++Index) std::cout << ',';
	}

	// Deliberately linear reference. This is a geometry-only scan, not a second policy.
	void RunLinearReference(const PackageContentManifest &Manifest, std::size_t FocusCount, std::size_t Samples) {
		std::vector<double> Durations;
		Durations.reserve(Samples);
		std::size_t LastHits = 0;
		for (std::size_t Sample = 0; Sample < Samples; ++Sample) {
			const auto StartedAt = Clock::now();
			std::size_t Hits = 0;
			for (std::size_t Focus = 0; Focus < FocusCount; ++Focus) {
				const auto Position = FocusPosition(Focus, FocusCount, Manifest.Entries.size());
				for (const auto &Entry : Manifest.Entries) {
					const auto &Bounds = *Entry.CoarseBounds;
					const auto Dx = std::max({Bounds.Minimum[0] - Position.x, 0.0f, Position.x - Bounds.Maximum[0]});
					const auto Dz = std::max({Bounds.Minimum[2] - Position.z, 0.0f, Position.z - Bounds.Maximum[2]});
					Hits += Dx * Dx + Dz * Dz <= 320.0f * 320.0f;
				}
			}
			LastHits = Hits;
			Durations.push_back(Microseconds(StartedAt));
		}
		const auto Time = Summarize(std::move(Durations));
		std::cout << "linear_reference," << Manifest.Entries.size() << ',' << FocusCount << ',' << Samples
			<< ',' << Time.Mean << ',' << Time.P50 << ',' << Time.P95 << ',' << Time.P99 << ',' << Time.Maximum;
		PrintEmptyColumns(18);
		std::cout << LastHits << '\n';
	}

	void CheckWorkBound(const ResidencyDemandPolicyMetrics &Before, const ResidencyDemandPolicyMetrics &After,
		const ResidencyDemandPolicyLimits &Limits) {
		const auto Visits = After.QueryMembershipVisits - Before.QueryMembershipVisits;
		const auto Revalidations = After.CandidateRevalidations - Before.CandidateRevalidations;
		const auto Ranking = After.RankingOperations - Before.RankingOperations;
		const auto RankingDepth = std::max<std::uint64_t>(1, std::bit_width(Limits.MaximumCandidatesPerFocus));
		const auto RankingBound = (Visits + Revalidations +
			8ull * Limits.MaximumFocusEvaluationsPerTick * Limits.MaximumCandidatesPerFocus) * RankingDepth;
		if (After.QueryCellsVisited - Before.QueryCellsVisited > Limits.MaximumQueryCellsPerTick ||
			Visits > Limits.MaximumMembershipVisitsPerTick ||
			Revalidations > Limits.MaximumCandidateRevalidationsPerTick ||
			After.RequestValidationVisits - Before.RequestValidationVisits > Limits.MaximumRequestValidationVisitsPerTick ||
			Ranking > RankingBound ||
			After.CompletedFocusEvaluations - Before.CompletedFocusEvaluations > Limits.MaximumFocusEvaluationsPerTick)
			throw std::runtime_error("[Content:ResidencyBenchmark] policy exceeded a per-step work bound");
	}

	void PrintPolicyRow(std::string_view Phase, std::size_t EntryCount, std::size_t FocusCount,
		const std::vector<double> &Samples, const AllocationSample &Allocations,
		const ResidencyDemandPolicyMetrics &Before, const ResidencyDemandPolicyMetrics &After) {
		const auto Time = Summarize(Samples);
		std::cout << Phase << ',' << EntryCount << ',' << FocusCount << ',' << Samples.size() << ','
			<< Time.Mean << ',' << Time.P50 << ',' << Time.P95 << ',' << Time.P99 << ',' << Time.Maximum << ','
			<< Allocations.Count << ',' << Allocations.PeakBytes << ',' << Allocations.RetainedBytes << ','
			<< After.QueryPages - Before.QueryPages << ','
			<< After.QueryCellsVisited - Before.QueryCellsVisited << ','
			<< After.QueryMembershipVisits - Before.QueryMembershipVisits << ','
			<< After.CompletedFocusEvaluations - Before.CompletedFocusEvaluations << ','
			<< After.StaleRevisions - Before.StaleRevisions << ','
			<< After.MovingSweepCompletions - Before.MovingSweepCompletions << ','
			<< After.CandidateRevalidations - Before.CandidateRevalidations << ','
			<< After.RequestValidationVisits - Before.RequestValidationVisits << ','
			<< After.RankingOperations - Before.RankingOperations << ','
			<< After.Requests - Before.Requests << ',' << After.Releases - Before.Releases << ','
			<< After.RetainedCandidates << ',' << After.DemandRecords << ',' << After.PendingDeltas << ",0\n";
	}

	void RunPolicyCase(std::size_t EntryCount, std::size_t FocusCount, bool Quick) {
		const auto Manifest = MakeManifest(EntryCount);
		ResidencyDemandPolicyLimits Limits;
		Limits.MaximumFocuses = 512;
		std::vector<double> Samples;
		Samples.reserve(Quick ? 12 : 64);
		StartAllocationSample();
		const auto BuildStartedAt = Clock::now();
		ResidencyDemandPolicy Policy(Manifest, Limits);
		const auto BuildMicroseconds = Microseconds(BuildStartedAt);
		const auto BuildAllocations = SnapshotAllocationSample();
		for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
			if (!Policy.UpdateFocus(Focus + 1, FocusPosition(Focus, FocusCount, EntryCount)))
				throw std::runtime_error("[Content:ResidencyBenchmark] focus rejected");
		std::uint64_t Tick = 1;
		for (std::size_t Warm = 0; Warm < (Quick ? 64u : 128u); ++Warm) {
			const auto Before = Policy.GetMetrics();
			Policy.Step(nullptr, Tick++);
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		if (const auto WarmMetrics = Policy.GetMetrics();
			WarmMetrics.PendingFocuses != 0 || WarmMetrics.QueryFailures != 0 ||
			WarmMetrics.DemandRecords > EntryCount)
			throw std::runtime_error("[Content:ResidencyBenchmark] warm policy state did not converge");
		const auto StateMicroseconds = Microseconds(BuildStartedAt);
		const auto StateAllocations = FinishAllocationSample();
		const auto PrintAllocationRow = [&](std::string_view Phase, double Duration, const AllocationSample &Allocations) {
			std::cout << Phase << ',' << EntryCount << ',' << FocusCount << ",1," << Duration
				<< ',' << Duration << ',' << Duration << ',' << Duration << ',' << Duration << ','
				<< Allocations.Count << ',' << Allocations.PeakBytes << ',' << Allocations.RetainedBytes;
			PrintEmptyColumns(15);
			std::cout << "0\n";
		};
		PrintAllocationRow("policy_build", BuildMicroseconds, BuildAllocations);
		PrintAllocationRow("policy_state", StateMicroseconds, StateAllocations);
		const auto Iterations = Quick ? 12u : 64u;
		const auto SteadyBefore = Policy.GetMetrics();
		StartAllocationSample();
		for (std::size_t Sample = 0; Sample < Iterations; ++Sample) {
			const auto Before = Policy.GetMetrics();
			const auto StartedAt = Clock::now();
			Policy.Step(nullptr, Tick++);
			Samples.push_back(Microseconds(StartedAt));
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto SteadyAllocations = FinishAllocationSample();
		PrintPolicyRow("policy_steady", EntryCount, FocusCount, Samples, SteadyAllocations,
			SteadyBefore, Policy.GetMetrics());
		Samples.clear();
		const auto ChurnBefore = Policy.GetMetrics();
		StartAllocationSample();
		for (std::size_t Sample = 0; Sample < Iterations; ++Sample) {
			const auto Focus = Sample % FocusCount;
			const auto Position = FocusPosition(Focus, FocusCount, EntryCount) +
				glm::vec3(Sample % 2 == 0 ? 640.0f : -640.0f, 0.0f, 0.0f);
			const auto Before = Policy.GetMetrics();
			const auto StartedAt = Clock::now();
			if (!Policy.UpdateFocus(Focus + 1, Position))
				throw std::runtime_error("[Content:ResidencyBenchmark] focus update rejected");
			Policy.Step(nullptr, Tick++);
			Samples.push_back(Microseconds(StartedAt));
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto ChurnAllocations = FinishAllocationSample();
		PrintPolicyRow("policy_churn", EntryCount, FocusCount, Samples, ChurnAllocations,
			ChurnBefore, Policy.GetMetrics());
		Samples.clear();
		const auto MovingBefore = Policy.GetMetrics();
		StartAllocationSample();
		for (std::size_t Sample = 0; Sample < Iterations; ++Sample) {
			const auto Before = Policy.GetMetrics();
			const auto StartedAt = Clock::now();
			for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
				if (!Policy.UpdateFocus(Focus + 1, FocusPosition(Focus, FocusCount, EntryCount) +
					glm::vec3(Sample % 2 == 0 ? 1.0f : -1.0f, 0, 0), {32, 0, 0}))
					throw std::runtime_error("[Content:ResidencyBenchmark] moving focus rejected");
			Policy.Step(nullptr, Tick++);
			Samples.push_back(Microseconds(StartedAt));
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto MovingAllocations = FinishAllocationSample();
		PrintPolicyRow("policy_all_moving", EntryCount, FocusCount, Samples, MovingAllocations,
			MovingBefore, Policy.GetMetrics());
		const auto CleanupBefore = Policy.GetMetrics();
		const auto CleanupStartedAt = Clock::now();
		for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
			if (!Policy.RemoveFocus(Focus + 1))
				throw std::runtime_error("[Content:ResidencyBenchmark] focus removal rejected");
		for (std::size_t Step = 0; Step < Limits.GraceTicks + 64; ++Step) {
			const auto Before = Policy.GetMetrics();
			Policy.Step(nullptr, Tick++);
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto CleanupMicroseconds = Microseconds(CleanupStartedAt);
		const auto CleanupAfter = Policy.GetMetrics();
		if (CleanupAfter.RetainedCandidates != 0 || CleanupAfter.DemandRecords != 0 ||
			CleanupAfter.PendingDeltas != 0)
			throw std::runtime_error("[Content:ResidencyBenchmark] policy retained removed focus state");
		std::cout << "policy_cleanup," << EntryCount << ',' << FocusCount << ",1," << CleanupMicroseconds
			<< ',' << CleanupMicroseconds << ',' << CleanupMicroseconds << ',' << CleanupMicroseconds << ','
			<< CleanupMicroseconds << ",,,," << CleanupAfter.QueryPages - CleanupBefore.QueryPages << ','
			<< CleanupAfter.QueryCellsVisited - CleanupBefore.QueryCellsVisited << ','
			<< CleanupAfter.QueryMembershipVisits - CleanupBefore.QueryMembershipVisits << ','
			<< CleanupAfter.CompletedFocusEvaluations - CleanupBefore.CompletedFocusEvaluations << ','
			<< CleanupAfter.StaleRevisions - CleanupBefore.StaleRevisions << ','
			<< CleanupAfter.MovingSweepCompletions - CleanupBefore.MovingSweepCompletions << ','
			<< CleanupAfter.CandidateRevalidations - CleanupBefore.CandidateRevalidations << ','
			<< CleanupAfter.RequestValidationVisits - CleanupBefore.RequestValidationVisits << ','
			<< CleanupAfter.RankingOperations - CleanupBefore.RankingOperations << ','
			<< CleanupAfter.Requests - CleanupBefore.Requests << ','
			<< CleanupAfter.Releases - CleanupBefore.Releases << ',' << CleanupAfter.RetainedCandidates << ','
			<< CleanupAfter.DemandRecords << ',' << CleanupAfter.PendingDeltas << ",0\n";
	}

	void RunStaleCursorCase(bool Quick) {
		auto Manifest = MakeManifest(1'000);
		for (auto &Entry : Manifest.Entries)
			Entry.CoarseBounds = PackageCoarseBounds{{500.0f, 0.0f, 500.0f}, {516.0f, 0.0f, 516.0f}};
		ResidencyDemandPolicyLimits Limits;
		ResidencyDemandPolicy Policy(Manifest, Limits);
		if (!Policy.UpdateFocus(1, {508.0f, 0.0f, 508.0f}))
			throw std::runtime_error("[Content:ResidencyBenchmark] stale focus rejected");
		Policy.Step(nullptr, 1);
		if (Policy.GetMetrics().PendingFocuses != 1)
			throw std::runtime_error("[Content:ResidencyBenchmark] dense query did not retain a cursor");
		const auto Iterations = Quick ? 32u : 64u;
		std::vector<double> Samples;
		Samples.reserve(Iterations);
		const auto BeforePhase = Policy.GetMetrics();
		StartAllocationSample();
		for (std::size_t Sample = 0; Sample < Iterations; ++Sample) {
			const auto Before = Policy.GetMetrics();
			const auto StartedAt = Clock::now();
			if (!Policy.UpdateFocus(1, {509.0f + static_cast<float>(Sample % 2), 0.0f, 508.0f}))
				throw std::runtime_error("[Content:ResidencyBenchmark] stale focus update rejected");
			Policy.Step(nullptr, Sample + 2);
			Samples.push_back(Microseconds(StartedAt));
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto Allocations = FinishAllocationSample();
		const auto AfterPhase = Policy.GetMetrics();
		if (AfterPhase.StaleRevisions != BeforePhase.StaleRevisions ||
			AfterPhase.MovingSweepCompletions == BeforePhase.MovingSweepCompletions ||
			AfterPhase.CompletedFocusEvaluations == BeforePhase.CompletedFocusEvaluations ||
			AfterPhase.DemandRecords == 0 || Policy.GetDemands().empty())
			throw std::runtime_error("[Content:ResidencyBenchmark] moving dense focus made no completed demand progress");
		PrintPolicyRow("policy_moving_cursor", Manifest.Entries.size(), 1, Samples, Allocations, BeforePhase, AfterPhase);
		std::uint64_t NextTick = Iterations + 2;
		const auto SettleDeadline = NextTick + 64;
		for (; NextTick < SettleDeadline && Policy.GetMetrics().PendingFocuses != 0; ++NextTick) {
			const auto Before = Policy.GetMetrics();
			Policy.Step(nullptr, NextTick);
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
		}
		const auto Settled = Policy.GetMetrics();
		if (Settled.PendingFocuses != 0 || Settled.DemandRecords == 0 || Settled.RetainedCandidates == 0 ||
			Policy.GetDemands().empty())
			throw std::runtime_error("[Content:ResidencyBenchmark] moving dense focus did not converge after updates stopped");
		if (!Policy.UpdateFocus(1, {509.5f, 0.0f, 508.0f}))
			throw std::runtime_error("[Content:ResidencyBenchmark] restart focus update rejected");
		Policy.Step(nullptr, NextTick++);
		if (Policy.GetMetrics().PendingFocuses != 1)
			throw std::runtime_error("[Content:ResidencyBenchmark] restart did not retain a paged cursor");
		const auto BeforeTeleport = Policy.GetMetrics();
		std::vector<double> TeleportSamples;
		TeleportSamples.reserve(1);
		StartAllocationSample();
		const auto TeleportStartedAt = Clock::now();
		if (!Policy.UpdateFocus(1, {5'000.0f, 0.0f, 5'000.0f}))
			throw std::runtime_error("[Content:ResidencyBenchmark] teleport focus update rejected");
		Policy.Step(nullptr, NextTick++);
		TeleportSamples.push_back(Microseconds(TeleportStartedAt));
		CheckWorkBound(BeforeTeleport, Policy.GetMetrics(), Limits);
		const auto TeleportAllocations = FinishAllocationSample();
		const auto AfterTeleport = Policy.GetMetrics();
		if (AfterTeleport.StaleRevisions != BeforeTeleport.StaleRevisions + 1)
			throw std::runtime_error("[Content:ResidencyBenchmark] teleport did not invalidate old cursor");
		PrintPolicyRow("policy_teleport_restart", Manifest.Entries.size(), 1, TeleportSamples,
			TeleportAllocations, BeforeTeleport, AfterTeleport);
	}

	class BenchmarkContentProvider final : public IContentAvailabilityProvider {
	  public:
		std::string Manifest;
		ContentPayload Payload;
		[[nodiscard]] std::string_view Name() const override { return "residency-benchmark"; }
		[[nodiscard]] ContentManifestProviderResult GetManifest(
			const ContentRequestContext &, const PackageContentNamespace &
		) override { return Manifest; }
		[[nodiscard]] ContentPayloadProviderResult GetContent(
			const ContentRequestContext &, const PackageContentIdentity &Identity
		) override {
			auto Result = Payload;
			Result.Identity = Identity;
			return Result;
		}
	};

	void RunServiceIntegration() {
		std::shared_ptr<Instance> Root = std::make_shared<Folder>();
		Root->SetArchivable(true);
		Root->SetName("");
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		auto Manifest = MakeManifest(2);
		const auto Digest = AssetContentId::Hash(*Bytes);
		for (auto &Entry : Manifest.Entries) {
			Entry.Digest = Digest;
			Entry.CompressedBytes = Bytes->size();
			Entry.UncompressedBytes = Bytes->size();
		}
		Manifest.Entries.front().Dependencies = {Manifest.Entries.back().Key};
		auto Provider = std::make_shared<BenchmarkContentProvider>();
		Provider->Manifest = EncodePackageContentManifest(Manifest);
		Provider->Payload = {{Manifest.Package, ""}, Digest, Bytes};
		const auto ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Provider->Manifest.data()), Provider->Manifest.size()));
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		ContentAvailabilityService Service(World, WorkspaceValue, {
			.Provider = Provider, .Package = Manifest.Package, .ManifestDigest = ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		});
		const auto Deadline = Clock::now() + std::chrono::seconds(10);
		while (!Service.IsManifestAvailable() && Clock::now() < Deadline) {
			Service.Step();
			std::this_thread::yield();
		}
		if (!Service.IsManifestAvailable()) throw std::runtime_error("[Content:ResidencyBenchmark] service manifest unavailable");
		ResidencyDemandPolicy Policy(Manifest);
		if (!Policy.SetRequirement(Manifest.Entries.front().Key, ResidencyDemandClass::Critical) ||
			!Policy.SetRequirement(Manifest.Entries.back().Key, ResidencyDemandClass::Gameplay) ||
			!Policy.UpdateFocus(1, {508.0f, 0.0f, 508.0f}) ||
			!Policy.UpdateFocus(2, {508.0f, 0.0f, 508.0f}))
			throw std::runtime_error("[Content:ResidencyBenchmark] service requirements rejected");
		std::vector<double> PolicySamples;
		std::vector<double> ServiceSamples;
		PolicySamples.reserve(4'096);
		ServiceSamples.reserve(4'096);
		std::uint64_t Tick = 1;
		while ((Service.GetState(Manifest.Entries.front().Key) != ContentResidencyState::Resident ||
			Service.GetState(Manifest.Entries.back().Key) != ContentResidencyState::Resident) && Clock::now() < Deadline) {
			const auto PolicyStartedAt = Clock::now();
			Policy.Step(&Service, Tick++);
			if (PolicySamples.size() < 4'096) PolicySamples.push_back(Microseconds(PolicyStartedAt));
			const auto ServiceStartedAt = Clock::now();
			Service.Step();
			if (ServiceSamples.size() < 4'096) ServiceSamples.push_back(Microseconds(ServiceStartedAt));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto Metrics = Service.GetMetrics();
		if (Metrics.ResidentUnits != 2 || Metrics.Acquisitions != 2 || Metrics.Admissions != 2 || Metrics.Failures != 0)
			throw std::runtime_error("[Content:ResidencyBenchmark] real 3L acquisition/admission failed");
		const auto PolicyMetrics = Policy.GetMetrics();
		if (PolicyMetrics.Requests != 2 || PolicyMetrics.DemandRecords != 2 ||
			PolicyMetrics.RetainedCandidates < 4 || Policy.GetDemands().size() != 2)
			throw std::runtime_error("[Content:ResidencyBenchmark] direct dependency demand was not coalesced");
		const auto PolicyTime = Summarize(std::move(PolicySamples));
		const auto ServiceTime = Summarize(std::move(ServiceSamples));
		std::cerr << "[Content:ResidencyService] sampledSteps=" << std::min<std::size_t>(Tick - 1, 4'096)
			<< " policyMeanUs=" << PolicyTime.Mean
			<< " policyP50Us=" << PolicyTime.P50 << " policyP95Us=" << PolicyTime.P95
			<< " policyP99Us=" << PolicyTime.P99 << " policyMaxUs=" << PolicyTime.Maximum
			<< " serviceMeanUs=" << ServiceTime.Mean << " serviceP50Us=" << ServiceTime.P50
			<< " serviceP95Us=" << ServiceTime.P95 << " serviceP99Us=" << ServiceTime.P99
			<< " serviceMaxUs=" << ServiceTime.Maximum << " policyRequests=" << PolicyMetrics.Requests
			<< " serviceRequests=" << Metrics.Requests << " acquisitions=" << Metrics.Acquisitions
			<< " acquisitionDeduplications=" << Metrics.AcquisitionDeduplications
			<< " admissions=" << Metrics.Admissions << '\n';
		if (!Policy.RemoveFocus(1) || !Policy.RemoveFocus(2) ||
			!Policy.ClearRequirement(Manifest.Entries.front().Key) ||
			!Policy.ClearRequirement(Manifest.Entries.back().Key))
			throw std::runtime_error("[Content:ResidencyBenchmark] service requirements did not clear");
		const auto ReleasedAt = Policy.GetMetrics().Releases;
		for (std::size_t Grace = 1; Grace < ResidencyDemandPolicyLimits{}.GraceTicks; ++Grace) {
			Policy.Step(&Service, Tick++);
			Service.Step();
		}
		if (Policy.GetMetrics().Releases != ReleasedAt)
			throw std::runtime_error("[Content:ResidencyBenchmark] demand released before grace elapsed");
		Policy.Step(&Service, Tick++);
		if (Policy.GetMetrics().Releases != ReleasedAt + 2)
			throw std::runtime_error("[Content:ResidencyBenchmark] demand did not release after grace");
		std::cerr << "[Content:ResidencyService] graceTicks=" << ResidencyDemandPolicyLimits{}.GraceTicks
			<< " releases=" << Policy.GetMetrics().Releases << '\n';
		Policy.Stop(&Service);
		Service.Stop();
	}

	void RunServiceScale(std::size_t EntryCount, std::size_t FocusCount) {
		std::shared_ptr<Instance> Root = std::make_shared<Folder>();
		Root->SetArchivable(true);
		Root->SetName("");
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		auto Manifest = MakeManifest(EntryCount);
		const auto Digest = AssetContentId::Hash(*Bytes);
		for (auto &Entry : Manifest.Entries) {
			Entry.Digest = Digest;
			Entry.CompressedBytes = Bytes->size();
			Entry.UncompressedBytes = Bytes->size();
		}
		Manifest.Entries.front().Dependencies = {Manifest.Entries[1].Key};
		auto Provider = std::make_shared<BenchmarkContentProvider>();
		const auto EncodeStartedAt = Clock::now();
		Provider->Manifest = EncodePackageContentManifest(Manifest);
		const auto EncodeMicroseconds = Microseconds(EncodeStartedAt);
		Provider->Payload = {{Manifest.Package, ""}, Digest, Bytes};
		const auto ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Provider->Manifest.data()), Provider->Manifest.size()));
		auto World = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		const auto StartupStartedAt = Clock::now();
		ContentAvailabilityService Service(World, WorkspaceValue, {
			.Provider = Provider, .Package = Manifest.Package, .ManifestDigest = ManifestDigest,
			.Mode = ContentResidencyMode::OnDemand,
		});
		const auto ConstructMicroseconds = Microseconds(StartupStartedAt);
		const auto Deadline = Clock::now() + std::chrono::seconds(30);
		while (!Service.IsManifestAvailable() && Clock::now() < Deadline) {
			Service.Step();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto ManifestReadyMicroseconds = Microseconds(StartupStartedAt);
		const auto *ParsedManifest = Service.GetManifest();
		if (!ParsedManifest || ParsedManifest->Entries.size() != EntryCount)
			throw std::runtime_error("[Content:ResidencyBenchmark] scale manifest unavailable");
		ResidencyDemandPolicyLimits Limits;
		Limits.EnterRadius = 8.0f;
		Limits.LeaveRadius = 16.0f;
		Limits.LookaheadSeconds = 0.0f;
		const auto PolicyBuildStartedAt = Clock::now();
		ResidencyDemandPolicy Policy(*ParsedManifest, Limits);
		const auto PolicyBuildMicroseconds = Microseconds(PolicyBuildStartedAt);
		for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
			if (!Policy.UpdateFocus(Focus + 1, {508.0f, 0.0f, 508.0f}))
				throw std::runtime_error("[Content:ResidencyBenchmark] scale focus rejected");
		for (std::size_t Unit = 0; Unit < 8; ++Unit)
			if (!Policy.SetRequirement(ParsedManifest->Entries[Unit].Key, ResidencyDemandClass::Critical))
				throw std::runtime_error("[Content:ResidencyBenchmark] scale requirement rejected");
		std::vector<double> PolicySamples;
		std::vector<double> ServiceSamples;
		PolicySamples.reserve(31);
		ServiceSamples.reserve(31);
		double FirstPolicyStepMicroseconds = 0.0;
		double FirstServiceStepMicroseconds = 0.0;
		for (std::uint64_t Tick = 1; Tick <= 32; ++Tick) {
			const auto Before = Policy.GetMetrics();
			const auto PolicyStartedAt = Clock::now();
			Policy.Step(&Service, Tick);
			const auto PolicyStepMicroseconds = Microseconds(PolicyStartedAt);
			if (Tick == 1) FirstPolicyStepMicroseconds = PolicyStepMicroseconds;
			else PolicySamples.push_back(PolicyStepMicroseconds);
			CheckWorkBound(Before, Policy.GetMetrics(), Limits);
			const auto ServiceStartedAt = Clock::now();
			Service.Step();
			const auto ServiceStepMicroseconds = Microseconds(ServiceStartedAt);
			if (Tick == 1) FirstServiceStepMicroseconds = ServiceStepMicroseconds;
			else ServiceSamples.push_back(ServiceStepMicroseconds);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto TimedPolicyMetrics = Policy.GetMetrics();
		const auto TimedServiceMetrics = Service.GetMetrics();
		if (TimedPolicyMetrics.PendingFocuses != 0 || TimedPolicyMetrics.QueryFailures != 0 ||
			TimedPolicyMetrics.Requests != 8 || TimedPolicyMetrics.DemandRecords != 8)
			throw std::runtime_error("[Content:ResidencyBenchmark] scale demand did not converge within 32 ticks");
		for (std::uint64_t Tick = 33; Service.GetMetrics().ResidentUnits != 8 && Clock::now() < Deadline; ++Tick) {
			Policy.Step(&Service, Tick);
			Service.Step();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto FinalMetrics = Service.GetMetrics();
		if (FinalMetrics.ResidentUnits != 8 || FinalMetrics.Acquisitions != 8 ||
			FinalMetrics.Admissions != 8 || FinalMetrics.Failures != 0)
			throw std::runtime_error("[Content:ResidencyBenchmark] scale 3L admission did not converge");
		const auto PolicyTime = Summarize(std::move(PolicySamples));
		const auto ServiceTime = Summarize(std::move(ServiceSamples));
		std::cerr << "[Content:ResidencyScale] entries=" << EntryCount << " focuses=" << FocusCount
			<< " manifestBytes=" << Provider->Manifest.size() << " encodeUs=" << EncodeMicroseconds
			<< " serviceConstructUs=" << ConstructMicroseconds
			<< " manifestReadyUs=" << ManifestReadyMicroseconds
			<< " policyBuildUs=" << PolicyBuildMicroseconds
			<< " policyFirstStepUs=" << FirstPolicyStepMicroseconds
			<< " serviceFirstStepUs=" << FirstServiceStepMicroseconds
			<< " sampledHotTicks=31 policyMeanUs=" << PolicyTime.Mean
			<< " policyP50Us=" << PolicyTime.P50
			<< " policyP95Us=" << PolicyTime.P95 << " policyP99Us=" << PolicyTime.P99
			<< " policyMaxUs=" << PolicyTime.Maximum
			<< " serviceMeanUs=" << ServiceTime.Mean
			<< " serviceP50Us=" << ServiceTime.P50 << " serviceP95Us=" << ServiceTime.P95
			<< " serviceP99Us=" << ServiceTime.P99 << " serviceMaxUs=" << ServiceTime.Maximum
			<< " queryPages=" << TimedPolicyMetrics.QueryPages
			<< " queryCells=" << TimedPolicyMetrics.QueryCellsVisited
			<< " membershipVisits=" << TimedPolicyMetrics.QueryMembershipVisits
			<< " retainedCandidates=" << TimedPolicyMetrics.RetainedCandidates
			<< " serviceRequestsAt32=" << TimedServiceMetrics.Requests
			<< " admissionsAt32=" << TimedServiceMetrics.Admissions
			<< " finalAcquisitions=" << FinalMetrics.Acquisitions
			<< " finalAdmissions=" << FinalMetrics.Admissions << '\n';
		Policy.Stop(&Service);
		Service.Stop();
	}

	struct InMemoryServiceFixture final {
		std::shared_ptr<Instance> Root;
		std::shared_ptr<const std::vector<std::uint8_t>> PayloadBytes;
		PackageContentManifest InputManifest;
		std::shared_ptr<BenchmarkContentProvider> Provider;
		std::shared_ptr<DataModel> World;
		std::shared_ptr<Workspace> WorkspaceValue;
		std::unique_ptr<ContentAvailabilityService> Service;
		std::shared_ptr<const PackageContentManifest> Snapshot;
		double ManifestBuildMicroseconds = 0.0;
		double ManifestEncodeMicroseconds = 0.0;
		double ServiceConstructMicroseconds = 0.0;
		double ProviderReadyMicroseconds = 0.0;
		double SnapshotAcquireMicroseconds = 0.0;
	};

	InMemoryServiceFixture MakeInMemoryServiceFixture(std::size_t EntryCount) {
		InMemoryServiceFixture Fixture;
		Fixture.Root = std::make_shared<Folder>();
		Fixture.Root->SetArchivable(true);
		Fixture.Root->SetName("");
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Fixture.Root);
		Fixture.PayloadBytes = std::make_shared<const std::vector<std::uint8_t>>(Encoded.begin(), Encoded.end());
		const auto ManifestStartedAt = Clock::now();
		Fixture.InputManifest = MakeManifest(EntryCount);
		const auto Digest = AssetContentId::Hash(*Fixture.PayloadBytes);
		for (auto &Entry : Fixture.InputManifest.Entries) {
			Entry.Digest = Digest;
			Entry.CompressedBytes = Fixture.PayloadBytes->size();
			Entry.UncompressedBytes = Fixture.PayloadBytes->size();
		}
		Fixture.InputManifest.Entries.front().Dependencies = {Fixture.InputManifest.Entries[1].Key};
		Fixture.ManifestBuildMicroseconds = Microseconds(ManifestStartedAt);
		Fixture.Provider = std::make_shared<BenchmarkContentProvider>();
		const auto EncodeStartedAt = Clock::now();
		Fixture.Provider->Manifest = EncodePackageContentManifest(Fixture.InputManifest);
		Fixture.ManifestEncodeMicroseconds = Microseconds(EncodeStartedAt);
		Fixture.Provider->Payload = {{Fixture.InputManifest.Package, ""}, Digest, Fixture.PayloadBytes};
		const auto ManifestDigest = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Fixture.Provider->Manifest.data()), Fixture.Provider->Manifest.size()));
		Fixture.World = std::make_shared<DataModel>();
		Fixture.WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Fixture.World->GetService("Workspace"));
		const auto ReadyStartedAt = Clock::now();
		Fixture.Service = std::make_unique<ContentAvailabilityService>(Fixture.World, Fixture.WorkspaceValue,
			ContentAvailabilityConfiguration{
				.Provider = Fixture.Provider, .Package = Fixture.InputManifest.Package,
				.ManifestDigest = ManifestDigest, .Mode = ContentResidencyMode::OnDemand,
			});
		Fixture.ServiceConstructMicroseconds = Microseconds(ReadyStartedAt);
		const auto Deadline = Clock::now() + std::chrono::seconds(30);
		while (!Fixture.Service->IsManifestAvailable() && Clock::now() < Deadline) {
			Fixture.Service->Step();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		Fixture.ProviderReadyMicroseconds = Microseconds(ReadyStartedAt);
		if (!Fixture.Service->IsManifestAvailable())
			throw std::runtime_error("[Content:ResidencyReadiness] in-memory provider manifest unavailable");
		const auto SnapshotStartedAt = Clock::now();
		Fixture.Snapshot = Fixture.Service->GetManifestSnapshot();
		Fixture.SnapshotAcquireMicroseconds = Microseconds(SnapshotStartedAt);
		if (!Fixture.Snapshot || Fixture.Snapshot->Entries.size() != EntryCount)
			throw std::runtime_error("[Content:ResidencyReadiness] service manifest snapshot unavailable");
		return Fixture;
	}

	void RunStartupComparison(std::size_t EntryCount, std::size_t Repetitions) {
		auto Fixture = MakeInMemoryServiceFixture(EntryCount);
		std::cout << "[Content:ResidencyStartup] phase=manifest entryCount=" << EntryCount
			<< " manifestBuildUs=" << Fixture.ManifestBuildMicroseconds
			<< " manifestEncodeUs=" << Fixture.ManifestEncodeMicroseconds
			<< " manifestBytes=" << Fixture.Provider->Manifest.size()
			<< " serviceConstructUs=" << Fixture.ServiceConstructMicroseconds
			<< " providerReadyUs=" << Fixture.ProviderReadyMicroseconds
			<< " snapshotAcquireUs=" << Fixture.SnapshotAcquireMicroseconds
			<< " provider=in-memory" << '\n';
		for (std::size_t Repetition = 0; Repetition < Repetitions; ++Repetition)
			for (const bool Shared : {false, true}) {
				StartAllocationSample();
				const auto ConstructStartedAt = Clock::now();
				std::unique_ptr<ResidencyDemandPolicy> Policy;
				if (Shared) Policy = std::make_unique<ResidencyDemandPolicy>(Fixture.Snapshot);
				else Policy = std::make_unique<ResidencyDemandPolicy>(*Fixture.Snapshot);
				const auto ConstructMicroseconds = Microseconds(ConstructStartedAt);
				const auto ConstructAllocations = SnapshotAllocationSample();
				const auto BindingStartedAt = Clock::now();
				Policy->Step(Fixture.Service.get(), 1);
				const auto BindingMicroseconds = Microseconds(BindingStartedAt);
				const auto Metrics = Policy->GetMetrics();
				const auto BindingAllocations = FinishAllocationSample();
				if (Metrics.ServiceBindingFailures != 0 ||
					Metrics.ManifestBindingEntriesCompared != (Shared ? 0u : EntryCount))
					throw std::runtime_error("[Content:ResidencyReadiness] startup binding comparison mismatch");
				std::cout << "[Content:ResidencyStartup] phase=policy entryCount=" << EntryCount
					<< " repetition=" << Repetition << " constructor=" << (Shared ? "shared-snapshot" : "value-compat")
					<< " constructorUs=" << ConstructMicroseconds
					<< " constructorAllocations=" << ConstructAllocations.Count
					<< " constructorRetainedBytes=" << ConstructAllocations.RetainedBytes
					<< " firstBindingUs=" << BindingMicroseconds
					<< " bindingEntriesCompared=" << Metrics.ManifestBindingEntriesCompared
					<< " boundRetainedBytes=" << BindingAllocations.RetainedBytes << '\n';
				Policy->Stop(Fixture.Service.get());
			}
		Fixture.Service->Stop();
	}

	struct ResourcePoint final {
		double ElapsedSeconds = 0.0;
		std::uint64_t Tick = 0;
		AllocationSample Allocations;
		std::uint64_t ResidentBytes = 0;
		ResidencyDemandPolicyMetrics Policy;
		ContentAvailabilityMetrics Availability;
	};

	void PrintResourcePoints(std::string_view Phase, const std::vector<ResourcePoint> &Points) {
		for (const auto &Point : Points)
			std::cout << "[Content:ResidencyResource] phase=" << Phase
				<< " elapsedSeconds=" << Point.ElapsedSeconds << " tick=" << Point.Tick
				<< " allocations=" << Point.Allocations.Count
				<< " trackedPeakBytes=" << Point.Allocations.PeakBytes
				<< " trackedRetainedBytes=" << Point.Allocations.RetainedBytes
				<< " processRssBytes=" << Point.ResidentBytes
				<< " focuses=" << Point.Policy.Focuses
				<< " pendingFocuses=" << Point.Policy.PendingFocuses
				<< " candidates=" << Point.Policy.RetainedCandidates
				<< " demandRecords=" << Point.Policy.DemandRecords
				<< " pendingDeltas=" << Point.Policy.PendingDeltas
				<< " policyRequests=" << Point.Policy.Requests
				<< " policyReleases=" << Point.Policy.Releases
				<< " serviceRequests=" << Point.Availability.Requests
				<< " acquisitions=" << Point.Availability.Acquisitions
				<< " admissions=" << Point.Availability.Admissions
				<< " evictions=" << Point.Availability.Evictions
				<< " serviceRetainedRecords=" << Point.Availability.RetainedRecordCount
				<< " cachedBytes=" << Point.Availability.CachedPayloadBytes << '\n';
	}

	void AddResourcePoint(std::vector<ResourcePoint> &Points, Clock::time_point StartedAt, std::uint64_t Tick,
		const ResidencyDemandPolicy &Policy, const ContentAvailabilityService *Availability = nullptr) {
		Points.push_back({
			.ElapsedSeconds = std::chrono::duration<double>(Clock::now() - StartedAt).count(),
			.Tick = Tick,
			.Allocations = SnapshotAllocationSample(),
			.ResidentBytes = ProcessResidentBytes(),
			.Policy = Policy.GetMetrics(),
			.Availability = Availability ? Availability->GetMetrics() : ContentAvailabilityMetrics{},
		});
	}

	void CheckResourceGauges(const ResidencyDemandPolicyMetrics &Metrics, std::size_t FocusCount,
		std::size_t ManifestEntries, const ResidencyDemandPolicyLimits &Limits) {
		if (Metrics.Focuses > FocusCount || Metrics.RetainedCandidates > 2 * FocusCount * Limits.MaximumCandidatesPerFocus ||
			Metrics.DemandRecords > ManifestEntries || Metrics.PendingDeltas > ManifestEntries ||
			Metrics.QueryFailures != 0 || Metrics.ServiceBindingFailures != 0)
			throw std::runtime_error("[Content:ResidencyReadiness] policy resource gauge exceeded fixture bound");
	}

	void CheckTailRetainedSpan(const std::vector<ResourcePoint> &Points, bool Quick, std::string_view Phase) {
		if (Quick) return;
		if (Points.size() < 20)
			throw std::runtime_error("[Content:ResidencyReadiness] too few one-second resource samples");
		const auto TailBegin = Points.end() - 10;
		const auto [Minimum, Maximum] = std::minmax_element(TailBegin, Points.end(),
			[](const ResourcePoint &Left, const ResourcePoint &Right) {
				return Left.Allocations.RetainedBytes < Right.Allocations.RetainedBytes;
			});
		const auto Span = Maximum->Allocations.RetainedBytes - Minimum->Allocations.RetainedBytes;
		std::cout << "[Content:ResidencyResource] phase=" << Phase << " tailSamples=10"
			<< " trackedTailRetainedSpanBytes=" << Span << " tailRetainedSpanLimitBytes=" << 16 * 1024 * 1024 << '\n';
		if (Span > 16 * 1024 * 1024)
			throw std::runtime_error("[Content:ResidencyReadiness] tracked tail retained span exceeded finite gate");
	}

	void AddCorePhaseTiming(const ResidencyDemandPolicy &Policy, std::uint64_t Tick,
		std::vector<double> &EvaluationSamples, std::vector<double> &ReconciliationSamples) {
		const auto Observation = Policy.GetDiagnostics(ResidencyDiagnosticQuery{0, 0, 1});
		if (!Observation.Enabled || Observation.TimingSamples.size() != 1 ||
			Observation.TimingSamples.front().Tick != Tick)
			throw std::runtime_error("[Content:ResidencyReadiness] core timing observation missing current tick");
		EvaluationSamples.push_back(static_cast<double>(Observation.TimingSamples.front().EvaluationNanoseconds) / 1'000.0);
		ReconciliationSamples.push_back(static_cast<double>(Observation.TimingSamples.front().ReconciliationNanoseconds) / 1'000.0);
	}

	void RunPolicyResource(bool Quick) {
		const std::size_t EntryCount = Quick ? 1'000 : 65'536;
		const std::size_t FocusCount = Quick ? 10 : 500;
		const std::uint64_t DurationSeconds = Quick ? 2 : 30;
		const std::uint64_t MaximumTicks = DurationSeconds * 60;
		auto ManifestSnapshot = std::make_shared<const PackageContentManifest>(MakeManifest(EntryCount));
		std::vector<glm::vec3> BasePositions;
		BasePositions.reserve(FocusCount);
		for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
			BasePositions.push_back(FocusPosition(Focus, FocusCount, EntryCount));
		std::vector<ResourcePoint> Points;
		Points.reserve(DurationSeconds + 7); // Include the five-second overrun guard and endpoints.
		std::vector<double> UpdateAndStepSamples;
		std::vector<double> CoreEvaluationSamples;
		std::vector<double> CoreReconciliationSamples;
		UpdateAndStepSamples.reserve(MaximumTicks);
		CoreEvaluationSamples.reserve(MaximumTicks);
		CoreReconciliationSamples.reserve(MaximumTicks);
		ResidencyDemandPolicyLimits Limits;
		std::uint64_t LastTick = 0;
		StartAllocationSample();
		{
			auto Policy = std::make_unique<ResidencyDemandPolicy>(ManifestSnapshot, Limits);
			for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
				if (!Policy->UpdateFocus(Focus + 1, BasePositions[Focus]))
					throw std::runtime_error("[Content:ResidencyReadiness] policy-only initial focus rejected");
			for (std::size_t Warm = 0; Warm < (Quick ? 32u : 128u); ++Warm)
				Policy->Step(nullptr, ++LastTick);
			Policy->SetDiagnosticsEnabled(true);
			const auto StartedAt = Clock::now();
			auto NextSampleAt = StartedAt + std::chrono::seconds(1);
			AddResourcePoint(Points, StartedAt, LastTick, *Policy);
			for (std::uint64_t Sample = 0; Sample < MaximumTicks; ++Sample) {
				if (Clock::now() >= StartedAt + std::chrono::seconds(DurationSeconds + 5)) break;
				const auto Cycle = Sample % 360;
				const float Offset = Cycle < 120 ? static_cast<float>(Cycle) * 4.0f
					: Cycle < 240 ? static_cast<float>(240 - Cycle) * 4.0f
					: Cycle < 300 ? 4'096.0f : 0.0f;
				const auto Velocity = Cycle < 120 ? glm::vec3(240.0f, 0.0f, 0.0f)
					: Cycle < 240 ? glm::vec3(-240.0f, 0.0f, 0.0f) : glm::vec3{};
				const auto Before = Policy->GetMetrics();
				const auto StepStartedAt = Clock::now();
				for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
					if (!Policy->UpdateFocus(Focus + 1, BasePositions[Focus] + glm::vec3(Offset, 0.0f, 0.0f), Velocity))
						throw std::runtime_error("[Content:ResidencyReadiness] policy-only movement rejected");
				Policy->Step(nullptr, ++LastTick);
				UpdateAndStepSamples.push_back(Microseconds(StepStartedAt));
				AddCorePhaseTiming(*Policy, LastTick, CoreEvaluationSamples, CoreReconciliationSamples);
				const auto After = Policy->GetMetrics();
				CheckWorkBound(Before, After, Limits);
				CheckResourceGauges(After, FocusCount, EntryCount, Limits);
				if (Clock::now() >= NextSampleAt) {
					AddResourcePoint(Points, StartedAt, LastTick, *Policy);
					NextSampleAt = Clock::now() + std::chrono::seconds(1);
				}
				std::this_thread::sleep_until(StartedAt + std::chrono::milliseconds((Sample + 1) * 1'000 / 60));
			}
			AddResourcePoint(Points, StartedAt, LastTick, *Policy);
			Policy->Stop(nullptr);
			if (Policy->GetMetrics().DemandRecords != 0 || Policy->GetMetrics().Focuses != 0)
				throw std::runtime_error("[Content:ResidencyReadiness] policy-only stop retained demand state");
		}
		const auto FinalAllocations = FinishAllocationSample();
		const auto MeasuredTicks = UpdateAndStepSamples.size();
		const auto CoreSamples = CoreEvaluationSamples.size();
		if (CoreSamples != MeasuredTicks || CoreReconciliationSamples.size() != MeasuredTicks)
			throw std::runtime_error("[Content:ResidencyReadiness] policy core timing sample count mismatch");
		const auto Timings = Summarize(std::move(UpdateAndStepSamples));
		const auto EvaluationTime = Summarize(std::move(CoreEvaluationSamples));
		const auto ReconciliationTime = Summarize(std::move(CoreReconciliationSamples));
		PrintResourcePoints("policy-only", Points);
		CheckTailRetainedSpan(Points, Quick, "policy-only");
		std::cout << "[Content:ResidencyResource] phase=policy-only-final entryCount=" << EntryCount
			<< " focuses=" << FocusCount << " measuredTicks=" << MeasuredTicks
			<< " lastPolicyTick=" << LastTick
			<< " updateAndStepP50Us=" << Timings.P50 << " updateAndStepP95Us=" << Timings.P95
			<< " updateAndStepP99Us=" << Timings.P99 << " updateAndStepMaxUs=" << Timings.Maximum
			<< " coreSamples=" << CoreSamples
			<< " coreEvaluationP50Us=" << EvaluationTime.P50
			<< " coreEvaluationP95Us=" << EvaluationTime.P95
			<< " coreEvaluationP99Us=" << EvaluationTime.P99
			<< " coreReconciliationP50Us=" << ReconciliationTime.P50
			<< " coreReconciliationP95Us=" << ReconciliationTime.P95
			<< " coreReconciliationP99Us=" << ReconciliationTime.P99
			<< " diagnosticsEnabled=true corePhasesExcludeObservation=true"
			<< " externalPolicyClockIncludesCollector=true"
			<< " observerAllocationsIncludedInResourceCensus=true"
			<< " trackedAfterPolicyDestructionBytes=" << FinalAllocations.RetainedBytes
			<< " rssAfterPolicyDestructionBytes=" << ProcessResidentBytes()
			<< " constructor=shared-snapshot trackedScope=policy-allocations-snapshot-excluded" << '\n';
		if (FinalAllocations.RetainedBytes != 0)
			throw std::runtime_error("[Content:ResidencyReadiness] policy-only tracked allocations survived destruction");
	}

	void RunInMemoryReconciliationResource(bool Quick) {
		const std::size_t EntryCount = Quick ? 1'000 : 65'536;
		const std::size_t FocusCount = Quick ? 10 : 500;
		const std::uint64_t DurationSeconds = Quick ? 2 : 30;
		const std::uint64_t MaximumTicks = DurationSeconds * 60;
		auto Fixture = MakeInMemoryServiceFixture(EntryCount);
		std::vector<ResourcePoint> Points;
		Points.reserve(DurationSeconds + 7); // Keep sampler growth outside the tracked policy scope.
		std::vector<double> PolicySamples;
		std::vector<double> AvailabilitySamples;
		std::vector<double> CoreEvaluationSamples;
		std::vector<double> CoreReconciliationSamples;
		PolicySamples.reserve(MaximumTicks);
		AvailabilitySamples.reserve(MaximumTicks);
		CoreEvaluationSamples.reserve(MaximumTicks);
		CoreReconciliationSamples.reserve(MaximumTicks);
		ResidencyDemandPolicyLimits Limits;
		std::uint64_t LastTick = 0;
		std::uint64_t ReplacementRequests = 0;
		std::size_t StoppedRequestsVerified = 0;
		ContentAvailabilityMetrics BeforeStopMetrics;
		StartAllocationSample();
		{
			auto Policy = std::make_unique<ResidencyDemandPolicy>(Fixture.Snapshot, Limits);
			for (std::size_t Focus = 0; Focus < FocusCount; ++Focus)
				if (!Policy->UpdateFocus(Focus + 1, {508.0f + static_cast<float>(Focus % 5) * 2.0f, 0.0f, 508.0f}))
					throw std::runtime_error("[Content:ResidencyReadiness] in-memory initial focus rejected");
			Policy->SetDiagnosticsEnabled(true);
			const auto StartedAt = Clock::now();
			auto NextSampleAt = StartedAt + std::chrono::seconds(1);
			AddResourcePoint(Points, StartedAt, LastTick, *Policy, Fixture.Service.get());
			for (std::uint64_t Sample = 0; Sample < MaximumTicks; ++Sample) {
				if (Clock::now() >= StartedAt + std::chrono::seconds(DurationSeconds + 5)) break;
				const auto Cycle = Sample % 360;
				if (Cycle == 0)
					for (std::size_t Focus = 0; Focus < std::min<std::size_t>(FocusCount, 100); ++Focus)
						if (!Policy->RemoveFocus(Focus + 1))
							throw std::runtime_error("[Content:ResidencyReadiness] in-memory focus retirement rejected");
				const std::size_t Cell = Cycle < 120 ? Cycle / 15
					: Cycle < 240 ? (239 - Cycle) / 15 : Cycle < 300 ? 7 : 0;
				const auto Velocity = Cycle < 120 ? glm::vec3(512.0f, 0.0f, 0.0f)
					: Cycle < 240 ? glm::vec3(-512.0f, 0.0f, 0.0f) : glm::vec3{};
				const auto Before = Policy->GetMetrics();
				const auto PolicyStartedAt = Clock::now();
				for (std::size_t Focus = 0; Focus < FocusCount; ++Focus) {
					if (Cycle < 30 && Focus < std::min<std::size_t>(FocusCount, 100)) continue;
					if (!Policy->UpdateFocus(Focus + 1,
						{508.0f + static_cast<float>(Cell) * 256.0f + static_cast<float>(Focus % 5) * 2.0f,
							0.0f, 508.0f}, Velocity))
						throw std::runtime_error("[Content:ResidencyReadiness] in-memory movement rejected");
				}
				Policy->Step(Fixture.Service.get(), ++LastTick);
				PolicySamples.push_back(Microseconds(PolicyStartedAt));
				AddCorePhaseTiming(*Policy, LastTick, CoreEvaluationSamples, CoreReconciliationSamples);
				const auto After = Policy->GetMetrics();
				CheckWorkBound(Before, After, Limits);
				CheckResourceGauges(After, FocusCount, EntryCount, Limits);
				const auto AvailabilityStartedAt = Clock::now();
				Fixture.Service->Step();
				AvailabilitySamples.push_back(Microseconds(AvailabilityStartedAt));
				if (Clock::now() >= NextSampleAt) {
					AddResourcePoint(Points, StartedAt, LastTick, *Policy, Fixture.Service.get());
					NextSampleAt = Clock::now() + std::chrono::seconds(1);
				}
				std::this_thread::sleep_until(StartedAt + std::chrono::milliseconds((Sample + 1) * 1'000 / 60));
			}
			AddResourcePoint(Points, StartedAt, LastTick, *Policy, Fixture.Service.get());
			BeforeStopMetrics = Fixture.Service->GetMetrics();
			const auto FinalPolicyMetrics = Policy->GetMetrics();
			if (BeforeStopMetrics.Failures != 0 || BeforeStopMetrics.Requests != FinalPolicyMetrics.Requests ||
				BeforeStopMetrics.Requests > 1'000 || FinalPolicyMetrics.DemandRecords > 64)
				throw std::runtime_error("[Content:ResidencyReadiness] in-memory reconciliation failed resource bound");
			std::vector<std::string> RequestedKeys;
			for (const auto &Demand : Policy->GetDemands())
				if (Demand.Requested) RequestedKeys.push_back(Demand.Key);
			if (RequestedKeys.empty())
				throw std::runtime_error("[Content:ResidencyReadiness] no direct request to verify at policy stop");
			Policy->Stop(Fixture.Service.get());
			const auto StoppedMetrics = Policy->GetMetrics();
			if (StoppedMetrics.Focuses != 0 || StoppedMetrics.DemandRecords != 0 ||
				StoppedMetrics.RetainedCandidates != 0 || StoppedMetrics.PendingDeltas != 0)
				throw std::runtime_error("[Content:ResidencyReadiness] in-memory policy stop retained demand state");
			for (const auto &Key : RequestedKeys)
				if (Fixture.Service->ReleaseContent(Key))
					throw std::runtime_error("[Content:ResidencyReadiness] policy stop left direct service demand");
			StoppedRequestsVerified = RequestedKeys.size();
			Policy.reset();
			const auto RequestsBeforeReplacement = Fixture.Service->GetMetrics().Requests;
			auto Replacement = std::make_unique<ResidencyDemandPolicy>(Fixture.Snapshot, Limits);
			if (!Replacement->SetRequirement(Fixture.Snapshot->Entries.front().Key, ResidencyDemandClass::Critical))
				throw std::runtime_error("[Content:ResidencyReadiness] replacement requirement rejected");
			const auto Deadline = Clock::now() + std::chrono::seconds(5);
			while (Clock::now() < Deadline) {
				Replacement->Step(Fixture.Service.get(), ++LastTick);
				Fixture.Service->Step();
				ReplacementRequests = Fixture.Service->GetMetrics().Requests - RequestsBeforeReplacement;
				if (ReplacementRequests == 1 &&
					Fixture.Service->GetState(Fixture.Snapshot->Entries.front().Key) == ContentResidencyState::Resident)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (ReplacementRequests != 1 ||
				Fixture.Service->GetState(Fixture.Snapshot->Entries.front().Key) != ContentResidencyState::Resident)
				throw std::runtime_error("[Content:ResidencyReadiness] replacement demand did not acquire content");
			Replacement->Stop(Fixture.Service.get());
			Fixture.Service->Stop();
			if (Fixture.Service->GetMetrics().RetainedRecordCount != 0)
				throw std::runtime_error("[Content:ResidencyReadiness] service stop retained session records");
		}
		const auto FinalAllocations = FinishAllocationSample();
		const auto MeasuredTicks = PolicySamples.size();
		const auto CoreSamples = CoreEvaluationSamples.size();
		if (CoreSamples != MeasuredTicks || CoreReconciliationSamples.size() != MeasuredTicks)
			throw std::runtime_error("[Content:ResidencyReadiness] in-memory core timing sample count mismatch");
		const auto PolicyTime = Summarize(std::move(PolicySamples));
		const auto AvailabilityTime = Summarize(std::move(AvailabilitySamples));
		const auto EvaluationTime = Summarize(std::move(CoreEvaluationSamples));
		const auto ReconciliationTime = Summarize(std::move(CoreReconciliationSamples));
		PrintResourcePoints("in-memory-cas", Points);
		CheckTailRetainedSpan(Points, Quick, "in-memory-cas");
		std::cout << "[Content:ResidencyResource] phase=in-memory-cas-final entryCount=" << EntryCount
			<< " focuses=" << FocusCount << " measuredTicks=" << MeasuredTicks
			<< " lastPolicyTick=" << LastTick
			<< " policyUpdateAndStepP50Us=" << PolicyTime.P50
			<< " policyUpdateAndStepP95Us=" << PolicyTime.P95
			<< " policyUpdateAndStepP99Us=" << PolicyTime.P99
			<< " coreSamples=" << CoreSamples
			<< " coreEvaluationP50Us=" << EvaluationTime.P50
			<< " coreEvaluationP95Us=" << EvaluationTime.P95
			<< " coreEvaluationP99Us=" << EvaluationTime.P99
			<< " coreReconciliationP50Us=" << ReconciliationTime.P50
			<< " coreReconciliationP95Us=" << ReconciliationTime.P95
			<< " coreReconciliationP99Us=" << ReconciliationTime.P99
			<< " diagnosticsEnabled=true corePhasesExcludeObservation=true"
			<< " externalPolicyClockIncludesCollector=true"
			<< " observerAllocationsIncludedInResourceCensus=true"
			<< " casStepP50Us=" << AvailabilityTime.P50
			<< " casStepP95Us=" << AvailabilityTime.P95
			<< " casStepP99Us=" << AvailabilityTime.P99
			<< " serviceRequests=" << BeforeStopMetrics.Requests
			<< " acquisitions=" << BeforeStopMetrics.Acquisitions
			<< " admissions=" << BeforeStopMetrics.Admissions
			<< " evictions=" << BeforeStopMetrics.Evictions
			<< " providerMaxUs=" << BeforeStopMetrics.Timing.Provider.MaximumMicroseconds
			<< " preparationMaxUs=" << BeforeStopMetrics.Timing.Preparation.MaximumMicroseconds
			<< " admissionCommitMaxUs=" << BeforeStopMetrics.Timing.Commit.MaximumMicroseconds
			<< " casStepMaxUs=" << BeforeStopMetrics.Timing.Step.MaximumMicroseconds
			<< " replacementRequests=" << ReplacementRequests
			<< " stoppedRequestsVerified=" << StoppedRequestsVerified
			<< " trackedAfterStopBytes=" << FinalAllocations.RetainedBytes
			<< " rssAfterStopBytes=" << ProcessResidentBytes()
			<< " provider=in-memory trackedScope=policy-and-post-ready-service-allocations" << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const bool Quick = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--quick";
		const bool ReadinessQuick = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--readiness-quick";
		const bool ReadinessLong = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--readiness-long";
		const bool ReadinessStartup = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--readiness-startup";
		const bool ReadinessPolicyLong = ArgumentCount == 2 &&
			std::string_view(Arguments[1]) == "--readiness-policy-long";
		const bool ReadinessServiceLong = ArgumentCount == 2 &&
			std::string_view(Arguments[1]) == "--readiness-service-long";
		if (ArgumentCount > 2 || (ArgumentCount == 2 && !Quick && !ReadinessQuick && !ReadinessLong && !ReadinessStartup &&
			!ReadinessPolicyLong && !ReadinessServiceLong))
			throw std::invalid_argument("usage: ResidencyDemandBenchmark "
				"[--quick|--readiness-quick|--readiness-startup|--readiness-long|--readiness-policy-long|--readiness-service-long]");
		BootstrapNativeRuntimeSchema();
		if (ReadinessStartup) {
			std::cout << "[Content:ResidencyReadiness] mode=startup manifestEntries=1000,10000,65536"
				<< " repetitions=3 provider=in-memory providerQualification=false" << '\n';
			for (const auto Entries : {std::size_t{1'000}, std::size_t{10'000}, std::size_t{65'536}})
				RunStartupComparison(Entries, 3);
			return 0;
		}
		if (ReadinessPolicyLong || ReadinessServiceLong) {
			std::cout << "[Content:ResidencyReadiness] mode="
				<< (ReadinessPolicyLong ? "policy-long" : "service-long")
				<< " durationSeconds=30 manifestEntries=65536 focuses=500"
				<< " providerQualification=false clientMaterializationMeasured=false" << '\n';
			if (ReadinessPolicyLong) RunPolicyResource(false);
			else RunInMemoryReconciliationResource(false);
			return 0;
		}
		if (ReadinessQuick || ReadinessLong) {
			std::cout << "[Content:ResidencyReadiness] mode=" << (ReadinessQuick ? "quick" : "long")
				<< " resourceScope=policy-only-then-in-memory-cas"
				<< " providerQualification=false clientMaterializationMeasured=false" << '\n';
			for (const auto Entries : ReadinessQuick ? std::vector<std::size_t>{1'000}
											  : std::vector<std::size_t>{1'000, 10'000, 65'536})
				RunStartupComparison(Entries, ReadinessQuick ? 2 : 5);
			RunPolicyResource(ReadinessQuick);
			RunInMemoryReconciliationResource(ReadinessQuick);
			return 0;
		}
		std::cout << "phase,manifest_entries,focuses,samples,mean_us,p50_us,p95_us,p99_us,max_us,"
			"allocations,peak_alloc_bytes,retained_alloc_bytes,query_pages,query_cells,membership_visits,"
			"completed_focus_evaluations,stale_revisions,moving_sweep_completions,candidate_revalidations,"
			"request_validation_visits,ranking_operations,requests,releases,retained_candidates,demand_records,"
			"pending_deltas,reference_hits\n";
		for (const auto Entries : Quick ? std::vector<std::size_t>{1'000, 10'000}
										 : std::vector<std::size_t>{1'000, 10'000, 65'536}) {
			for (const auto Focuses : Quick ? std::vector<std::size_t>{1, 10, 100}
											: std::vector<std::size_t>{1, 10, 100, 500}) {
				RunPolicyCase(Entries, Focuses, Quick);
				std::cout.flush();
				if (Entries <= 10'000 && Focuses <= 100)
					RunLinearReference(MakeManifest(Entries), Focuses, Quick ? 2 : 5);
			}
		}
		RunStaleCursorCase(Quick);
		RunServiceIntegration();
		for (const auto [Entries, Focuses] : Quick
			? std::vector<std::pair<std::size_t, std::size_t>>{{1'000, 10}}
			: std::vector<std::pair<std::size_t, std::size_t>>{{1'000, 10}, {10'000, 100}, {65'536, 500}})
			RunServiceScale(Entries, Focuses);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Content:ResidencyBenchmark] failed: " << Error.what() << '\n';
		return 1;
	}
}

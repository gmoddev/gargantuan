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
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const bool Quick = ArgumentCount == 2 && std::string_view(Arguments[1]) == "--quick";
		if (ArgumentCount > 2 || (ArgumentCount == 2 && !Quick))
			throw std::invalid_argument("usage: ResidencyDemandBenchmark [--quick]");
		BootstrapNativeRuntimeSchema();
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

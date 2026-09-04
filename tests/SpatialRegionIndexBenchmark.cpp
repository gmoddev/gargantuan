#include "gargantuan/runtime/SpatialRegionIndex.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

namespace {
	std::atomic<std::uint64_t> SpatialBenchmarkAllocations = 0;
	std::atomic<std::uint64_t> SpatialBenchmarkAllocatedBytes = 0;
}

void *operator new(std::size_t Size) {
	SpatialBenchmarkAllocations.fetch_add(1, std::memory_order_relaxed);
	SpatialBenchmarkAllocatedBytes.fetch_add(Size, std::memory_order_relaxed);
	if (auto *Value = std::malloc(Size)) return Value;
	throw std::bad_alloc();
}

void operator delete(void *Value) noexcept {
	std::free(Value);
}

void operator delete(void *Value, std::size_t) noexcept {
	std::free(Value);
}

namespace {
	using namespace gargantuan;

	double Milliseconds(std::chrono::steady_clock::time_point Started) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
	}

	SpatialRegionIndexConfiguration Configuration(std::size_t ObjectCount, double RegionSize) {
		return {
			.RegionSize = RegionSize,
			.MaximumObjects = ObjectCount,
			.MaximumRegions = ObjectCount,
			.MaximumMemberships = std::max<std::size_t>(ObjectCount, 262'144),
			.MaximumRegionsPerObject = 64,
			.MaximumLargeObjects = 1'024,
			.MaximumQueryVolumes = 4,
			.MaximumQueryRegions = 4'096,
			.MaximumQueryCandidates = 65'536,
			.MaximumQueryMembershipVisits = 262'144,
		};
	}

	glm::dvec3 Position(std::size_t Index, double RegionSize) {
		if (Index < 9'261) {
			const auto X = static_cast<int>(Index % 21) - 10;
			const auto Y = static_cast<int>((Index / 21) % 21) - 10;
			const auto Z = static_cast<int>((Index / 441) % 21) - 10;
			return glm::dvec3(X, Y, Z) * 64.0;
		}
		return {10'000.0 + static_cast<double>(Index - 9'261) * RegionSize * 2.0, 10'000.0, 10'000.0};
	}

	double AverageOccupancy(const SpatialRegionIndexMetrics &Metrics) {
		return Metrics.RegionCount == 0
				   ? 0.0
				   : static_cast<double>(Metrics.MembershipCount) / static_cast<double>(Metrics.RegionCount);
	}

	void RunWorldScale(std::size_t ObjectCount, double RegionSize) {
		auto Limits = Configuration(ObjectCount, RegionSize);
		SpatialRegionIndex Index(Limits);
		SpatialRegionQueryScratch Scratch;
		Scratch.Reserve(Limits);
		const auto AllocationsBeforeBuild = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto BytesBeforeBuild = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto BuildStarted = std::chrono::steady_clock::now();
		for (std::size_t Object = 0; Object < ObjectCount; ++Object) {
			const auto Status = Index.Register(
				{static_cast<std::uint32_t>(Object + 1), 1}, SpatialBounds::Point(Position(Object, RegionSize))
			);
			if (Status != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] world registration failed");
		}
		const auto BuildMs = Milliseconds(BuildStarted);
		const auto BuildAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
									  AllocationsBeforeBuild;
		const auto BuildAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
										 BytesBeforeBuild;
		constexpr double QueryRadius = 320.0;
		const SpatialRegionQueryVolume QueryVolume{.Center = {0.0, 0.0, 0.0}, .Radius = QueryRadius};
		std::size_t Relevant = 0;
		for (std::size_t Object = 0; Object < std::min<std::size_t>(ObjectCount, 9'261); ++Object)
			if (glm::dot(Position(Object, RegionSize), Position(Object, RegionSize)) <= QueryRadius * QueryRadius)
				++Relevant;
		const auto AllocationsBeforeQuery = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto BytesBeforeQuery = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto QueryStarted = std::chrono::steady_clock::now();
		constexpr std::size_t QueryIterations = 100;
		for (std::size_t Iteration = 0; Iteration < QueryIterations; ++Iteration)
			if (Index.Query(std::span(&QueryVolume, 1), Scratch) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] fixed-interest query failed");
		const auto QueryMs = Milliseconds(QueryStarted);
		const auto QueryAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
									  AllocationsBeforeQuery;
		const auto QueryAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
										 BytesBeforeQuery;
		const auto Metrics = Index.GetMetrics();
		std::cout << "WorldScale," << RegionSize << ',' << ObjectCount << ',' << BuildMs << ',' << BuildAllocations
				  << ',' << BuildAllocatedBytes << ',' << QueryMs / QueryIterations << ',' << QueryAllocations << ','
				  << QueryAllocatedBytes << ',' << Metrics.RegionCount << ',' << Metrics.MembershipCount << ','
				  << AverageOccupancy(Metrics) << ',' << Metrics.PeakRegionOccupancy << ',' << Scratch.Regions.size()
				  << ',' << Scratch.Candidates.size() << ',' << Relevant << ','
				  << (Scratch.Candidates.size() - Relevant) << ',' << Metrics.CandidateDedupHits << ",0,0,0,0,0\n";
	}

	void RunMovement(std::size_t ObjectCount, double RegionSize) {
		auto Limits = Configuration(ObjectCount, RegionSize);
		SpatialRegionIndex Index(Limits);
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Register(
					{static_cast<std::uint32_t>(Object + 1), 1},
					SpatialBounds::Point({static_cast<double>(Object) * RegionSize * 2.0, 0.0, 0.0})
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] movement registration failed");
		const auto SameAllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto SameBytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto SameStarted = std::chrono::steady_clock::now();
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Update(
					{static_cast<std::uint32_t>(Object + 1), 1},
					SpatialBounds::Point({static_cast<double>(Object) * RegionSize * 2.0 + RegionSize * 0.25, 0.0, 0.0})
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] same-region movement failed");
		const auto SameMs = Milliseconds(SameStarted);
		const auto SameAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
									 SameAllocationsBefore;
		const auto SameAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
										SameBytesBefore;
		const auto CrossAllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto CrossBytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto CrossStarted = std::chrono::steady_clock::now();
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Update(
					{static_cast<std::uint32_t>(Object + 1), 1},
					SpatialBounds::Point({static_cast<double>(Object) * RegionSize * 2.0 + RegionSize * 1.25, 0.0, 0.0})
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] boundary movement failed");
		const auto CrossMs = Milliseconds(CrossStarted);
		const auto CrossAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
									  CrossAllocationsBefore;
		const auto CrossAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
										 CrossBytesBefore;
		const auto CrossMetrics = Index.GetMetrics();
		std::cout << "Movement," << RegionSize << ',' << ObjectCount << ",0,0,0," << SameMs << ',' << SameAllocations
				  << ',' << SameAllocatedBytes << ',' << CrossMetrics.RegionCount << ',' << CrossMetrics.MembershipCount
				  << ',' << AverageOccupancy(CrossMetrics) << ',' << CrossMetrics.PeakRegionOccupancy << ",0,0,0,0,0,"
				  << CrossMs << ',' << CrossAllocations << ',' << CrossAllocatedBytes << ','
				  << CrossMetrics.SameRegionUpdates << ',' << CrossMetrics.MembershipMoves << '\n';
		const auto TeleportAllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto TeleportBytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto TeleportStarted = std::chrono::steady_clock::now();
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Update(
					{static_cast<std::uint32_t>(Object + 1), 1},
					SpatialBounds::Point({-10'000'000.0 - static_cast<double>(Object) * RegionSize * 3.0, 0.0, 0.0})
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] teleport movement failed");
		const auto TeleportMs = Milliseconds(TeleportStarted);
		const auto TeleportAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
										 TeleportAllocationsBefore;
		const auto TeleportAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
											TeleportBytesBefore;
		const auto TeleportMetrics = Index.GetMetrics();
		std::cout << "Teleport," << RegionSize << ',' << ObjectCount << ",0,0,0," << TeleportMs << ','
				  << TeleportAllocations << ',' << TeleportAllocatedBytes << ',' << TeleportMetrics.RegionCount << ','
				  << TeleportMetrics.MembershipCount << ',' << AverageOccupancy(TeleportMetrics) << ','
				  << TeleportMetrics.PeakRegionOccupancy << ",0,0,0,0,0,0,0,0," << TeleportMetrics.SameRegionUpdates
				  << ',' << TeleportMetrics.MembershipMoves << '\n';
	}

	void RunDense(std::size_t ObjectCount, double RegionSize) {
		auto Limits = Configuration(ObjectCount, RegionSize);
		Limits.MaximumQueryCandidates = ObjectCount;
		Limits.MaximumQueryMembershipVisits = std::max<std::size_t>(ObjectCount, 262'144);
		SpatialRegionIndex Index(Limits);
		SpatialRegionQueryScratch Scratch;
		Scratch.Reserve(Limits);
		const auto BuildAllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto BuildBytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto BuildStarted = std::chrono::steady_clock::now();
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Register(
					{static_cast<std::uint32_t>(Object + 1), 1},
					SpatialBounds::Point({static_cast<double>(Object % 100) * 0.01, 0.0, 0.0})
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] dense registration failed");
		const auto BuildMs = Milliseconds(BuildStarted);
		const auto BuildAllocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) -
									  BuildAllocationsBefore;
		const auto BuildAllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) -
										 BuildBytesBefore;
		const SpatialRegionQueryVolume Volume{.Center = {0.0, 0.0, 0.0}, .Radius = 1.0};
		const auto AllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
		const auto BytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
		const auto Started = std::chrono::steady_clock::now();
		if (Index.Query(std::span(&Volume, 1), Scratch) != SpatialRegionStatus::Success)
			throw std::runtime_error("[Spatial:Benchmark] dense query failed");
		const auto Duration = Milliseconds(Started);
		const auto Allocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
		const auto AllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) - BytesBefore;
		const auto Metrics = Index.GetMetrics();
		std::cout << "Dense," << RegionSize << ',' << ObjectCount << ',' << BuildMs << ',' << BuildAllocations << ','
				  << BuildAllocatedBytes << ',' << Duration << ',' << Allocations << ',' << AllocatedBytes << ','
				  << Metrics.RegionCount << ',' << Metrics.MembershipCount << ',' << AverageOccupancy(Metrics) << ','
				  << Metrics.PeakRegionOccupancy << ',' << Scratch.Regions.size() << ',' << Scratch.Candidates.size()
				  << ",0,0," << Metrics.CandidateDedupHits << ",0,0,0,0,0\n";
	}

	void RunFocusQueries(double RegionSize) {
		constexpr std::size_t ObjectCount = 10'000;
		auto Limits = Configuration(ObjectCount, RegionSize);
		SpatialRegionIndex Index(Limits);
		SpatialRegionQueryScratch Scratch;
		Scratch.Reserve(Limits);
		for (std::size_t Object = 0; Object < ObjectCount; ++Object)
			if (Index.Register(
					{static_cast<std::uint32_t>(Object + 1), 1}, SpatialBounds::Point(Position(Object, RegionSize))
				) != SpatialRegionStatus::Success)
				throw std::runtime_error("[Spatial:Benchmark] focus-query registration failed");
		const std::array<SpatialRegionQueryVolume, 4> Overlapping{
			SpatialRegionQueryVolume{.Center = {0.0, 0.0, 0.0}, .Radius = 320.0},
			SpatialRegionQueryVolume{.Center = {64.0, 0.0, 0.0}, .Radius = 320.0},
			SpatialRegionQueryVolume{.Center = {0.0, 64.0, 0.0}, .Radius = 320.0},
			SpatialRegionQueryVolume{.Center = {0.0, 0.0, 64.0}, .Radius = 320.0},
		};
		const std::array<SpatialRegionQueryVolume, 4> Disjoint{
			SpatialRegionQueryVolume{.Center = {-512.0, -512.0, -512.0}, .Radius = 128.0},
			SpatialRegionQueryVolume{.Center = {512.0, -512.0, 512.0}, .Radius = 128.0},
			SpatialRegionQueryVolume{.Center = {-512.0, 512.0, 512.0}, .Radius = 128.0},
			SpatialRegionQueryVolume{.Center = {512.0, 512.0, -512.0}, .Radius = 128.0},
		};
		auto Measure = [&](const char *Kind, std::span<const SpatialRegionQueryVolume> Volumes) {
			const auto Before = Index.GetMetrics();
			const auto AllocationsBefore = SpatialBenchmarkAllocations.load(std::memory_order_relaxed);
			const auto BytesBefore = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed);
			const auto Started = std::chrono::steady_clock::now();
			constexpr std::size_t Iterations = 100;
			for (std::size_t Iteration = 0; Iteration < Iterations; ++Iteration)
				if (Index.Query(Volumes, Scratch) != SpatialRegionStatus::Success)
					throw std::runtime_error("[Spatial:Benchmark] multi-focus query failed");
			const auto Duration = Milliseconds(Started) / Iterations;
			const auto Allocations = SpatialBenchmarkAllocations.load(std::memory_order_relaxed) - AllocationsBefore;
			const auto AllocatedBytes = SpatialBenchmarkAllocatedBytes.load(std::memory_order_relaxed) - BytesBefore;
			std::size_t Relevant = 0;
			for (std::size_t Object = 0; Object < 9'261; ++Object) {
				const auto Point = Position(Object, RegionSize);
				if (std::ranges::any_of(Volumes, [Point](const SpatialRegionQueryVolume &Volume) {
						return glm::dot(Point - Volume.Center, Point - Volume.Center) <= Volume.Radius * Volume.Radius;
					}))
					++Relevant;
			}
			const auto Metrics = Index.GetMetrics();
			std::cout << Kind << ',' << RegionSize << ',' << ObjectCount << ",0,0,0," << Duration << ',' << Allocations
					  << ',' << AllocatedBytes << ',' << Metrics.RegionCount << ',' << Metrics.MembershipCount << ','
					  << AverageOccupancy(Metrics) << ',' << Metrics.PeakRegionOccupancy << ','
					  << Scratch.Regions.size() << ',' << Scratch.Candidates.size() << ',' << Relevant << ','
					  << Scratch.Candidates.size() - Relevant << ','
					  << Metrics.CandidateDedupHits - Before.CandidateDedupHits << ",0,0,0,0,0\n";
		};
		Measure("MultiFocusOverlap", Overlapping);
		Measure("MultiFocusDisjoint", Disjoint);
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const bool Full = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--full";
		std::cout << "Kind,RegionSize,ObjectCount,BuildMs,BuildAllocations,BuildAllocatedBytes,OperationMs,"
					 "OperationAllocations,OperationAllocatedBytes,"
					 "RegionCount,MembershipCount,AverageOccupancy,PeakOccupancy,QueryRegions,Candidates,Relevant,"
					 "FalsePositives,DedupHits,"
					 "CrossMs,CrossAllocations,CrossAllocatedBytes,SameRegionUpdates,MembershipMoves\n";
		if (Full) {
			for (const auto RegionSize : {64.0, 128.0, 256.0, 512.0, 1024.0})
				RunWorldScale(100'000, RegionSize);
			for (const auto ObjectCount : {10'000u, 100'000u, 1'000'000u})
				RunWorldScale(ObjectCount, DefaultSpatialRegionSize);
			RunMovement(10'000, DefaultSpatialRegionSize);
			RunDense(10'000, DefaultSpatialRegionSize);
			RunFocusQueries(DefaultSpatialRegionSize);
		} else {
			RunWorldScale(10'000, DefaultSpatialRegionSize);
			RunMovement(1'000, DefaultSpatialRegionSize);
			RunDense(1'000, DefaultSpatialRegionSize);
			RunFocusQueries(DefaultSpatialRegionSize);
		}
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Spatial:Benchmark] " << Error.what() << '\n';
		return 1;
	}
}

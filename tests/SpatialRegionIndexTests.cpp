#include "gargantuan/runtime/SpatialRegionIndex.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include <glm/geometric.hpp>

namespace {
	using namespace gargantuan;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Contains(std::span<const ObjectId> Objects, ObjectId Object) {
		return std::ranges::binary_search(Objects, Object);
	}

	SpatialRegionStatus Query(
		SpatialRegionIndex &Index,
		SpatialRegionQueryScratch &Scratch,
		std::initializer_list<SpatialRegionQueryVolume> Volumes
	) {
		return Index.Query(std::span(Volumes.begin(), Volumes.size()), Scratch);
	}

	SpatialRegionQueryVolume Sphere(glm::dvec3 Center, double Radius) {
		return {.Center = Center, .Radius = Radius};
	}

	bool BoundsIntersectSphere(const SpatialBounds &Bounds, glm::dvec3 Center, double Radius) {
		const glm::dvec3 Closest{
			std::clamp(Center.x, Bounds.Minimum.x, Bounds.Maximum.x),
			std::clamp(Center.y, Bounds.Minimum.y, Bounds.Maximum.y),
			std::clamp(Center.z, Bounds.Minimum.z, Bounds.Maximum.z),
		};
		return glm::dot(Closest - Center, Closest - Center) <= Radius * Radius;
	}
}

int main() {
	SpatialRegionIndexConfiguration Invalid;
	Invalid.RegionSize = 0.0;
	Check(!Invalid.IsValid(), "zero region size is invalid");
	Invalid = {};
	Invalid.RegionSize = 0.5;
	Check(!Invalid.IsValid(), "unsafe sub-unit region size is invalid");
	Invalid = {};
	Invalid.MaximumRegionsPerObject = 0;
	Check(!Invalid.IsValid(), "zero per-object overlap is invalid");
	Invalid = {};
	Invalid.MaximumMemberships = Invalid.MaximumObjects - 1;
	Check(!Invalid.IsValid(), "membership capacity smaller than object capacity is contradictory");
	SpatialRegionIndexConfiguration TightMembership;
	TightMembership.MaximumObjects = 1;
	TightMembership.MaximumMemberships = 1;
	TightMembership.MaximumRegionsPerObject = 8;
	SpatialRegionIndex TightMembershipIndex(TightMembership);
	Check(
		TightMembershipIndex.Register({900, 1}, {{0.0, 0.0, 0.0}, {256.0, 128.0, 128.0}}) ==
			SpatialRegionStatus::MembershipLimit,
		"per-object membership cannot underflow a tighter aggregate membership bound"
	);

	const std::array CoordinateCases{
		std::pair{-129.0, -2ll},
		std::pair{-128.0, -1ll},
		std::pair{-127.0, -1ll},
		std::pair{-1.0, -1ll},
		std::pair{0.0, 0ll},
		std::pair{127.0, 0ll},
		std::pair{128.0, 1ll},
		std::pair{129.0, 1ll},
	};
	for (const auto [Position, Expected] : CoordinateCases) {
		const auto Address = SpatialAddressForPosition({Position, Position, Position}, 128.0);
		Check(
			Address && Address->Region.X == Expected && Address->Region.Y == Expected && Address->Region.Z == Expected,
			"negative and positive coordinates use mathematical floor"
		);
	}
	const auto BelowBoundary = SpatialAddressForPosition({std::nextafter(128.0, 0.0), 0.0, 0.0}, 128.0);
	const auto ExactBoundary = SpatialAddressForPosition({128.0, 0.0, 0.0}, 128.0);
	const auto AboveBoundary = SpatialAddressForPosition(
		{std::nextafter(128.0, std::numeric_limits<double>::infinity()), 0.0, 0.0}, 128.0
	);
	Check(
		BelowBoundary && ExactBoundary && AboveBoundary && BelowBoundary->Region.X == 0 &&
			ExactBoundary->Region.X == 1 && AboveBoundary->Region.X == 1,
		"region boundaries are deterministic half-open intervals"
	);
	const auto NegativeBelow = SpatialAddressForPosition(
		{std::nextafter(-128.0, -std::numeric_limits<double>::infinity()), 0.0, 0.0}, 128.0
	);
	const auto NegativeExact = SpatialAddressForPosition({-128.0, 0.0, 0.0}, 128.0);
	const auto NegativeAbove = SpatialAddressForPosition({std::nextafter(-128.0, 0.0), 0.0, 0.0}, 128.0);
	Check(
		NegativeBelow && NegativeExact && NegativeAbove && NegativeBelow->Region.X == -2 &&
			NegativeExact->Region.X == -1 && NegativeAbove->Region.X == -1,
		"negative exact-boundary mapping is symmetric"
	);
	const auto Extreme = SpatialAddressForPosition({1.0e15, -1.0e15, 9.0e14}, 128.0);
	Check(Extreme.has_value(), "large finite addressable coordinates remain valid");
	Check(
		!SpatialAddressForPosition({std::numeric_limits<double>::max(), 0.0, 0.0}, 128.0) &&
			!SpatialAddressForPosition({std::numeric_limits<double>::infinity(), 0.0, 0.0}, 128.0) &&
			!SpatialAddressForPosition({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 128.0),
		"overflowing and non-finite coordinates fail rather than wrap"
	);
	const SpatialAddress AddressA{1, {2, -3, 4}};
	const SpatialAddress AddressB{1, {2, -3, 4}};
	const SpatialAddress AddressC{1, {2, -3, 5}};
	Check(
		AddressA == AddressB && AddressA != AddressC && AddressA < AddressC &&
			AddressA.StableHash() == AddressB.StableHash() && AddressA.ToString() == "space=1 region=(2,-3,4)",
		"SpatialAddress equality, order, hash, and diagnostics are deterministic"
	);
	Check(
		AddressA.StableHash() == 6'713'522'209'043'173'464ull,
		"SpatialAddress stable hash has a platform-independent golden value"
	);

	SpatialRegionIndexConfiguration Configuration;
	Configuration.MaximumObjects = 2'000;
	Configuration.MaximumRegions = 2'000;
	Configuration.MaximumMemberships = 8'000;
	Configuration.MaximumRegionsPerObject = 8;
	Configuration.MaximumLargeObjects = 2;
	Configuration.MaximumQueryCandidates = 2'000;
	Configuration.MaximumQueryMembershipVisits = 8'000;
	SpatialRegionIndex Index(Configuration);
	SpatialRegionQueryScratch Scratch;
	Scratch.Reserve(Configuration);
	const ObjectId Point{1, 1};
	Check(
		Index.Register(Point, SpatialBounds::Point({1.0, 1.0, 1.0})) == SpatialRegionStatus::Success &&
			Index.GetMembershipCount(Point) == 1 && Index.GetPrimaryAddress(Point) == SpatialAddress{1, {0, 0, 0}},
		"a point spatial root registers in one canonical region"
	);
	const auto BeforeSame = Index.GetMetrics();
	Check(
		Index.Update(Point, SpatialBounds::Point({127.0, 64.0, 127.0})) == SpatialRegionStatus::Success &&
			Index.GetMetrics().MembershipMoves == BeforeSame.MembershipMoves &&
			Index.GetMetrics().SameRegionUpdates == BeforeSame.SameRegionUpdates + 1,
		"same-region movement performs no bucket mutation"
	);
	Check(
		Index.Update(Point, SpatialBounds::Point({128.0, 64.0, 127.0})) == SpatialRegionStatus::Success &&
			Index.GetPrimaryAddress(Point) == SpatialAddress{1, {1, 0, 0}},
		"single-boundary movement changes canonical membership"
	);
	Check(
		Index.Update(Point, SpatialBounds::Point({1'280'000.0, -1'280'000.0, 640'000.0})) ==
				SpatialRegionStatus::Success &&
			Index.GetPrimaryAddress(Point) == SpatialAddress{1, {10'000, -10'000, 5'000}},
		"teleport updates only the final old/new region regardless of distance"
	);
	const auto AddressBeforeInvalidUpdate = Index.GetPrimaryAddress(Point);
	Check(
		Index.Update(Point, SpatialBounds::Point({std::numeric_limits<double>::max(), 0.0, 0.0})) ==
				SpatialRegionStatus::InvalidCoordinate &&
			Index.GetPrimaryAddress(Point) == AddressBeforeInvalidUpdate && Index.VerifyConsistency(),
		"invalid movement leaves the previously committed membership intact"
	);

	const ObjectId OneRegionBounds{2, 1};
	const ObjectId MaximumOverlapBounds{3, 1};
	const ObjectId LargeBounds{4, 1};
	Check(
		Index.Register(OneRegionBounds, {{0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}}) == SpatialRegionStatus::Success &&
			Index.GetMembershipCount(OneRegionBounds) == 1,
		"half-open bounds ending on a region boundary remain in one region"
	);
	Check(
		Index.Register(MaximumOverlapBounds, {{0.0, 0.0, 0.0}, {256.0, 256.0, 256.0}}) ==
				SpatialRegionStatus::Success &&
			Index.GetMembershipCount(MaximumOverlapBounds) == 8,
		"bounds may overlap the exact configured maximum region count"
	);
	Check(
		Index.Register(LargeBounds, {{0.0, 0.0, 0.0}, {384.0, 256.0, 256.0}}) == SpatialRegionStatus::Success &&
			Index.IsLargeObject(LargeBounds) && Index.GetMembershipCount(LargeBounds) == 0,
		"overlap limit plus one uses the bounded conservative large-object set"
	);
	const ObjectId SecondLarge{5, 1};
	const ObjectId RejectedLarge{6, 1};
	Check(
		Index.Register(SecondLarge, {{-1'000.0, -1'000.0, -1'000.0}, {1'000.0, 1'000.0, 1'000.0}}) ==
				SpatialRegionStatus::Success &&
			Index.Register(RejectedLarge, {{-2'000.0, -2'000.0, -2'000.0}, {2'000.0, 2'000.0, 2'000.0}}) ==
				SpatialRegionStatus::LargeObjectLimit &&
			!Index.Contains(RejectedLarge),
		"the large-object fallback is hard-bounded and registration is atomic"
	);
	Check(
		Query(Index, Scratch, {Sphere({128.0, 0.0, 0.0}, 1.0)}) == SpatialRegionStatus::Success &&
			Contains(Scratch.Candidates, OneRegionBounds) && Contains(Scratch.Candidates, MaximumOverlapBounds) &&
			Contains(Scratch.Candidates, LargeBounds) && Contains(Scratch.Candidates, SecondLarge),
		"neighbor and boundary-corner queries conservatively include overlapping and large roots"
	);
	const auto DedupBefore = Index.GetMetrics().CandidateDedupHits;
	Check(
		Query(Index, Scratch, {Sphere({0.0, 0.0, 0.0}, 200.0), Sphere({64.0, 64.0, 64.0}, 200.0)}) ==
				SpatialRegionStatus::Success &&
			std::ranges::count(Scratch.Candidates, MaximumOverlapBounds) == 1 &&
			Index.GetMetrics().CandidateDedupHits > DedupBefore,
		"overlapping multi-focus and multi-region candidates are deduplicated"
	);
	Check(
		Query(Index, Scratch, {Sphere({64.0, 64.0, 64.0}, 1.0), Sphere({1'280'000.0, -1'280'000.0, 640'000.0}, 1.0)}) ==
				SpatialRegionStatus::Success &&
			Contains(Scratch.Candidates, OneRegionBounds) && Contains(Scratch.Candidates, Point),
		"disjoint bounded focus volumes contribute to one deterministic candidate set"
	);
	Check(
		Query(Index, Scratch, {Sphere({0.0, 10'000.0, 0.0}, 1.0)}) == SpatialRegionStatus::Success &&
			!Contains(Scratch.Candidates, Point),
		"three-dimensional regions preserve vertical separation"
	);
	Check(
		Index.Update(MaximumOverlapBounds, {{512.0, 512.0, 512.0}, {768.0, 768.0, 768.0}}) ==
				SpatialRegionStatus::Success &&
			Index.VerifyConsistency(),
		"multi-region objects move transactionally and preserve index consistency"
	);
	Check(
		Index.Remove(MaximumOverlapBounds) == SpatialRegionStatus::Success &&
			Index.Remove(MaximumOverlapBounds) == SpatialRegionStatus::MissingObject && Index.VerifyConsistency(),
		"multi-region destruction removes every membership exactly once"
	);

	SpatialRegionIndexConfiguration QueryLimited = Configuration;
	QueryLimited.MaximumQueryRegions = 1;
	SpatialRegionIndex LimitedQuery(QueryLimited);
	SpatialRegionQueryScratch LimitedScratch;
	LimitedScratch.Reserve(QueryLimited);
	Check(
		Query(LimitedQuery, LimitedScratch, {Sphere({0.0, 0.0, 0.0}, 128.0)}) ==
				SpatialRegionStatus::QueryRegionLimit &&
			LimitedScratch.Candidates.empty(),
		"oversized region enumeration fails without a partial candidate result"
	);
	SpatialRegionIndexConfiguration CandidateLimited = Configuration;
	CandidateLimited.MaximumQueryCandidates = 2;
	CandidateLimited.MaximumQueryMembershipVisits = 8'000;
	SpatialRegionIndex Dense(CandidateLimited);
	SpatialRegionQueryScratch DenseScratch;
	DenseScratch.Reserve(CandidateLimited);
	Check(
		Dense.Register({20, 1}, SpatialBounds::Point({0.0, 0.0, 0.0})) == SpatialRegionStatus::Success &&
			Dense.Register({21, 1}, SpatialBounds::Point({1.0, 0.0, 0.0})) == SpatialRegionStatus::Success &&
			Dense.Register({22, 1}, SpatialBounds::Point({2.0, 0.0, 0.0})) == SpatialRegionStatus::Success &&
			Query(Dense, DenseScratch, {Sphere({0.0, 0.0, 0.0}, 10.0)}) == SpatialRegionStatus::QueryCandidateLimit &&
			DenseScratch.Candidates.empty(),
		"a dense region cannot create an unbounded or arbitrarily truncated candidate result"
	);

	SpatialRegionIndex RandomIndex(Configuration);
	SpatialRegionQueryScratch RandomScratch;
	RandomScratch.Reserve(Configuration);
	std::mt19937 Random(0x3A11CEu);
	std::uniform_real_distribution<double> Position(-4'096.0, 4'096.0);
	std::uniform_real_distribution<double> Radius(0.0, 600.0);
	std::vector<std::pair<ObjectId, glm::dvec3>> Reference;
	Reference.reserve(500);
	for (std::uint32_t Slot = 1; Slot <= 500; ++Slot) {
		const glm::dvec3 PointValue{Position(Random), Position(Random), Position(Random)};
		const ObjectId Id{Slot, 1};
		Check(
			RandomIndex.Register(Id, SpatialBounds::Point(PointValue)) == SpatialRegionStatus::Success,
			"random reference object registers"
		);
		Reference.emplace_back(Id, PointValue);
	}
	for (std::size_t Iteration = 0; Iteration < 200; ++Iteration) {
		const glm::dvec3 Focus{Position(Random), Position(Random), Position(Random)};
		const double QueryRadius = Radius(Random);
		Check(
			Query(RandomIndex, RandomScratch, {Sphere(Focus, QueryRadius)}) == SpatialRegionStatus::Success,
			"random reference query succeeds"
		);
		const double RadiusSquared = QueryRadius * QueryRadius;
		for (const auto &[Id, PointValue] : Reference)
			if (glm::dot(PointValue - Focus, PointValue - Focus) <= RadiusSquared)
				Check(Contains(RandomScratch.Candidates, Id), "region query has zero brute-force false negatives");
	}
	Check(RandomIndex.VerifyConsistency(), "randomized index retains bidirectional membership consistency");

	SpatialRegionIndexConfiguration RandomBoundsConfiguration = Configuration;
	RandomBoundsConfiguration.MaximumMemberships = 32'000;
	RandomBoundsConfiguration.MaximumRegionsPerObject = 64;
	RandomBoundsConfiguration.MaximumLargeObjects = 500;
	SpatialRegionIndex RandomBoundsIndex(RandomBoundsConfiguration);
	SpatialRegionQueryScratch RandomBoundsScratch;
	RandomBoundsScratch.Reserve(RandomBoundsConfiguration);
	std::uniform_real_distribution<double> HalfExtent(0.0, 100.0);
	std::vector<std::pair<ObjectId, SpatialBounds>> BoundsReference;
	BoundsReference.reserve(250);
	for (std::uint32_t Slot = 1; Slot <= 250; ++Slot) {
		const glm::dvec3 Center{Position(Random), Position(Random), Position(Random)};
		const glm::dvec3 Extent{HalfExtent(Random), HalfExtent(Random), HalfExtent(Random)};
		const SpatialBounds Bounds{Center - Extent, Center + Extent};
		const ObjectId Id{1'000 + Slot, 4};
		Check(
			RandomBoundsIndex.Register(Id, Bounds) == SpatialRegionStatus::Success,
			"random bounded spatial object registers"
		);
		BoundsReference.emplace_back(Id, Bounds);
	}
	for (std::size_t Iteration = 0; Iteration < 200; ++Iteration) {
		const glm::dvec3 Focus{Position(Random), Position(Random), Position(Random)};
		const double QueryRadius = Radius(Random);
		Check(
			Query(RandomBoundsIndex, RandomBoundsScratch, {Sphere(Focus, QueryRadius)}) == SpatialRegionStatus::Success,
			"random bounds reference query succeeds"
		);
		for (const auto &[Id, Bounds] : BoundsReference)
			if (BoundsIntersectSphere(Bounds, Focus, QueryRadius))
				Check(
					Contains(RandomBoundsScratch.Candidates, Id),
					"multi-region query has zero brute-force bounds false negatives"
				);
	}
	Check(
		RandomBoundsIndex.VerifyConsistency(),
		"randomized multi-region bounds retain bidirectional membership consistency"
	);

	SpatialRegionIndex Churn(Configuration);
	const ObjectId ChurnObject{100, 7};
	Check(
		Churn.Register(ChurnObject, SpatialBounds::Point({0.0, 0.0, 0.0})) == SpatialRegionStatus::Success,
		"churn object registers"
	);
	for (std::size_t Region = 1; Region <= 100'000; ++Region)
		if (Churn.Update(
				ChurnObject, SpatialBounds::Point({static_cast<double>(Region) * Configuration.RegionSize, 0.0, 0.0})
			) != SpatialRegionStatus::Success) {
			Check(false, "long teleport churn stays within bounded live topology");
			break;
		}
	const auto ChurnMetrics = Churn.GetMetrics();
	Check(
		ChurnMetrics.RegionCount == 1 && ChurnMetrics.MembershipCount == 1 &&
			ChurnMetrics.RegionBucketsRemoved == 100'000 && Churn.VerifyConsistency(),
		"100,000 unique teleports reclaim every empty historical region"
	);
	Check(
		Churn.Remove(ChurnObject) == SpatialRegionStatus::Success && Churn.GetMetrics().RegionCount == 0 &&
			Churn.GetMetrics().MembershipCount == 0,
		"teardown releases all region state"
	);
	const ObjectId OldGeneration{200, 1};
	const ObjectId NewGeneration{200, 2};
	Check(
		Churn.Register(OldGeneration, SpatialBounds::Point({0.0, 0.0, 0.0})) == SpatialRegionStatus::Success &&
			Churn.Remove(OldGeneration) == SpatialRegionStatus::Success &&
			Churn.Register(NewGeneration, SpatialBounds::Point({0.0, 0.0, 0.0})) == SpatialRegionStatus::Success &&
			Query(Churn, Scratch, {Sphere({0.0, 0.0, 0.0}, 1.0)}) == SpatialRegionStatus::Success &&
			!Contains(Scratch.Candidates, OldGeneration) && Contains(Scratch.Candidates, NewGeneration),
		"stale ObjectId generations cannot resurrect through region storage"
	);

	if (Failures == 0) std::cout << "Spatial region index tests passed\n";
	return Failures == 0 ? 0 : 1;
}

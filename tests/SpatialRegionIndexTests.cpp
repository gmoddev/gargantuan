#include "gargantuan/runtime/SpatialRegionIndex.hpp"
#include "gargantuan/runtime/SpatialRuntimeProjection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
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

	SpatialRegionQueryVolume Sphere(glm::dvec3 Center, double Radius, SpatialSpaceId Space = DefaultSpatialSpace) {
		return {.Space = Space, .Center = Center, .Radius = Radius};
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
		TightMembershipIndex.Register({900, 1}, DefaultSpatialSpace, {{0.0, 0.0, 0.0}, {256.0, 128.0, 128.0}}) ==
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
		const auto Address = SpatialCellAddressForPosition({Position, Position, Position}, 128.0);
		Check(
			Address && Address->Cell.X == Expected && Address->Cell.Y == Expected && Address->Cell.Z == Expected,
			"negative and positive coordinates use mathematical floor"
		);
	}
	const auto BelowBoundary = SpatialCellAddressForPosition({std::nextafter(128.0, 0.0), 0.0, 0.0}, 128.0);
	const auto ExactBoundary = SpatialCellAddressForPosition({128.0, 0.0, 0.0}, 128.0);
	const auto AboveBoundary = SpatialCellAddressForPosition(
		{std::nextafter(128.0, std::numeric_limits<double>::infinity()), 0.0, 0.0}, 128.0
	);
	Check(
		BelowBoundary && ExactBoundary && AboveBoundary && BelowBoundary->Cell.X == 0 && ExactBoundary->Cell.X == 1 &&
			AboveBoundary->Cell.X == 1,
		"region boundaries are deterministic half-open intervals"
	);
	const auto NegativeBelow = SpatialCellAddressForPosition(
		{std::nextafter(-128.0, -std::numeric_limits<double>::infinity()), 0.0, 0.0}, 128.0
	);
	const auto NegativeExact = SpatialCellAddressForPosition({-128.0, 0.0, 0.0}, 128.0);
	const auto NegativeAbove = SpatialCellAddressForPosition({std::nextafter(-128.0, 0.0), 0.0, 0.0}, 128.0);
	Check(
		NegativeBelow && NegativeExact && NegativeAbove && NegativeBelow->Cell.X == -2 && NegativeExact->Cell.X == -1 &&
			NegativeAbove->Cell.X == -1,
		"negative exact-boundary mapping is symmetric"
	);
	const auto Extreme = SpatialCellAddressForPosition({1.0e15, -1.0e15, 9.0e14}, 128.0);
	Check(Extreme.has_value(), "large finite addressable coordinates remain valid");
	Check(
		!SpatialCellAddressForPosition({std::numeric_limits<double>::max(), 0.0, 0.0}, 128.0) &&
			!SpatialCellAddressForPosition({std::numeric_limits<double>::infinity(), 0.0, 0.0}, 128.0) &&
			!SpatialCellAddressForPosition({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 128.0),
		"overflowing and non-finite coordinates fail rather than wrap"
	);
	const SpatialCellAddress AddressA{DefaultSpatialSpace, {2, -3, 4}};
	const SpatialCellAddress AddressB{DefaultSpatialSpace, {2, -3, 4}};
	const SpatialCellAddress AddressC{DefaultSpatialSpace, {2, -3, 5}};
	Check(
		AddressA == AddressB && AddressA != AddressC && AddressA < AddressC &&
			AddressA.StableHash() == AddressB.StableHash() && AddressA.ToString() == "space=(1,1) cell=(2,-3,4)",
		"SpatialCellAddress equality, order, hash, and diagnostics are deterministic"
	);
	Check(AddressA.StableHash() == 14'621'089'474'729'233'241ull, "SpatialCellAddress hash has a golden value");

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
		Index.Register(Point, DefaultSpatialSpace, SpatialBounds::Point({1.0, 1.0, 1.0})) ==
				SpatialRegionStatus::Success &&
			Index.GetMembershipCount(Point) == 1 &&
			Index.GetPrimaryAddress(Point) == SpatialCellAddress{DefaultSpatialSpace, {0, 0, 0}},
		"a point spatial root registers in one canonical region"
	);
	const auto BeforeSame = Index.GetMetrics();
	Check(
		Index.Update(Point, DefaultSpatialSpace, SpatialBounds::Point({127.0, 64.0, 127.0})) ==
				SpatialRegionStatus::Success &&
			Index.GetMetrics().MembershipMoves == BeforeSame.MembershipMoves &&
			Index.GetMetrics().SameRegionUpdates == BeforeSame.SameRegionUpdates + 1,
		"same-region movement performs no bucket mutation"
	);
	Check(
		Index.Update(Point, DefaultSpatialSpace, SpatialBounds::Point({128.0, 64.0, 127.0})) ==
				SpatialRegionStatus::Success &&
			Index.GetPrimaryAddress(Point) == SpatialCellAddress{DefaultSpatialSpace, {1, 0, 0}},
		"single-boundary movement changes canonical membership"
	);
	Check(
		Index.Update(Point, DefaultSpatialSpace, SpatialBounds::Point({1'280'000.0, -1'280'000.0, 640'000.0})) ==
				SpatialRegionStatus::Success &&
			Index.GetPrimaryAddress(Point) == SpatialCellAddress{DefaultSpatialSpace, {10'000, -10'000, 5'000}},
		"teleport updates only the final old/new region regardless of distance"
	);
	const auto AddressBeforeInvalidUpdate = Index.GetPrimaryAddress(Point);
	Check(
		Index.Update(
			Point, DefaultSpatialSpace, SpatialBounds::Point({std::numeric_limits<double>::max(), 0.0, 0.0})
		) == SpatialRegionStatus::InvalidCoordinate &&
			Index.GetPrimaryAddress(Point) == AddressBeforeInvalidUpdate && Index.VerifyConsistency(),
		"invalid movement leaves the previously committed membership intact"
	);

	const ObjectId OneRegionBounds{2, 1};
	const ObjectId MaximumOverlapBounds{3, 1};
	const ObjectId LargeBounds{4, 1};
	Check(
		Index.Register(OneRegionBounds, DefaultSpatialSpace, {{0.0, 0.0, 0.0}, {128.0, 128.0, 128.0}}) ==
				SpatialRegionStatus::Success &&
			Index.GetMembershipCount(OneRegionBounds) == 1,
		"half-open bounds ending on a region boundary remain in one region"
	);
	Check(
		Index.Register(MaximumOverlapBounds, DefaultSpatialSpace, {{0.0, 0.0, 0.0}, {256.0, 256.0, 256.0}}) ==
				SpatialRegionStatus::Success &&
			Index.GetMembershipCount(MaximumOverlapBounds) == 8,
		"bounds may overlap the exact configured maximum region count"
	);
	Check(
		Index.Register(LargeBounds, DefaultSpatialSpace, {{0.0, 0.0, 0.0}, {384.0, 256.0, 256.0}}) ==
				SpatialRegionStatus::Success &&
			Index.IsLargeObject(LargeBounds) && Index.GetMembershipCount(LargeBounds) == 0,
		"overlap limit plus one uses the bounded conservative large-object set"
	);
	const ObjectId SecondLarge{5, 1};
	const ObjectId RejectedLarge{6, 1};
	Check(
		Index.Register(
			SecondLarge, DefaultSpatialSpace, {{-1'000.0, -1'000.0, -1'000.0}, {1'000.0, 1'000.0, 1'000.0}}
		) == SpatialRegionStatus::Success &&
			Index.Register(
				RejectedLarge, DefaultSpatialSpace, {{-2'000.0, -2'000.0, -2'000.0}, {2'000.0, 2'000.0, 2'000.0}}
			) == SpatialRegionStatus::LargeObjectLimit &&
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
		Index.Update(MaximumOverlapBounds, DefaultSpatialSpace, {{512.0, 512.0, 512.0}, {768.0, 768.0, 768.0}}) ==
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
		Dense.Register({20, 1}, DefaultSpatialSpace, SpatialBounds::Point({0.0, 0.0, 0.0})) ==
				SpatialRegionStatus::Success &&
			Dense.Register({21, 1}, DefaultSpatialSpace, SpatialBounds::Point({1.0, 0.0, 0.0})) ==
				SpatialRegionStatus::Success &&
			Dense.Register({22, 1}, DefaultSpatialSpace, SpatialBounds::Point({2.0, 0.0, 0.0})) ==
				SpatialRegionStatus::Success &&
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
			RandomIndex.Register(Id, DefaultSpatialSpace, SpatialBounds::Point(PointValue)) ==
				SpatialRegionStatus::Success,
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
			RandomBoundsIndex.Register(Id, DefaultSpatialSpace, Bounds) == SpatialRegionStatus::Success,
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
		Churn.Register(ChurnObject, DefaultSpatialSpace, SpatialBounds::Point({0.0, 0.0, 0.0})) ==
			SpatialRegionStatus::Success,
		"churn object registers"
	);
	for (std::size_t Region = 1; Region <= 100'000; ++Region)
		if (Churn.Update(
				ChurnObject,
				DefaultSpatialSpace,
				SpatialBounds::Point({static_cast<double>(Region) * Configuration.RegionSize, 0.0, 0.0})
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
		Churn.Register(OldGeneration, DefaultSpatialSpace, SpatialBounds::Point({0.0, 0.0, 0.0})) ==
				SpatialRegionStatus::Success &&
			Churn.Remove(OldGeneration) == SpatialRegionStatus::Success &&
			Churn.Register(NewGeneration, DefaultSpatialSpace, SpatialBounds::Point({0.0, 0.0, 0.0})) ==
				SpatialRegionStatus::Success &&
			Query(Churn, Scratch, {Sphere({0.0, 0.0, 0.0}, 1.0)}) == SpatialRegionStatus::Success &&
			!Contains(Scratch.Candidates, OldGeneration) && Contains(Scratch.Candidates, NewGeneration),
		"stale ObjectId generations cannot resurrect through region storage"
	);

	SpatialRuntimeProjectionConfiguration ProjectionConfiguration;
	ProjectionConfiguration.Index = Configuration;
	ProjectionConfiguration.MaximumSpaces = 4;
	SpatialRuntimeProjectionConfiguration InvalidProjectionConfiguration = ProjectionConfiguration;
	InvalidProjectionConfiguration.MaximumSpaces = 0;
	Check(!InvalidProjectionConfiguration.IsValid(), "zero runtime spatial spaces is invalid");
	InvalidProjectionConfiguration = ProjectionConfiguration;
	InvalidProjectionConfiguration.MaximumSpaces = MaximumSpatialRuntimeSpaces + 1;
	Check(!InvalidProjectionConfiguration.IsValid(), "runtime spatial space capacity has a hard ceiling");
	SpatialRuntimeProjectionStore Projections(ProjectionConfiguration);
	const auto SpaceB = Projections.CreateSpace();
	Check(
		SpaceB && *SpaceB != Projections.GetDefaultSpace() && Projections.IsSpaceAlive(*SpaceB),
		"an isolated runtime space receives strong generation-safe identity"
	);
	const ObjectId PartA{300, 1};
	const ObjectId CharacterA{301, 1};
	const ObjectId PartB{302, 1};
	const ObjectId CharacterB{303, 1};
	const SpatialBounds OriginBounds = SpatialBounds::Point({0.0, 0.0, 0.0});
	const SpatialBounds CharacterBounds = SpatialBounds::Point({10.0, 0.0, 0.0});
	Check(
		SpaceB &&
			Projections.Register(PartA, {Projections.GetDefaultSpace(), CFrame(0.0f, 0.0f, 0.0f)}, OriginBounds) ==
				SpatialRuntimeProjectionStatus::Success &&
			Projections.Register(
				CharacterA, {Projections.GetDefaultSpace(), CFrame(10.0f, 0.0f, 0.0f)}, CharacterBounds
			) == SpatialRuntimeProjectionStatus::Success &&
			Projections.Register(PartB, {*SpaceB, CFrame(0.0f, 0.0f, 0.0f)}, OriginBounds) ==
				SpatialRuntimeProjectionStatus::Success &&
			Projections.Register(CharacterB, {*SpaceB, CFrame(10.0f, 0.0f, 0.0f)}, CharacterBounds) ==
				SpatialRuntimeProjectionStatus::Success,
		"parts and Character projections may occupy identical local coordinates in isolated spaces"
	);
	SpatialRegionQueryScratch ProjectionScratch;
	ProjectionScratch.Reserve(ProjectionConfiguration.Index);
	const SpatialRegionQueryVolume QueryA = Sphere({0.0, 0.0, 0.0}, 16.0, Projections.GetDefaultSpace());
	Check(
		Projections.Query(std::span(&QueryA, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			Contains(ProjectionScratch.Candidates, PartA) && Contains(ProjectionScratch.Candidates, CharacterA) &&
			!Contains(ProjectionScratch.Candidates, PartB) && !Contains(ProjectionScratch.Candidates, CharacterB),
		"a DefaultSpace query cannot alias identical local coordinates in another space"
	);
	const SpatialRegionQueryVolume QueryB = Sphere({0.0, 0.0, 0.0}, 16.0, *SpaceB);
	Check(
		Projections.Query(std::span(&QueryB, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			!Contains(ProjectionScratch.Candidates, PartA) && !Contains(ProjectionScratch.Candidates, CharacterA) &&
			Contains(ProjectionScratch.Candidates, PartB) && Contains(ProjectionScratch.Candidates, CharacterB),
		"an isolated-space query returns only that space's local candidates"
	);
	Check(
		Projections.GetCellAddress(PartA) == SpatialCellAddress{Projections.GetDefaultSpace(), {0, 0, 0}} &&
			Projections.GetCellAddress(PartB) == SpatialCellAddress{*SpaceB, {0, 0, 0}} &&
			Projections.GetCellAddress(PartA) != Projections.GetCellAddress(PartB),
		"cell identity includes full space lifetime rather than local coordinates alone"
	);
	SpatialRuntimeProjectionConfiguration LargeProjectionConfiguration = ProjectionConfiguration;
	LargeProjectionConfiguration.Index.MaximumLargeObjects = 2;
	SpatialRuntimeProjectionStore LargeProjections(LargeProjectionConfiguration);
	const auto LargeSpaceB = LargeProjections.CreateSpace();
	const ObjectId LargeA{310, 1};
	const ObjectId LargeB{311, 1};
	const SpatialBounds IsolatedLargeBounds{{-1'000.0, -1'000.0, -1'000.0}, {1'000.0, 1'000.0, 1'000.0}};
	Check(
		LargeSpaceB &&
			LargeProjections.Register(LargeA, {LargeProjections.GetDefaultSpace(), CFrame()}, IsolatedLargeBounds) ==
				SpatialRuntimeProjectionStatus::Success &&
			LargeProjections.Register(LargeB, {*LargeSpaceB, CFrame()}, IsolatedLargeBounds) ==
				SpatialRuntimeProjectionStatus::Success &&
			LargeProjections.Query(std::span(&QueryA, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			Contains(ProjectionScratch.Candidates, LargeA) && !Contains(ProjectionScratch.Candidates, LargeB),
		"large-object conservative fallback is partitioned by complete space identity"
	);
	Check(
		Projections.MarkDirty(PartA) == SpatialRuntimeProjectionStatus::Success &&
			Projections.MarkDirty(PartA) == SpatialRuntimeProjectionStatus::Success &&
			Projections.GetDirtyObjects().size() == 1 && Projections.GetMetrics().DirtyCoalesces == 1,
		"repeated semantic movement marks coalesce into one bounded projection update"
	);
	Projections.ClearDirtyObjects();
	const auto RevisionBeforeTransfer = Projections.Get(CharacterA)->Revision;
	Check(
		Projections.Transfer(CharacterA, {*SpaceB, CFrame(10.0f, 0.0f, 0.0f)}, CharacterBounds) ==
				SpatialRuntimeProjectionStatus::Success &&
			Projections.Get(CharacterA)->Pose.Space == *SpaceB &&
			Projections.Get(CharacterA)->Revision == RevisionBeforeTransfer + 1 &&
			Projections.Query(std::span(&QueryA, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			!Contains(ProjectionScratch.Candidates, CharacterA) &&
			Projections.Query(std::span(&QueryB, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			Contains(ProjectionScratch.Candidates, CharacterA) && Projections.VerifyConsistency(),
		"space transfer preserves ObjectId and commits destination index state atomically"
	);
	const auto TransferState = *Projections.Get(CharacterA);
	const SpatialBounds InvalidTransferBounds{{1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
	Check(
		Projections.Transfer(
			CharacterA, {Projections.GetDefaultSpace(), CFrame(100.0f, 0.0f, 0.0f)}, InvalidTransferBounds
		) == SpatialRuntimeProjectionStatus::InvalidProjection &&
			Projections.Get(CharacterA)->Pose.Space == TransferState.Pose.Space &&
			Projections.Get(CharacterA)->Revision == TransferState.Revision && Projections.VerifyConsistency(),
		"failed transfer leaves the complete source projection and memberships committed"
	);
	Check(
		Projections.DestroySpace(*SpaceB) == SpatialRuntimeProjectionStatus::SpaceOccupied,
		"an occupied runtime space cannot be retired beneath live projections"
	);
	Check(
		Projections.Remove(PartB) == SpatialRuntimeProjectionStatus::Success &&
			Projections.Remove(CharacterB) == SpatialRuntimeProjectionStatus::Success &&
			Projections.Remove(CharacterA) == SpatialRuntimeProjectionStatus::Success &&
			Projections.DestroySpace(*SpaceB) == SpatialRuntimeProjectionStatus::Success &&
			!Projections.IsSpaceAlive(*SpaceB),
		"projection removal precedes generation-safe space retirement"
	);
	const auto ReusedSpace = Projections.CreateSpace();
	Check(
		ReusedSpace && ReusedSpace->Slot == SpaceB->Slot && ReusedSpace->Generation == SpaceB->Generation + 1 &&
			!Projections.IsSpaceAlive(*SpaceB) && Projections.IsSpaceAlive(*ReusedSpace) &&
			Projections.Query(std::span(&QueryB, 1), ProjectionScratch) == SpatialRegionStatus::InvalidSpace &&
			ProjectionScratch.Candidates.empty(),
		"space-slot reuse rejects stale generations at query boundaries"
	);
	Check(
		Projections.MarkDirty(PartA) == SpatialRuntimeProjectionStatus::Success &&
			Projections.Remove(PartA) == SpatialRuntimeProjectionStatus::Success && Projections.Get(PartA) == nullptr &&
			Projections.GetDirtyObjects().size() == 1,
		"destroyed projections cannot resolve even while a stale dirty token awaits bounded cleanup"
	);
	Projections.ClearDirtyObjects();
	const ObjectId PartANewGeneration{PartA.Slot, PartA.Generation + 1};
	Check(
		Projections.Register(
			PartANewGeneration, {Projections.GetDefaultSpace(), CFrame(0.0f, 0.0f, 0.0f)}, OriginBounds
		) == SpatialRuntimeProjectionStatus::Success &&
			Projections.Get(PartA) == nullptr && Projections.Get(PartANewGeneration) != nullptr &&
			Projections.VerifyConsistency(),
		"a reused ObjectId slot cannot inherit its prior projection generation"
	);

	SpatialRuntimeProjectionConfiguration IsolationConfiguration = ProjectionConfiguration;
	IsolationConfiguration.MaximumSpaces = 2;
	SpatialRuntimeProjectionStore IsolationProjections(IsolationConfiguration);
	const auto IsolationSpaceB = IsolationProjections.CreateSpace();
	for (std::uint32_t Index = 0; Index < 100; ++Index) {
		Check(
			IsolationProjections.Register(
				{5'000 + Index, 1}, {IsolationProjections.GetDefaultSpace(), CFrame()}, OriginBounds
			) == SpatialRuntimeProjectionStatus::Success &&
				IsolationProjections.Register({5'100 + Index, 1}, {*IsolationSpaceB, CFrame()}, OriginBounds) ==
					SpatialRuntimeProjectionStatus::Success,
			"identical-coordinate isolated-space fixture registration succeeds"
		);
	}
	const SpatialRegionQueryVolume IsolationQueryA = Sphere({0.0, 0.0, 0.0}, 1.0);
	const SpatialRegionQueryVolume IsolationQueryB = Sphere({0.0, 0.0, 0.0}, 1.0, *IsolationSpaceB);
	Check(
		IsolationProjections.Query(std::span(&IsolationQueryA, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			ProjectionScratch.Candidates.size() == 100 &&
			std::ranges::all_of(
				ProjectionScratch.Candidates,
				[](ObjectId Object) { return Object.Slot >= 5'000 && Object.Slot < 5'100; }
			),
		"100 colocated DefaultSpace projections exclude all 100 isolated-space projections"
	);
	Check(
		IsolationProjections.Query(std::span(&IsolationQueryB, 1), ProjectionScratch) == SpatialRegionStatus::Success &&
			ProjectionScratch.Candidates.size() == 100 &&
			std::ranges::all_of(
				ProjectionScratch.Candidates,
				[](ObjectId Object) { return Object.Slot >= 5'100 && Object.Slot < 5'200; }
			),
		"100 colocated isolated-space projections exclude all 100 DefaultSpace projections"
	);

	SpatialRuntimeProjectionStore ChurnProjections(IsolationConfiguration);
	const auto ChurnSpaceB = ChurnProjections.CreateSpace();
	const ObjectId ProjectionChurnObject{6'000, 1};
	Check(
		ChurnProjections.Register(
			ProjectionChurnObject, {ChurnProjections.GetDefaultSpace(), CFrame()}, OriginBounds
		) == SpatialRuntimeProjectionStatus::Success,
		"projection churn fixture registration succeeds"
	);
	for (std::size_t Mutation = 0; Mutation < 100'000; ++Mutation)
		Check(
			ChurnProjections.MarkDirty(ProjectionChurnObject) == SpatialRuntimeProjectionStatus::Success,
			"projection dirty churn remains bounded"
		);
	Check(
		ChurnProjections.GetDirtyObjects().size() == 1 && ChurnProjections.GetMetrics().DirtyCoalesces == 99'999,
		"100,000 transform notifications coalesce to one current-state synchronization token"
	);
	ChurnProjections.ClearDirtyObjects();
	for (std::size_t Transfer = 0; Transfer < 100; ++Transfer) {
		const auto Destination = (Transfer & 1u) == 0 ? *ChurnSpaceB : ChurnProjections.GetDefaultSpace();
		Check(
			ChurnProjections.Transfer(ProjectionChurnObject, {Destination, CFrame()}, OriginBounds) ==
				SpatialRuntimeProjectionStatus::Success,
			"repeated isolated-space transfer succeeds"
		);
	}
	Check(
		ChurnProjections.Get(ProjectionChurnObject)->Pose.Space == ChurnProjections.GetDefaultSpace() &&
			ChurnProjections.Get(ProjectionChurnObject)->Revision == 101 && ChurnProjections.VerifyConsistency(),
		"100 transfer cycles retain one projection, one identity, and coherent memberships"
	);
	Check(
		ChurnProjections.Remove(ProjectionChurnObject) == SpatialRuntimeProjectionStatus::Success,
		"projection churn object removal succeeds"
	);
	Check(
		ChurnProjections.DestroySpace(*ChurnSpaceB) == SpatialRuntimeProjectionStatus::Success,
		"projection churn destination retires after its final transfer participant leaves"
	);
	for (std::uint32_t Generation = 1; Generation <= 100; ++Generation) {
		const ObjectId LifetimeObject{6'100, Generation};
		Check(
			ChurnProjections.Register(LifetimeObject, {ChurnProjections.GetDefaultSpace(), CFrame()}, OriginBounds) ==
					SpatialRuntimeProjectionStatus::Success &&
				ChurnProjections.Remove(LifetimeObject) == SpatialRuntimeProjectionStatus::Success &&
				ChurnProjections.Get(LifetimeObject) == nullptr,
			"create/destroy lifecycle cannot retain an old object generation"
		);
	}
	for (std::size_t Lifetime = 0; Lifetime < 100; ++Lifetime) {
		const auto Space = ChurnProjections.CreateSpace();
		Check(
			Space && ChurnProjections.DestroySpace(*Space) == SpatialRuntimeProjectionStatus::Success &&
				!ChurnProjections.IsSpaceAlive(*Space),
			"create/destroy lifecycle cannot retain an old space generation"
		);
	}
	Check(
		ChurnProjections.GetProjectionCount() == 0 && ChurnProjections.VerifyConsistency(),
		"projection and space lifecycle stress returns to an empty coherent baseline"
	);
	for (const std::size_t SpaceCapacity : {1u, 2u, 8u, 32u, 256u}) {
		SpatialRuntimeProjectionConfiguration SpaceScaleConfiguration = ProjectionConfiguration;
		SpaceScaleConfiguration.MaximumSpaces = SpaceCapacity;
		SpatialRuntimeProjectionStore SpaceScaleProjections(SpaceScaleConfiguration);
		for (std::size_t Space = 1; Space < SpaceCapacity; ++Space)
			Check(SpaceScaleProjections.CreateSpace().has_value(), "configured runtime spatial space is admitted");
		Check(
			SpaceScaleProjections.GetSpaceCount() == SpaceCapacity && !SpaceScaleProjections.CreateSpace().has_value(),
			"1/2/8/32/256-space capacities are exact and hard-bounded"
		);
	}

	SpatialRuntimeProjectionStore RandomProjections(ProjectionConfiguration);
	const auto RandomSpaceB = RandomProjections.CreateSpace();
	struct ReferenceProjection {
		SpatialSpaceId Space;
		glm::dvec3 Position;
	};
	std::map<ObjectId, ReferenceProjection> ProjectionReference;
	std::mt19937 ProjectionRandom(0x3B0A5Eu);
	std::uniform_real_distribution<double> ProjectionPosition(-8'192.0, 8'192.0);
	std::uniform_int_distribution<int> ProjectionOperation(0, 3);
	for (std::size_t Iteration = 0; Iteration < 10'000; ++Iteration) {
		const ObjectId Object{static_cast<std::uint32_t>(400 + ProjectionRandom() % 1'000), 9};
		const auto Operation = ProjectionOperation(ProjectionRandom);
		auto Existing = ProjectionReference.find(Object);
		if (Operation == 0 && Existing == ProjectionReference.end()) {
			const auto Space = (ProjectionRandom() & 1u) == 0 ? RandomProjections.GetDefaultSpace() : *RandomSpaceB;
			const glm::dvec3 PointValue{
				ProjectionPosition(ProjectionRandom),
				ProjectionPosition(ProjectionRandom),
				ProjectionPosition(ProjectionRandom)
			};
			Check(
				RandomProjections.Register(
					Object, {Space, CFrame(glm::vec3(PointValue))}, SpatialBounds::Point(PointValue)
				) == SpatialRuntimeProjectionStatus::Success,
				"randomized projection create succeeds"
			);
			ProjectionReference.emplace(Object, ReferenceProjection{Space, PointValue});
		} else if (Operation == 1 && Existing != ProjectionReference.end()) {
			const glm::dvec3 PointValue{
				ProjectionPosition(ProjectionRandom),
				ProjectionPosition(ProjectionRandom),
				ProjectionPosition(ProjectionRandom)
			};
			Check(
				RandomProjections.Update(
					Object, {Existing->second.Space, CFrame(glm::vec3(PointValue))}, SpatialBounds::Point(PointValue)
				) == SpatialRuntimeProjectionStatus::Success,
				"randomized projection update succeeds"
			);
			Existing->second.Position = PointValue;
		} else if (Operation == 2 && Existing != ProjectionReference.end()) {
			const auto Space = Existing->second.Space == RandomProjections.GetDefaultSpace()
								   ? *RandomSpaceB
								   : RandomProjections.GetDefaultSpace();
			Check(
				RandomProjections.Transfer(
					Object,
					{Space, CFrame(glm::vec3(Existing->second.Position))},
					SpatialBounds::Point(Existing->second.Position)
				) == SpatialRuntimeProjectionStatus::Success,
				"randomized projection transfer succeeds"
			);
			Existing->second.Space = Space;
		} else if (Operation == 3 && Existing != ProjectionReference.end()) {
			Check(
				RandomProjections.Remove(Object) == SpatialRuntimeProjectionStatus::Success,
				"randomized projection removal succeeds"
			);
			ProjectionReference.erase(Existing);
		}
		if (Iteration % 37 == 0) {
			Check(RandomProjections.VerifyConsistency(), "randomized projection/index state remains consistent");
			for (const auto Space : {RandomProjections.GetDefaultSpace(), *RandomSpaceB}) {
				const SpatialRegionQueryVolume Volume = Sphere({0.0, 0.0, 0.0}, 512.0, Space);
				Check(
					RandomProjections.Query(std::span(&Volume, 1), ProjectionScratch) == SpatialRegionStatus::Success,
					"randomized isolated-space query succeeds"
				);
				for (const auto Candidate : ProjectionScratch.Candidates) {
					auto Expected = ProjectionReference.find(Candidate);
					Check(
						Expected != ProjectionReference.end() && Expected->second.Space == Space,
						"randomized query candidates never cross space identity"
					);
				}
			}
		}
	}
	Check(RandomProjections.VerifyConsistency(), "long randomized projection lifecycle finishes consistent");

	if (Failures == 0) std::cout << "Spatial region index tests passed\n";
	return Failures == 0 ? 0 : 1;
}

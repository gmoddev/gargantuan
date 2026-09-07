#pragma once

#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/SpatialTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace gargantuan {
	inline constexpr double DefaultSpatialRegionSize = 128.0;
	inline constexpr std::size_t MaximumSpatialRegionIndexObjects = 1'000'000;
	inline constexpr std::size_t MaximumSpatialRegionIndexRegions = 1'000'000;
	inline constexpr std::size_t MaximumSpatialRegionIndexMemberships = 4'000'000;
	inline constexpr std::size_t MaximumSpatialRegionsPerObject = 4'096;
	inline constexpr std::size_t MaximumSpatialLargeObjects = 4'096;
	inline constexpr std::size_t MaximumSpatialQueryVolumes = 64;
	inline constexpr std::size_t MaximumSpatialQueryRegions = 65'536;
	inline constexpr std::size_t MaximumSpatialQueryCandidates = 1'000'000;
	inline constexpr std::size_t MaximumSpatialQueryMembershipVisits = 4'000'000;

	struct SpatialBounds {
		glm::dvec3 Minimum{0.0};
		glm::dvec3 Maximum{0.0};

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] static SpatialBounds Point(glm::dvec3 Position) {
			return {Position, Position};
		}
	};

	struct SpatialRegionQueryVolume {
		SpatialSpaceId Space = DefaultSpatialSpace;
		glm::dvec3 Center{0.0};
		double Radius = 0.0;

		[[nodiscard]] bool IsValid() const;
	};

	struct SpatialRegionIndexConfiguration {
		double RegionSize = DefaultSpatialRegionSize;
		std::size_t MaximumObjects = 65'536;
		std::size_t MaximumRegions = 65'536;
		std::size_t MaximumMemberships = 262'144;
		std::size_t MaximumRegionsPerObject = 64;
		std::size_t MaximumLargeObjects = 1'024;
		std::size_t MaximumQueryVolumes = 4;
		std::size_t MaximumQueryRegions = 4'096;
		std::size_t MaximumQueryCandidates = 65'536;
		std::size_t MaximumQueryMembershipVisits = 262'144;

		[[nodiscard]] bool IsValid() const;
	};

	enum class SpatialRegionStatus : std::uint8_t {
		Success,
		InvalidConfiguration,
		InvalidIdentity,
		InvalidSpace,
		InvalidBounds,
		InvalidCoordinate,
		DuplicateObject,
		MissingObject,
		ObjectLimit,
		RegionLimit,
		MembershipLimit,
		LargeObjectLimit,
		QueryVolumeLimit,
		QueryRegionLimit,
		QueryCandidateLimit,
		QueryMembershipVisitLimit,
		AllocationFailure,
	};

	[[nodiscard]] const char *SpatialRegionStatusName(SpatialRegionStatus Status) noexcept;

	struct SpatialRegionQueryScratch {
		std::vector<SpatialCellAddress> Regions;
		std::vector<ObjectId> Candidates;

		void Reserve(const SpatialRegionIndexConfiguration &Configuration);
		void Clear();
	};

	struct SpatialRegionIndexMetrics {
		std::uint64_t RegionCount = 0;
		std::uint64_t SpatialObjectCount = 0;
		std::uint64_t MembershipCount = 0;
		std::uint64_t LargeObjectCount = 0;
		std::uint64_t ObjectRegistrations = 0;
		std::uint64_t ObjectRemovals = 0;
		std::uint64_t MembershipMoves = 0;
		std::uint64_t SameRegionUpdates = 0;
		std::uint64_t RegionBucketsCreated = 0;
		std::uint64_t RegionBucketsRemoved = 0;
		std::uint64_t PeakRegionOccupancy = 0;
		std::uint64_t RegionQueries = 0;
		std::uint64_t RegionsVisited = 0;
		std::uint64_t CandidateMembershipVisits = 0;
		std::uint64_t CandidateObjects = 0;
		std::uint64_t CandidateDedupHits = 0;
		std::uint64_t LargeObjectCandidates = 0;
		std::uint64_t QueryLimitFailures = 0;
		std::uint64_t CandidateLimitFailures = 0;
	};

	[[nodiscard]] std::optional<SpatialCellAddress> SpatialCellAddressForPosition(
		glm::dvec3 Position, double RegionSize = DefaultSpatialRegionSize, SpatialSpaceId Space = DefaultSpatialSpace
	);

	class SpatialRegionIndex final {
	  public:
		explicit SpatialRegionIndex(SpatialRegionIndexConfiguration Configuration = {});
		~SpatialRegionIndex();
		SpatialRegionIndex(const SpatialRegionIndex &) = delete;
		SpatialRegionIndex &operator=(const SpatialRegionIndex &) = delete;

		[[nodiscard]] SpatialRegionStatus Register(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds);
		[[nodiscard]] SpatialRegionStatus Update(ObjectId Object, SpatialSpaceId Space, const SpatialBounds &Bounds);
		[[nodiscard]] SpatialRegionStatus Remove(ObjectId Object);
		[[nodiscard]] SpatialRegionStatus
		Query(std::span<const SpatialRegionQueryVolume> Volumes, SpatialRegionQueryScratch &Scratch);

		[[nodiscard]] bool Contains(ObjectId Object) const;
		[[nodiscard]] bool IsLargeObject(ObjectId Object) const;
		[[nodiscard]] std::optional<SpatialCellAddress> GetPrimaryAddress(ObjectId Object) const;
		[[nodiscard]] std::size_t GetMembershipCount(ObjectId Object) const;
		[[nodiscard]] bool VerifyConsistency() const;
		[[nodiscard]] const SpatialRegionIndexConfiguration &GetConfiguration() const;
		[[nodiscard]] SpatialRegionIndexMetrics GetMetrics() const;

	  private:
		struct Implementation;
		std::unique_ptr<Implementation> State;
	};
}

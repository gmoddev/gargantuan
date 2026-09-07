#pragma once

#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/SpatialRegionIndex.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace gargantuan {
	inline constexpr std::size_t MaximumSpatialRuntimeSpaces = 256;

	struct SpatialRuntimeProjectionConfiguration {
		SpatialRegionIndexConfiguration Index;
		std::size_t MaximumSpaces = 16;

		[[nodiscard]] bool IsValid() const;
	};

	enum class SpatialRuntimeProjectionStatus : std::uint8_t {
		Success,
		InvalidConfiguration,
		InvalidObject,
		InvalidSpace,
		InvalidProjection,
		DuplicateProjection,
		MissingProjection,
		SpaceLimit,
		SpaceOccupied,
		DirtyLimit,
		IndexFailure,
		AllocationFailure,
	};

	[[nodiscard]] const char *SpatialRuntimeProjectionStatusName(SpatialRuntimeProjectionStatus Status) noexcept;

	struct SpatialRuntimeProjection {
		SpatialPose Pose;
		SpatialBounds Bounds;
		std::uint64_t Revision = 0;
		bool Dirty = false;
	};

	struct SpatialRuntimeProjectionMetrics {
		std::uint64_t ActiveSpaces = 0;
		std::uint64_t ActiveProjections = 0;
		std::uint64_t DirtyProjections = 0;
		std::uint64_t PeakDirtyProjections = 0;
		std::uint64_t SpaceCreations = 0;
		std::uint64_t SpaceDestructions = 0;
		std::uint64_t ProjectionCreations = 0;
		std::uint64_t ProjectionUpdates = 0;
		std::uint64_t ProjectionTransfers = 0;
		std::uint64_t ProjectionRemovals = 0;
		std::uint64_t DirtyMarks = 0;
		std::uint64_t DirtyCoalesces = 0;
		std::uint64_t FailedTransactions = 0;
	};

	// Owns derived spatial participation for one runtime/world context. Instance
	// identity and properties remain authoritative; this store contains no
	// Instance pointer and cannot extend an Instance generation lifetime.
	class SpatialRuntimeProjectionStore final {
	  public:
		explicit SpatialRuntimeProjectionStore(SpatialRuntimeProjectionConfiguration Configuration = {});
		~SpatialRuntimeProjectionStore();
		SpatialRuntimeProjectionStore(const SpatialRuntimeProjectionStore &) = delete;
		SpatialRuntimeProjectionStore &operator=(const SpatialRuntimeProjectionStore &) = delete;

		[[nodiscard]] SpatialSpaceId GetDefaultSpace() const noexcept;
		[[nodiscard]] std::optional<SpatialSpaceId> CreateSpace();
		[[nodiscard]] SpatialRuntimeProjectionStatus DestroySpace(SpatialSpaceId Space);
		[[nodiscard]] bool IsSpaceAlive(SpatialSpaceId Space) const;

		[[nodiscard]] SpatialRuntimeProjectionStatus
		Register(ObjectId Object, const SpatialPose &Pose, const SpatialBounds &Bounds);
		[[nodiscard]] SpatialRuntimeProjectionStatus
		Update(ObjectId Object, const SpatialPose &Pose, const SpatialBounds &Bounds);
		[[nodiscard]] SpatialRuntimeProjectionStatus
		Transfer(ObjectId Object, const SpatialPose &Destination, const SpatialBounds &DestinationBounds);
		[[nodiscard]] SpatialRuntimeProjectionStatus Remove(ObjectId Object);

		[[nodiscard]] SpatialRuntimeProjectionStatus MarkDirty(ObjectId Object);
		[[nodiscard]] std::span<const ObjectId> GetDirtyObjects() const;
		void ClearDirtyObjects();

		[[nodiscard]] const SpatialRuntimeProjection *Get(ObjectId Object) const;
		[[nodiscard]] std::optional<SpatialCellAddress> GetCellAddress(ObjectId Object) const;
		[[nodiscard]] bool IsLargeObject(ObjectId Object) const;
		[[nodiscard]] SpatialRegionStatus
		Query(std::span<const SpatialRegionQueryVolume> Volumes, SpatialRegionQueryScratch &Scratch);

		[[nodiscard]] bool VerifyConsistency() const;
		[[nodiscard]] std::size_t GetProjectionCount() const;
		[[nodiscard]] std::size_t GetSpaceCount() const;
		[[nodiscard]] SpatialRuntimeProjectionMetrics GetMetrics() const;
		[[nodiscard]] SpatialRegionIndexMetrics GetIndexMetrics() const;
		[[nodiscard]] SpatialRegionStatus GetLastIndexFailure() const;

	  private:
		struct Implementation;
		std::unique_ptr<Implementation> State;
	};
}

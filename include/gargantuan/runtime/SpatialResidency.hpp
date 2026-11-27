#pragma once

#include "gargantuan/runtime/SpatialDestination.hpp"
#include <chrono>
#include <expected>
#include <optional>
#include <span>

namespace gargantuan {
	class WorldRoot;
	struct SpatialCollisionQueue;
	using SpatialResidencyClock = std::chrono::steady_clock;
	struct SpatialResidencyAudience {
		ObjectId Owner;
		std::uint64_t Epoch{};
		bool operator==(const SpatialResidencyAudience &) const = default;
	};
	enum class SpatialDemandPriority : std::uint8_t { Background, Normal, Interactive };
	enum class SpatialLeaseLife : std::uint8_t { Active, Released, Expired, Stale };
	enum class SpatialCollisionCondition : std::uint8_t { Pending, Ready, Unavailable };
	enum class SpatialRequestFailure : std::uint8_t { Invalid, Unavailable, Budget, Stale };
	struct SpatialCollisionSnapshot {
		SpatialLeaseLife Life{SpatialLeaseLife::Stale};
		SpatialCollisionCondition Collision{SpatialCollisionCondition::Pending};
		std::uint64_t Revision{};
	};
	struct SpatialCollisionWork {
		std::size_t Holders{}, DistinctRequests{}, Captures{};
	};
	class SpatialCollisionLease final {
	  public:
		~SpatialCollisionLease();
		SpatialCollisionLease(const SpatialCollisionLease &) = delete;
		SpatialCollisionLease &operator=(const SpatialCollisionLease &) = delete;
		[[nodiscard]] SpatialCollisionSnapshot GetStatus(SpatialResidencyClock::time_point Now) const;
		// A local published observation. A movement commit must validate it against the live world.
		[[nodiscard]] std::shared_ptr<const SpatialDestinationReceipt>
		GetReceipt(SpatialResidencyClock::time_point Now) const;
		bool Renew(SpatialResidencyClock::time_point Now, std::chrono::milliseconds Duration);
		bool SetPriority(SpatialResidencyClock::time_point Now, SpatialDemandPriority Priority);
		bool SetArea(
			SpatialResidencyClock::time_point Now,
			const SpatialDestinationBounds &Bounds,
			std::span<const ObjectId> RequiredOwners = {}
		);
		void Release();

	  private:
		friend class WorldRoot;
		friend struct SpatialCollisionQueue;
		struct State;
		explicit SpatialCollisionLease(std::shared_ptr<State> Value);
		std::shared_ptr<State> Holder;
	};
}

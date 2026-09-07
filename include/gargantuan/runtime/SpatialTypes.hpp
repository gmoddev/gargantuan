#pragma once

#include "gargantuan/datatypes/CFrame.hpp"

#include <compare>
#include <cstdint>
#include <string>

namespace gargantuan {
	// SpatialSpaceId identifies one runtime-local coordinate space. Slot reuse is
	// generation checked; zero in either component is always invalid.
	struct SpatialSpaceId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] bool IsValid() const {
			return Slot != 0 && Generation != 0;
		}
		auto operator<=>(const SpatialSpaceId &) const = default;
	};

	inline constexpr SpatialSpaceId DefaultSpatialSpace{1, 1};

	// SpatialPose is semantic placement within a space. It is deliberately not
	// an acceleration-cell address and is never serialized by the spatial index.
	struct SpatialPose {
		SpatialSpaceId Space = DefaultSpatialSpace;
		CFrame LocalTransform;
	};

	struct SpatialCellCoordinate {
		std::int64_t X = 0;
		std::int64_t Y = 0;
		std::int64_t Z = 0;
		auto operator<=>(const SpatialCellCoordinate &) const = default;
	};

	// SpatialCellAddress is rebuildable acceleration metadata. It must not be
	// interpreted as semantic object identity or a persisted world location.
	struct SpatialCellAddress {
		SpatialSpaceId Space = DefaultSpatialSpace;
		SpatialCellCoordinate Cell;

		[[nodiscard]] bool IsValid() const {
			return Space.IsValid();
		}
		[[nodiscard]] std::uint64_t StableHash() const noexcept;
		[[nodiscard]] std::string ToString() const;
		auto operator<=>(const SpatialCellAddress &) const = default;
	};
}

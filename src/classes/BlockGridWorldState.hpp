#pragma once

#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <array>
#include <map>
#include <set>
#include <unordered_map>

namespace gargantuan {
	// Integer bounds stay in grid coordinates across rigid motion and world-index changes.
	using BlockGridBox = std::array<std::int32_t, 6>;
	using BlockGridLeafBoxes = std::map<blockgrid::Cell, std::vector<BlockGridBox>, blockgrid::CellOrder>;

	struct BlockGridCollider {
		ObjectId Grid;
		BlockGridBox Bounds;
	};

	struct BlockGridParticipant {
		std::shared_ptr<BlockGrid> Grid;
		std::map<BlockGridBox, PhysicsBodyId> Bodies;
		BlockGridLeafBoxes Desired;
		std::size_t DesiredCount{};
		bool TopologyCurrent{};
		bool TopologyActive{};
		std::optional<std::uint64_t> Revision;
		CFrame Pose;
		float CellSize{};
		bool Collidable{};
		bool Queryable{};
		bool Ready{};
	};

	struct BlockGridWorldState {
		std::map<ObjectId, BlockGridParticipant> Members;
		std::unordered_map<PhysicsBodyId, BlockGridCollider> Owners;
		std::set<PhysicsBodyId> Retiring;
		std::size_t ReservedBoxes{};
	};
}

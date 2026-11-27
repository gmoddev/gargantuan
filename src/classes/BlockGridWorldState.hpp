#pragma once

#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"

#include <array>
#include <map>
#include <set>
#include <unordered_map>

namespace gargantuan {
	// Integer bounds stay in grid coordinates across rigid motion and world-index changes.
	struct BlockGridBox {
		std::array<std::int32_t,6> Cells; // inclusive minimum and exclusive maximum of semantic owners
		std::uint8_t Participation{};
		blockgrid::Shape Geometry{blockgrid::Shape::Cube};
		std::uint8_t Orientation{}, Piece{};
		std::shared_ptr<const PhysicsTriangleMesh> Mesh;
		auto operator<=>(const BlockGridBox&) const = default;
	};
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
		std::uint8_t TopologyMask{};
		std::optional<std::uint64_t> Revision;
		std::uint64_t AssetRevision{};
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

#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace gargantuan {
	class Instance;
}

namespace gargantuan::network {
	inline constexpr std::size_t MaximumReplicationFocusPoints = 4;
	inline constexpr std::size_t MaximumReplicationSpatialObjects = 65'536;
	inline constexpr std::size_t MaximumReplicationSpatialCells = 65'536;
	inline constexpr std::size_t MaximumPeerDesiredObjects = 65'536;
	inline constexpr std::size_t MaximumReplicationQueryCells = 4'096;
	inline constexpr std::size_t MaximumReplicationRelevancePeers = 512;

	struct ReplicationRelevanceConfiguration {
		float CellSize = 128.0f;
		float EnterRadius = 256.0f;
		float LeaveRadius = 320.0f;
		std::uint64_t UpdateIntervalTicks = 6;
		std::size_t MaximumSpatialObjects = MaximumReplicationSpatialObjects;
		std::size_t MaximumSpatialCells = MaximumReplicationSpatialCells;
		std::size_t MaximumDesiredObjectsPerPeer = MaximumPeerDesiredObjects;
		std::size_t MaximumQueryCells = MaximumReplicationQueryCells;

		[[nodiscard]] bool IsValid() const;
	};

	struct PeerRelevanceSelection {
		std::vector<ObjectId> RequiredObjects;
		std::vector<ObjectId> DesiredObjects;
		bool operator==(const PeerRelevanceSelection &) const = default;
	};

	struct ReplicationRelevanceMetrics {
		std::uint64_t SpatialQueries = 0;
		std::uint64_t QueryCells = 0;
		std::uint64_t CandidateObjects = 0;
		std::uint64_t RelevanceEvaluations = 0;
		std::uint64_t SpatialEntries = 0;
		std::uint64_t SpatialMoves = 0;
		std::uint64_t SpatialRemovals = 0;
		std::uint64_t DesiredObjects = 0;
		std::uint64_t RelevanceEnters = 0;
		std::uint64_t RelevanceLeaves = 0;
		std::uint64_t LimitFailures = 0;
		std::uint64_t UpdateCpuNanoseconds = 0;
	};

	class ReplicationRelevance final {
	  public:
		using ExclusionPolicy = std::function<bool(ObjectId)>;

		ReplicationRelevance(
			std::shared_ptr<Instance> SourceRoot,
			ExclusionPolicy IsExcluded = {},
			ReplicationRelevanceConfiguration Configuration = {}
		);
		~ReplicationRelevance();
		ReplicationRelevance(const ReplicationRelevance &) = delete;
		ReplicationRelevance &operator=(const ReplicationRelevance &) = delete;

		bool AddPeer(ConnectionId Connection, ObjectId LocalPlayer, ObjectId OwnerCharacter = {});
		bool RemovePeer(ConnectionId Connection);
		bool SetOwnerCharacter(ConnectionId Connection, ObjectId Character);
		bool SetTrustedFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints);
		bool Update(std::uint64_t SimulationTick);

		[[nodiscard]] const PeerRelevanceSelection *GetSelection(ConnectionId Connection) const;
		[[nodiscard]] bool IsRuntimeRelevant(ConnectionId Connection, ObjectId Object) const;
		[[nodiscard]] bool WasSelectionEvaluated(ConnectionId Connection) const;
		[[nodiscard]] bool IsHealthy() const;
		[[nodiscard]] const std::string &GetFailure() const;
		[[nodiscard]] ReplicationRelevanceMetrics GetMetrics() const;

	  private:
		struct Implementation;
		std::unique_ptr<Implementation> State;
	};
}

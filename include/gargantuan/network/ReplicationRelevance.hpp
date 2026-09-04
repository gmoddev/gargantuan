#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/SpatialRegionIndex.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
	inline constexpr std::size_t MaximumReplicationSpatialRegions = 262'144;
	inline constexpr std::size_t MaximumReplicationSpatialMemberships = 262'144;
	inline constexpr std::size_t MaximumReplicationRegionsPerSpatialObject = 64;
	inline constexpr std::size_t MaximumReplicationLargeSpatialObjects = 1'024;
	inline constexpr std::size_t MaximumPeerDesiredObjects = 65'536;
	inline constexpr std::size_t MaximumReplicationQueryRegions = 4'096;
	inline constexpr std::size_t MaximumReplicationQueryMembershipVisits = 262'144;
	inline constexpr std::size_t MaximumReplicationRelevancePeers = 512;

	struct ReplicationRelevanceConfiguration {
		double RegionSize = DefaultSpatialRegionSize;
		float EnterRadius = 256.0f;
		float LeaveRadius = 320.0f;
		std::uint64_t UpdateIntervalTicks = 6;
		std::size_t MaximumSpatialObjects = MaximumReplicationSpatialObjects;
		std::size_t MaximumSpatialRegions = MaximumReplicationSpatialRegions;
		std::size_t MaximumSpatialMemberships = MaximumReplicationSpatialMemberships;
		std::size_t MaximumRegionsPerSpatialObject = MaximumReplicationRegionsPerSpatialObject;
		std::size_t MaximumLargeSpatialObjects = MaximumReplicationLargeSpatialObjects;
		std::size_t MaximumDesiredObjectsPerPeer = MaximumPeerDesiredObjects;
		std::size_t MaximumQueryRegions = MaximumReplicationQueryRegions;
		std::size_t MaximumQueryMembershipVisits = MaximumReplicationQueryMembershipVisits;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] SpatialRegionIndexConfiguration SpatialConfiguration() const;
	};

	struct PeerRelevanceSelection {
		std::vector<ObjectId> RequiredObjects;
		std::vector<ObjectId> DesiredObjects;
		bool operator==(const PeerRelevanceSelection &) const = default;
	};

	struct ReplicationRelevanceMetrics {
		std::uint64_t SpatialQueries = 0;
		std::uint64_t QueryRegions = 0;
		std::uint64_t CandidateObjects = 0;
		std::uint64_t CandidateMembershipVisits = 0;
		std::uint64_t CandidateDedupHits = 0;
		std::uint64_t RelevanceEvaluations = 0;
		std::uint64_t SpatialEntries = 0;
		std::uint64_t SpatialMemberships = 0;
		std::uint64_t SpatialRegions = 0;
		std::uint64_t LargeSpatialObjects = 0;
		std::uint64_t SpatialMoves = 0;
		std::uint64_t SameRegionUpdates = 0;
		std::uint64_t SpatialRemovals = 0;
		std::uint64_t RegionBucketsCreated = 0;
		std::uint64_t RegionBucketsRemoved = 0;
		std::uint64_t PeakRegionOccupancy = 0;
		std::uint64_t SpatialQueryLimitFailures = 0;
		std::uint64_t SpatialCandidateLimitFailures = 0;
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
		[[nodiscard]] std::span<const glm::vec3> GetResolvedFocus(ConnectionId Connection) const;
		[[nodiscard]] bool IsRuntimeRelevant(ConnectionId Connection, ObjectId Object) const;
		[[nodiscard]] bool WasSelectionEvaluated(ConnectionId Connection) const;
		[[nodiscard]] bool IsHealthy() const;
		[[nodiscard]] const std::string &GetFailure() const;
		[[nodiscard]] ReplicationRelevanceMetrics GetMetrics() const;
		[[nodiscard]] std::optional<SpatialAddress> GetSpatialAddress(ObjectId Object) const;
		[[nodiscard]] bool IsLargeSpatialObject(ObjectId Object) const;
		[[nodiscard]] bool VerifySpatialIndex() const;

	  private:
		struct Implementation;
		std::unique_ptr<Implementation> State;
	};
}

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/runtime/SpatialRegionIndex.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

namespace gargantuan::network {
	class GameSession;
	struct ReplicationMetrics;
}

namespace gargantuan::network::detail {
	struct FrozenCessationQuote {
		std::unique_ptr<ReplicationCoordinator> Replication;
		std::map<ConnectionId, std::uint64_t> AcceptedUnretiredCompleteBytes;
		std::map<ConnectionId, std::size_t> MaximumFrameBytes;
	};
	struct JournalRequirement {
		ChangeCursor Cursor;
		ConnectionId Connection;
		bool Catalog = false;
		bool PreparedCommit = false;
		std::uint64_t NameCoalescingBegin = 0;
		bool PendingRelevance = false;
	};

	enum class GameSessionFailurePoint : std::uint8_t {
		None,
		TransportStart,
		SchedulerRegistration,
		ReplicationPeerCreation,
		LocalPlayerResolution,
		RemoteManagerPeerCreation,
		PredictedCharacterPeerCreation,
		RuntimeCallbackAttachment,
		ClientGraphSynchronization,
		ClientReadySerialization,
		ClientReadySchedulerAdmission,
		StructuralSchedulerAdmission,
	};

	class GameSessionTestAccess final {
	  public:
		static void SetFailurePoint(GameSession &Session, GameSessionFailurePoint Point);
		static void RequestSpatialValidation(GameSession &Session);
		[[nodiscard]] static bool VerifySpatialIndex(const GameSession &Session);
		[[nodiscard]] static std::optional<SpatialCellAddress> GetSpatialCellAddress(const GameSession &Session, ObjectId Object);
		[[nodiscard]] static CharacterNetworkMetrics GetCharacterMetrics(const GameSession &Session);
		[[nodiscard]] static std::uint64_t GetReliableEventsAccepted(const GameSession &Session);
		[[nodiscard]] static RemoteMetrics GetRemoteMetrics(const GameSession &Session);
		[[nodiscard]] static std::vector<ConnectionId> GetConnections(const GameSession &Session);
		// Actual raw-history readers, not dependency/publication revision stamps.
		[[nodiscard]] static std::vector<JournalRequirement> GetJournalRequirements(const GameSession &Session);
		[[nodiscard]] static ReplicationMetrics GetReplicationMetrics(const GameSession &Session);
		// Main-only qualification capture. Accepted debt and the detached 3J
		// source are sampled without an intervening session step.
		[[nodiscard]] static std::optional<FrozenCessationQuote> CaptureFrozenCessationQuote(
			const GameSession &Session, std::string &Error);
	};
}

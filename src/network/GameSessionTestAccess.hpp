#pragma once

#include <cstdint>
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/runtime/SpatialRegionIndex.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

namespace gargantuan::network {
	class GameSession;
}

namespace gargantuan::network::detail {
	struct JournalRequirement {
		ChangeCursor Cursor;
		ConnectionId Connection;
		bool Catalog = false;
		bool PreparedCommit = false;
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
		[[nodiscard]] static std::vector<ConnectionId> GetConnections(const GameSession &Session);
		// Actual raw-history readers, not dependency/publication revision stamps.
		[[nodiscard]] static std::vector<JournalRequirement> GetJournalRequirements(const GameSession &Session);
	};
}

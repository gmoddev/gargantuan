#pragma once

#include <cstdint>

namespace gargantuan::network {
	class GameSession;
}

namespace gargantuan::network::detail {
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
	};
}

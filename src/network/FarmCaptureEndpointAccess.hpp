#pragma once

#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"

namespace gargantuan::network::detail {
	// Qualification-only read of the direct UDP peer bound to this generation.
	// No transport/wire or GameSession contract is changed by this accessor.
	struct FarmCaptureEndpointAccess {
		[[nodiscard]] static std::optional<TransportEndpoint> GetDirectRemoteEndpoint(
			const GameNetworkingSocketsTransport &Transport, ConnectionId Connection
		);
	};
}

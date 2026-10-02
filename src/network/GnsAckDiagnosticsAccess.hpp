#pragma once
#include "../../cmake/gns/AckDiagnostics.hpp"
#include "gargantuan/network/Transport.hpp"

namespace gargantuan::network {
class GameNetworkingSocketsTransport;
namespace detail {
// Private test/investigation surface. Reset starts a bounded capture; ordinary
// transport connections never enable this observer.
struct GnsAckDiagnosticsAccess {
	static bool Read(GameNetworkingSocketsTransport &Transport, ConnectionId Connection,
		GargantuanAckDiagnostics &Result, bool Reset = false);
	// Prototype policy is disabled by default and may change only while no
	// attributed grant is owned. Not exposed through the transport/public API.
	static bool PromptFinalGrantAck(GameNetworkingSocketsTransport &Transport, ConnectionId Connection, bool Enabled,
		std::uint64_t TailBudget = 0);
};
}
}

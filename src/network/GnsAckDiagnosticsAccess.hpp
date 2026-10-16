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
	// Private A/B override, only while no attributed grant is owned. Enabling
	// requires a funded tail budget; zero cannot enable the historical Q policy.
	static bool PromptFinalGrantAck(GameNetworkingSocketsTransport &Transport, ConnectionId Connection, bool Enabled,
		std::uint64_t TailBudget = 0);
	static bool FailNextFinalPacket(GameNetworkingSocketsTransport &Transport, ConnectionId Connection, bool AtSocket = false);
};
}
}

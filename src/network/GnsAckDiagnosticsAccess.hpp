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
};
}
}

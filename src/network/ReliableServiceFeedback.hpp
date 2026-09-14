#pragma once
#include "gargantuan/network/Connection.hpp"
#include <cstdint>
#include <optional>

namespace gargantuan::network {
class GameNetworkingSocketsTransport;
namespace detail {
// Private networking infrastructure. No raw handle, workload policy or rate claim.
struct ReliableServiceFeedback {
	ConnectionId Connection;
	std::uint64_t ObservedAtMicroseconds = 0;
	std::uint64_t UniqueReliableStreamBytesFirstSent = 0;
	std::uint64_t UniqueReliableStreamBytesAcked = 0;
	std::uint64_t ReliablePayloadBytesAcked = 0;
	std::uint64_t ReliableStreamBytesRetransmitted = 0;
	std::uint64_t PendingReliableStreamBytes = 0;
	std::uint64_t SentUnackedReliableStreamBytes = 0;
	ConnectionState State = ConnectionState::Connecting;
};
struct ReliableServiceFeedbackAccess {
	[[nodiscard]] static std::optional<ReliableServiceFeedback> Observe(
		const GameNetworkingSocketsTransport &Transport, ConnectionId Connection);
};
}
}

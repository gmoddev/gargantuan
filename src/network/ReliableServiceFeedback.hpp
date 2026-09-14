#pragma once
#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/network/Transport.hpp"
#include <cstdint>
#include <optional>

namespace gargantuan::network {
class GameNetworkingSocketsTransport;
class NetworkScheduler;
namespace detail {
	struct ReliableServiceAcceptedBytes {
		std::uint64_t All = 0, Structural = 0, Gameplay = 0;
	};
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
	std::uint64_t AttributedRetirementSequence = 0;
	std::uint64_t ActiveAttributedRetirementToken = 0;
	std::uint64_t ActiveAttributedMessageNumber = 0;
	std::uint64_t LastAttributedRetirementToken = 0;
	std::uint64_t LastAttributedRetirementMessageNumber = 0;
	std::uint64_t LastAttributedRetiredPayloadBytes = 0;
	bool CountersValid = true;
};
struct ReliableServiceFeedbackAccess {
	[[nodiscard]] static std::optional<ReliableServiceAcceptedBytes> Accepted(const NetworkScheduler &Scheduler, ConnectionId Connection);
	static bool Attribute(NetworkMessageIntent &Message, std::uint64_t Token) {
		if (!Token || Message.ReliableRetirementToken || Message.Delivery() != DeliveryMode::ReliableOrdered ||
			Message.Traffic() != TrafficClass::StructuralReplication) return false;
		Message.ReliableRetirementToken = Token;
		return true;
	}
	static std::uint64_t Token(const NetworkMessageIntent &Message) { return Message.ReliableRetirementToken; }
	static bool Enable(IGameTransport &Transport) { return Transport.EnableReliableServiceFeedback(); }
	static bool Release(IGameTransport &Transport, ConnectionId Connection) { return Transport.ReleaseReliableServiceFeedback(Connection); }
	[[nodiscard]] static std::optional<ReliableServiceFeedback> Observe(
		const IGameTransport &Transport, ConnectionId Connection) { return Transport.ReadReliableServiceFeedback(Connection); }
};
}
}

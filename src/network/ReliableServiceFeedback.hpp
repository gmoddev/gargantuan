#pragma once
#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/network/Transport.hpp"
#include <array>
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
	std::uint64_t StructuralPayloadBytesFirstSent = 0;
	std::uint64_t StructuralPayloadBytesAcked = 0;
	std::uint64_t StructuralQualifiedActiveMicroseconds = 0;
	std::uint64_t StructuralCurrentDeficitByteMicroseconds = 0;
	std::uint64_t StructuralMaximumDeficitByteMicroseconds = 0;
	std::uint64_t StructuralActiveGrantBytes = 0;
	std::uint64_t StructuralActiveGrantFirstSentBytes = 0;
	std::uint64_t StructuralActiveSinceMicroseconds = 0;
	std::uint64_t StructuralActiveGrantStartedAtMicroseconds = 0;
	bool StructuralServiceFailed = false;
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
	// F1 finite-grant evidence. The last completed record survives through
	// ACK/retirement so a slower Main-thread observer cannot invent a timeline.
	std::uint64_t StructuralGrantFirstSendAtMicroseconds = 0;
	std::uint64_t StructuralGrantCompletedAtMicroseconds = 0;
	std::uint64_t StructuralCompletedGrantSequence = 0;
	std::uint64_t StructuralLastCompletedGrantToken = 0;
	std::uint64_t StructuralLastCompletedGrantBytes = 0;
	std::uint64_t StructuralLastCompletedGrantActivatedAtMicroseconds = 0;
	std::uint64_t StructuralLastCompletedGrantFirstSendAtMicroseconds = 0;
	std::uint64_t StructuralLastCompletedGrantCompletedAtMicroseconds = 0;
	std::uint64_t StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds = 0;
	bool StructuralLastCompletedGrantFailed = false;
	std::uint64_t StructuralMaximumFiniteShortfallByteMicroseconds = 0;
	struct StructuralSegmentEvent {
		std::uint64_t AtMicroseconds = 0;
		std::uint64_t PayloadBytes = 0;
	};
	std::array<StructuralSegmentEvent, 512> LastCompletedStructuralSegmentEvents{};
	std::uint32_t LastCompletedStructuralSegmentEventCount = 0;
	std::uint64_t NativePacketsSent = 0;
	std::uint64_t NativePacketBytesSent = 0;
	std::uint64_t NativeMaximumPacketBytes = 0;
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
	static std::uint64_t ActivatedAt(const NetworkMessageIntent &Message) {
		return Message.ReliableGrantActivatedAtMicroseconds;
	}
	static bool Activate(NetworkMessageIntent &Message, std::uint64_t Token, std::uint64_t Time) {
		if (!Token || !Time || Message.ReliableRetirementToken != Token ||
			Message.ReliableGrantActivatedAtMicroseconds) return false;
		Message.ReliableGrantActivatedAtMicroseconds = Time;
		return true;
	}
	static bool Enable(IGameTransport &Transport) { return Transport.EnableReliableServiceFeedback(); }
	static bool Release(IGameTransport &Transport, ConnectionId Connection) { return Transport.ReleaseReliableServiceFeedback(Connection); }
	[[nodiscard]] static std::optional<ReliableServiceFeedback> Observe(
		const IGameTransport &Transport, ConnectionId Connection) { return Transport.ReadReliableServiceFeedback(Connection); }
};
}
}

#include "ReliableServiceFeedback.hpp"
#include <limits>

void GargantuanReliableServiceCounters::Add(std::uint64_t &Value, int Bytes) noexcept {
	if (Invalid) return;
	if (Bytes < 0 || static_cast<std::uint64_t>(Bytes) > std::numeric_limits<std::uint64_t>::max() - Value) {
		Invalid = true;
		return;
	}
	Value += static_cast<std::uint64_t>(Bytes);
}

void GargantuanReliableServiceCounters::FirstSend(int Bytes) noexcept {
	Add(UniqueReliableStreamBytesFirstSent, Bytes);
}

void GargantuanReliableServiceCounters::Retransmit(int Bytes) noexcept {
	Add(ReliableStreamBytesRetransmitted, Bytes);
}

void GargantuanReliableServiceCounters::AckSegment(int Bytes, bool AlreadyAcked) noexcept {
	if (Invalid || AlreadyAcked) return;
	const auto Before = UniqueReliableStreamBytesAcked;
	Add(UniqueReliableStreamBytesAcked, Bytes);
	if (!Invalid && UniqueReliableStreamBytesAcked > UniqueReliableStreamBytesFirstSent) {
		UniqueReliableStreamBytesAcked = Before;
		Invalid = true;
	}
}

void GargantuanReliableServiceCounters::AttributeMessage(
	std::uint64_t Token,
	std::int64_t MessageNumber
) noexcept {
	if (Invalid) return;
	if (!Token || MessageNumber <= 0 || ActiveAttributedRetirementToken ||
		Token <= LastAttributedRetirementToken ||
		AttributedRetirementSequence == std::numeric_limits<std::uint64_t>::max()) {
		Invalid = true;
		return;
	}
	ActiveAttributedRetirementToken = Token;
	ActiveAttributedMessageNumber = static_cast<std::uint64_t>(MessageNumber);
}

void GargantuanReliableServiceCounters::AckMessage(
	std::int64_t MessageNumber,
	int MessageBytes,
	int PrivateHeaderBytes
) noexcept {
	if (Invalid) return;
	if (MessageBytes < 0 || PrivateHeaderBytes < 0 || PrivateHeaderBytes > MessageBytes) {
		Invalid = true;
		return;
	}
	const int PayloadBytes = MessageBytes - PrivateHeaderBytes;
	Add(ReliablePayloadBytesAcked, PayloadBytes);
	if (Invalid || !ActiveAttributedRetirementToken) return;
	if (MessageNumber <= 0) {
		Invalid = true;
		return;
	}
	if (static_cast<std::uint64_t>(MessageNumber) != ActiveAttributedMessageNumber) return;
	if (AttributedRetirementSequence == std::numeric_limits<std::uint64_t>::max()) {
		Invalid = true;
		return;
	}
	++AttributedRetirementSequence;
	LastAttributedRetirementToken = ActiveAttributedRetirementToken;
	LastAttributedRetirementMessageNumber = ActiveAttributedMessageNumber;
	LastAttributedRetiredPayloadBytes = static_cast<std::uint64_t>(PayloadBytes);
	ActiveAttributedRetirementToken = 0;
	ActiveAttributedMessageNumber = 0;
}

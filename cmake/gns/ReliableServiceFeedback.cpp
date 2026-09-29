#include "ReliableServiceFeedback.hpp"
#include <chrono>
#include <limits>

namespace {
constexpr std::uint64_t StructuralRate = 16'777'216;
constexpr std::uint64_t StructuralDeficitBound = 1'248'000'000 + StructuralRate * 6'000;

std::uint64_t ServiceClock() noexcept {
	const auto Time = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return Time > 0 ? static_cast<std::uint64_t>(Time) : 0;
}
}

void GargantuanReliableServiceCounters::Add(std::uint64_t &Value, int Bytes) noexcept {
	if (Invalid) return;
	if (Bytes < 0 || static_cast<std::uint64_t>(Bytes) > std::numeric_limits<std::uint64_t>::max() - Value) {
		Invalid = true;
		return;
	}
	Value += static_cast<std::uint64_t>(Bytes);
}

void GargantuanReliableServiceCounters::ObserveActiveService(std::uint64_t NowMicroseconds) noexcept {
	if (Invalid || !StructuralActiveSinceMicroseconds) return;
	if (NowMicroseconds < StructuralActiveSinceMicroseconds) { Invalid = true; return; }
	const auto Elapsed = NowMicroseconds - StructuralActiveSinceMicroseconds;
	if (Elapsed > std::numeric_limits<std::uint64_t>::max() / StructuralRate ||
		Elapsed > std::numeric_limits<std::uint64_t>::max() - StructuralQualifiedActiveMicroseconds) {
		Invalid = true; return;
	}
	const auto Added = Elapsed * StructuralRate;
	if (Added > std::numeric_limits<std::uint64_t>::max() - StructuralCurrentDeficitByteMicroseconds) {
		Invalid = true; return;
	}
	StructuralQualifiedActiveMicroseconds += Elapsed;
	StructuralCurrentDeficitByteMicroseconds += Added;
	if (StructuralCurrentDeficitByteMicroseconds > StructuralMaximumDeficitByteMicroseconds)
		StructuralMaximumDeficitByteMicroseconds = StructuralCurrentDeficitByteMicroseconds;
	if (StructuralCurrentDeficitByteMicroseconds > StructuralDeficitBound) StructuralServiceFailed = true;
	StructuralActiveSinceMicroseconds = NowMicroseconds;
}

void GargantuanReliableServiceCounters::FirstSend(int Bytes, int StructuralPayloadBytes,
	std::uint64_t NowMicroseconds) noexcept {
	if (StructuralPayloadBytes < 0 || StructuralPayloadBytes > Bytes) { Invalid = true; return; }
	if (StructuralPayloadBytes) {
		if (!StructuralActiveGrantBytes ||
			StructuralPayloadBytes > StructuralActiveGrantBytes - StructuralActiveGrantFirstSentBytes) {
			Invalid = true; return;
		}
		if (StructuralActiveSinceMicroseconds)
			ObserveActiveService(NowMicroseconds ? NowMicroseconds : ServiceClock());
		if (Invalid) return;
		if (StructuralActiveSinceMicroseconds) {
			const auto Served = static_cast<std::uint64_t>(StructuralPayloadBytes) * 1'000'000;
			StructuralCurrentDeficitByteMicroseconds = Served >= StructuralCurrentDeficitByteMicroseconds
				? 0 : StructuralCurrentDeficitByteMicroseconds - Served;
		}
		StructuralActiveGrantFirstSentBytes += static_cast<std::uint64_t>(StructuralPayloadBytes);
		if (StructuralActiveGrantFirstSentBytes == StructuralActiveGrantBytes) {
			StructuralActiveSinceMicroseconds = 0;
			StructuralActiveGrantStartedAtMicroseconds = 0;
			StructuralActiveGrantBytes = 0;
			StructuralActiveGrantFirstSentBytes = 0;
		}
	}
	Add(UniqueReliableStreamBytesFirstSent, Bytes);
	Add(StructuralPayloadBytesFirstSent, StructuralPayloadBytes);
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
	std::int64_t MessageNumber,
	int PayloadBytes,
	std::uint64_t ActivatedAtMicroseconds
) noexcept {
	if (Invalid) return;
	if (!Token || MessageNumber <= 0 || ActiveAttributedRetirementToken ||
		PayloadBytes < 0 || StructuralActiveSinceMicroseconds ||
		Token <= LastAttributedRetirementToken ||
		AttributedRetirementSequence == std::numeric_limits<std::uint64_t>::max()) {
		Invalid = true;
		return;
	}
	ActiveAttributedRetirementToken = Token;
	ActiveAttributedMessageNumber = static_cast<std::uint64_t>(MessageNumber);
	ActiveAttributedPayloadBytes = static_cast<std::uint64_t>(PayloadBytes);
	StructuralActiveGrantBytes = static_cast<std::uint64_t>(PayloadBytes);
	StructuralActiveGrantFirstSentBytes = 0;
	StructuralActiveSinceMicroseconds = PayloadBytes &&
		ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max()
		? ActivatedAtMicroseconds : 0;
	StructuralActiveGrantStartedAtMicroseconds = StructuralActiveSinceMicroseconds;
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
	if (ActiveAttributedPayloadBytes) {
		if (static_cast<std::uint64_t>(PayloadBytes) != ActiveAttributedPayloadBytes) {
			Invalid = true; return;
		}
		Add(StructuralPayloadBytesAcked, PayloadBytes);
		if (Invalid || StructuralPayloadBytesAcked > StructuralPayloadBytesFirstSent) { Invalid = true; return; }
	}
	LastAttributedRetirementToken = ActiveAttributedRetirementToken;
	LastAttributedRetirementMessageNumber = ActiveAttributedMessageNumber;
	LastAttributedRetiredPayloadBytes = static_cast<std::uint64_t>(PayloadBytes);
	ActiveAttributedRetirementToken = 0;
	ActiveAttributedMessageNumber = 0;
	ActiveAttributedPayloadBytes = 0;
}

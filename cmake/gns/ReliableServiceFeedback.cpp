#include "ReliableServiceFeedback.hpp"
#include <chrono>
#include <limits>

namespace {
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

void GargantuanReliableServiceCounters::SyncStructuralGrant() noexcept {
	if (Invalid) return;
	const auto &Grant = StructuralGrantCurve;
	if (Grant.Invalid || Grant.QualifiedRunningMicroseconds < StructuralGrantLastRunningMicroseconds ||
		Grant.QualifiedRunningMicroseconds - StructuralGrantLastRunningMicroseconds >
			std::numeric_limits<std::uint64_t>::max() - StructuralQualifiedActiveMicroseconds) {
		Invalid = true; return;
	}
	StructuralQualifiedActiveMicroseconds += Grant.QualifiedRunningMicroseconds - StructuralGrantLastRunningMicroseconds;
	StructuralGrantLastRunningMicroseconds = Grant.QualifiedRunningMicroseconds;
	StructuralCurrentDeficitByteMicroseconds = Grant.CurrentRunningDeficitByteMicroseconds;
	if (Grant.MaximumRunningDeficitByteMicroseconds > StructuralMaximumDeficitByteMicroseconds)
		StructuralMaximumDeficitByteMicroseconds = Grant.MaximumRunningDeficitByteMicroseconds;
	if (Grant.MaximumFiniteShortfallByteMicroseconds > StructuralMaximumFiniteShortfallByteMicroseconds)
		StructuralMaximumFiniteShortfallByteMicroseconds = Grant.MaximumFiniteShortfallByteMicroseconds;
	StructuralServiceFailed = Grant.ServiceFailed;
	StructuralGrantFirstSendAtMicroseconds = Grant.FirstSentAtMicroseconds;
	StructuralGrantCompletedAtMicroseconds = Grant.CompletedAtMicroseconds;
	StructuralActiveGrantFirstSentBytes = Grant.FirstSentBytes;
	StructuralActiveSinceMicroseconds = Grant.FirstSentAtMicroseconds && !Grant.CompletedAtMicroseconds
		? Grant.FirstSentAtMicroseconds : 0;
	if (Grant.CompletedAtMicroseconds && StructuralActiveGrantBytes) {
		if (StructuralCompletedGrantSequence == std::numeric_limits<std::uint64_t>::max()) {
			Invalid = true; return;
		}
		++StructuralCompletedGrantSequence;
		StructuralLastCompletedGrantToken = Grant.ActiveToken;
		StructuralLastCompletedGrantBytes = Grant.GrantBytes;
		StructuralLastCompletedGrantActivatedAtMicroseconds = Grant.ActivatedAtMicroseconds;
		StructuralLastCompletedGrantFirstSendAtMicroseconds = Grant.FirstSentAtMicroseconds;
		StructuralLastCompletedGrantCompletedAtMicroseconds = Grant.CompletedAtMicroseconds;
		StructuralLastCompletedGrantMaximumRunningDeficitByteMicroseconds = Grant.MaximumRunningDeficitByteMicroseconds;
		StructuralLastCompletedGrantFailed = Grant.GrantFailed;
		StructuralActiveGrantStartedAtMicroseconds = 0;
		StructuralActiveGrantBytes = 0;
		StructuralActiveGrantFirstSentBytes = 0;
		StructuralActiveSinceMicroseconds = 0;
		StructuralCurrentDeficitByteMicroseconds = 0;
	}
}

void GargantuanReliableServiceCounters::ObserveActiveService(std::uint64_t NowMicroseconds) noexcept {
	if (Invalid || !StructuralActiveGrantBytes ||
		StructuralGrantCurve.ActiveToken != ActiveAttributedRetirementToken) return;
	(void)StructuralGrantCurve.Observe(NowMicroseconds);
	SyncStructuralGrant();
}

void GargantuanReliableServiceCounters::FirstSend(int Bytes, int StructuralPayloadBytes,
	std::uint64_t NowMicroseconds) noexcept {
	if (StructuralPayloadBytes < 0 || StructuralPayloadBytes > Bytes) { Invalid = true; return; }
	if (StructuralPayloadBytes) {
		if (!StructuralActiveGrantBytes ||
			StructuralPayloadBytes > StructuralActiveGrantBytes - StructuralActiveGrantFirstSentBytes) {
			Invalid = true; return;
		}
		if (StructuralGrantCurve.ActiveToken == ActiveAttributedRetirementToken) {
			(void)StructuralGrantCurve.FirstSend(ActiveAttributedRetirementToken,
				static_cast<std::uint64_t>(StructuralPayloadBytes),
				NowMicroseconds ? NowMicroseconds : ServiceClock());
			SyncStructuralGrant();
		} else {
			StructuralActiveGrantFirstSentBytes += static_cast<std::uint64_t>(StructuralPayloadBytes);
		}
		if (Invalid) return;
		if (StructuralActiveGrantBytes && StructuralActiveGrantFirstSentBytes == StructuralActiveGrantBytes) {
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
	StructuralActiveSinceMicroseconds = 0;
	StructuralGrantFirstSendAtMicroseconds = 0;
	StructuralGrantCompletedAtMicroseconds = 0;
	StructuralCurrentDeficitByteMicroseconds = 0;
	StructuralActiveGrantStartedAtMicroseconds = PayloadBytes && ActivatedAtMicroseconds &&
		ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max()
		? ActivatedAtMicroseconds : 0;
	if (StructuralActiveGrantStartedAtMicroseconds) {
		StructuralGrantLastRunningMicroseconds = 0;
		if (!StructuralGrantCurve.Activate(Token, static_cast<std::uint64_t>(PayloadBytes),
			StructuralActiveGrantStartedAtMicroseconds)) Invalid = true;
		else StructuralServiceFailed = false;
	}
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

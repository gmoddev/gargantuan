#include "ReliableServiceFeedback.hpp"
#include "ServiceTimingDiagnostics.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

namespace {
std::atomic<unsigned> RunningStructuralGrants{0};
std::uint64_t ServiceClock() noexcept {
	const auto Time = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return Time > 0 ? static_cast<std::uint64_t>(Time) : 0;
}
}

namespace SteamNetworkingSocketsLib {
std::uint64_t GargantuanReliableServiceClock() noexcept {
	return ServiceClock();
}

void GargantuanSetRunningStructuralGrant(bool &SenderRunning, bool Running) noexcept {
	if (SenderRunning == Running) return;
	SenderRunning = Running;
	if (Running) RunningStructuralGrants.fetch_add(1, std::memory_order_release);
	else RunningStructuralGrants.fetch_sub(1, std::memory_order_release);
}

bool GargantuanHasRunningStructuralGrant() noexcept {
	return RunningStructuralGrants.load(std::memory_order_acquire) != 0;
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

void GargantuanReliableServiceCounters::SyncCompletionGrant() noexcept {
	StructuralCompletionEvidenceInvalid = StructuralCompletionCurve.Invalid;
	StructuralCompletionServiceFailed = StructuralCompletionCurve.GrantFailed;
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
		StructuralLastCompletedCompletionFailed = StructuralCompletionCurve.GrantFailed;
		LastCompletedStructuralSegmentEvents = StructuralSegmentEvents;
		LastCompletedStructuralSegmentEventCount = StructuralSegmentEventCount;
		LastCompletedInitialTrace = InitialTrace;
		StructuralActiveGrantStartedAtMicroseconds = 0;
		StructuralActiveGrantBytes = 0;
		StructuralActiveGrantFirstSentBytes = 0;
		StructuralActiveSinceMicroseconds = 0;
		StructuralCurrentDeficitByteMicroseconds = 0;
	}
}

void GargantuanReliableServiceCounters::NativePacket(int Bytes) noexcept {
	// Physical exposure remains physical after a fatal service/ownership fault.
	// Keep counting it without unfreezing any unique BODY or ACK certificate.
	if (Bytes <= 0 || NativePacketsSent == std::numeric_limits<std::uint64_t>::max() ||
		static_cast<std::uint64_t>(Bytes) > std::numeric_limits<std::uint64_t>::max() - NativePacketBytesSent) {
		Invalid = true; return;
	}
	++NativePacketsSent;
	NativePacketBytesSent += static_cast<std::uint64_t>(Bytes);
	if (static_cast<std::uint64_t>(Bytes) > NativeMaximumPacketBytes)
		NativeMaximumPacketBytes = static_cast<std::uint64_t>(Bytes);
}

void GargantuanReliableServiceCounters::ObserveActiveService(std::uint64_t NowMicroseconds) noexcept {
	if (Invalid || !StructuralActiveGrantBytes ||
		StructuralGrantCurve.ActiveToken != ActiveAttributedRetirementToken) return;
	(void)StructuralGrantCurve.Observe(NowMicroseconds);
	(void)StructuralCompletionCurve.Observe(NowMicroseconds);
	SyncCompletionGrant();
	SyncStructuralGrant();
}

void GargantuanReliableServiceCounters::FirstSend(int Bytes, int StructuralPayloadBytes,
	std::uint64_t NowMicroseconds, int OrdinaryPayloadBytes) noexcept {
	if (StructuralPayloadBytes < 0 || OrdinaryPayloadBytes < 0 || StructuralPayloadBytes > Bytes ||
		OrdinaryPayloadBytes > Bytes - StructuralPayloadBytes) { Invalid = true; return; }
	if (StructuralPayloadBytes) {
		if (!StructuralActiveGrantBytes ||
			StructuralPayloadBytes > StructuralActiveGrantBytes - StructuralActiveGrantFirstSentBytes) {
			Invalid = true; return;
		}
		if (StructuralGrantCurve.ActiveToken == ActiveAttributedRetirementToken) {
			const auto EventTime = NowMicroseconds ? NowMicroseconds : ServiceClock();
			if (StructuralSegmentEventCount < MaximumStructuralSegmentEvents)
				StructuralSegmentEvents[StructuralSegmentEventCount++] =
					{EventTime, static_cast<std::uint64_t>(StructuralPayloadBytes)};
			// Observer only: freeze before a late initial packet's remaining grant
			// drains. Neither callback state nor result changes the F1 evaluator.
			if (!Invalid && !StructuralGrantCurve.Invalid && !StructuralGrantCurve.HasFirstSend &&
				EventTime >= StructuralGrantCurve.LastObservedAtMicroseconds &&
				gargantuan::network::FiniteGrantServiceCurve::ZeroFirstSendBreach(
					StructuralGrantCurve.ActivatedAtMicroseconds, EventTime)) {
				auto *Observer = SteamNetworkingSocketsLib::GargantuanServiceTimingObserver.load(std::memory_order_acquire);
				if (Observer && Observer->InitialBreach)
					Observer->InitialBreach(Observer->Context, ActiveAttributedRetirementToken,
						StructuralGrantCurve.ActivatedAtMicroseconds, EventTime);
			}
			(void)StructuralGrantCurve.FirstSend(ActiveAttributedRetirementToken,
				static_cast<std::uint64_t>(StructuralPayloadBytes),
				EventTime);
			(void)StructuralCompletionCurve.FirstSend(ActiveAttributedRetirementToken,
				static_cast<std::uint64_t>(StructuralPayloadBytes), EventTime);
			SyncCompletionGrant();
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
	Add(OrdinaryPayloadBytesFirstSent, OrdinaryPayloadBytes);
}

void GargantuanReliableServiceCounters::RegisterQualifiedOrdinary(std::int64_t MessageNumber,
	int PayloadBytes, std::uint64_t Sequence, std::uint64_t Phase, std::uint64_t AcceptedAtMicroseconds) noexcept {
	if (!Sequence) return; // Unclassified traffic is not a qualification exemption.
	if (MessageNumber <= 0 || PayloadBytes <= 0 || !AcceptedAtMicroseconds ||
		QualifiedOrdinaryRegisteredMessages == UINT64_MAX) { QualifiedOrdinaryEvidenceInvalid = true; return; }
	QualifiedOrdinaryPending *Free = nullptr;
	for (auto &Value : QualifiedOrdinaryPendingMessagesStorage) {
		if (Value.MessageNumber == static_cast<std::uint64_t>(MessageNumber) ||
			(Value.MessageNumber && Value.Sequence == Sequence && Value.Phase == Phase)) {
			QualifiedOrdinaryEvidenceInvalid = true; return;
		}
		if (!Value.MessageNumber && !Free) Free = &Value;
	}
	if (!Free) { QualifiedOrdinaryEvidenceInvalid = true; return; }
	*Free = {static_cast<std::uint64_t>(MessageNumber), Sequence, Phase, AcceptedAtMicroseconds,
		static_cast<std::uint32_t>(PayloadBytes), 0};
	++QualifiedOrdinaryRegisteredMessages; ++QualifiedOrdinaryPendingMessages;
}

void GargantuanReliableServiceCounters::LatchQualifiedOrdinaryFailure(const QualifiedOrdinaryPending &Message,
	std::uint64_t CompletedAtMicroseconds) noexcept {
	QualifiedOrdinaryEverFailed = true;
	if (QualifiedOrdinaryFailureSequence) return;
	QualifiedOrdinaryFailureSequence = Message.Sequence; QualifiedOrdinaryFailurePhase = Message.Phase;
	QualifiedOrdinaryFailureMessageNumber = Message.MessageNumber;
	QualifiedOrdinaryFailureAcceptedAtMicroseconds = Message.AcceptedAtMicroseconds;
	QualifiedOrdinaryFailureCompletedAtMicroseconds = CompletedAtMicroseconds;
	QualifiedOrdinaryFailureExpectedBytes = Message.ExpectedBytes;
	QualifiedOrdinaryFailureFirstSentBytes = Message.FirstSentBytes;
}

void GargantuanReliableServiceCounters::FirstSendQualifiedOrdinary(std::int64_t MessageNumber,
	int PayloadBytes, std::uint64_t CompletedAtMicroseconds) noexcept {
	if (Invalid || Purged || PayloadBytes < 0) { QualifiedOrdinaryEvidenceInvalid = true; return; }
	if (PayloadBytes == 0) return;
	if (!QualifiedOrdinaryPendingMessages) return;
	for (auto &Value : QualifiedOrdinaryPendingMessagesStorage) {
		if (Value.MessageNumber != static_cast<std::uint64_t>(MessageNumber)) continue;
		if (!CompletedAtMicroseconds || CompletedAtMicroseconds < Value.AcceptedAtMicroseconds ||
			static_cast<std::uint32_t>(PayloadBytes) > Value.ExpectedBytes - Value.FirstSentBytes) {
			QualifiedOrdinaryEvidenceInvalid = true; return;
		}
		Value.FirstSentBytes += static_cast<std::uint32_t>(PayloadBytes);
		const auto Latency = CompletedAtMicroseconds - Value.AcceptedAtMicroseconds;
		if (Latency > QualifiedOrdinaryDeadlineMicroseconds)
			LatchQualifiedOrdinaryFailure(Value,
				Value.FirstSentBytes == Value.ExpectedBytes ? CompletedAtMicroseconds : 0);
		if (Value.FirstSentBytes == Value.ExpectedBytes) {
			if (!QualifiedOrdinaryPendingMessages || QualifiedOrdinaryCompletedMessages == UINT64_MAX) {
				QualifiedOrdinaryEvidenceInvalid = true; return;
			}
			QualifiedOrdinaryMaximumLatencyMicroseconds = std::max(QualifiedOrdinaryMaximumLatencyMicroseconds, Latency);
			QualifiedOrdinaryLastCompletedFailed = Latency > QualifiedOrdinaryDeadlineMicroseconds;
			++QualifiedOrdinaryCompletedMessages; --QualifiedOrdinaryPendingMessages; Value = {};
		}
		return;
	}
}

void GargantuanReliableServiceCounters::ObserveQualifiedOrdinary(std::uint64_t ResolvedThroughMicroseconds) noexcept {
	QualifiedOrdinaryActiveDeadlineFailed = false;
	if (!QualifiedOrdinaryPendingMessages) return;
	for (const auto &Value : QualifiedOrdinaryPendingMessagesStorage) {
		if (!Value.MessageNumber) continue;
		if (ResolvedThroughMicroseconds < Value.AcceptedAtMicroseconds) { QualifiedOrdinaryEvidenceInvalid = true; continue; }
		if (ResolvedThroughMicroseconds - Value.AcceptedAtMicroseconds > QualifiedOrdinaryDeadlineMicroseconds) {
			QualifiedOrdinaryActiveDeadlineFailed = true; LatchQualifiedOrdinaryFailure(Value, 0);
		}
	}
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
	StructuralSegmentEventCount = 0;
	InitialTrace = {};
	StructuralCurrentDeficitByteMicroseconds = 0;
	StructuralActiveGrantStartedAtMicroseconds = PayloadBytes && ActivatedAtMicroseconds &&
		ActivatedAtMicroseconds != std::numeric_limits<std::uint64_t>::max()
		? ActivatedAtMicroseconds : 0;
	if (StructuralActiveGrantStartedAtMicroseconds) {
		StructuralGrantLastRunningMicroseconds = 0;
		(void)StructuralCompletionCurve.Activate(Token, static_cast<std::uint64_t>(PayloadBytes),
			StructuralActiveGrantStartedAtMicroseconds);
		SyncCompletionGrant();
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

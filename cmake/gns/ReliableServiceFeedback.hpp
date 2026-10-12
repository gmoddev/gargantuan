#pragma once
#include "../../src/network/FiniteGrantServiceCurve.hpp"
#include "../../src/network/InitialServiceTrace.hpp"
#include <array>
#include <cstdint>

class ISteamNetworkingSockets;

struct GargantuanReliableServiceCounters {
	struct StructuralSegmentEvent {
		std::uint64_t AtMicroseconds = 0;
		std::uint64_t PayloadBytes = 0;
	};
	static constexpr std::size_t MaximumStructuralSegmentEvents = 512;
	std::array<StructuralSegmentEvent, MaximumStructuralSegmentEvents> StructuralSegmentEvents{};
	std::array<StructuralSegmentEvent, MaximumStructuralSegmentEvents> LastCompletedStructuralSegmentEvents{};
	std::uint32_t StructuralSegmentEventCount = 0;
	std::uint32_t LastCompletedStructuralSegmentEventCount = 0;
	gargantuan::network::detail::InitialServiceTrace InitialTrace{}, LastCompletedInitialTrace{};
	gargantuan::network::FiniteGrantServiceCurve StructuralGrantCurve;
	gargantuan::network::NativeGrantCompletionCurve StructuralCompletionCurve;
	bool StructuralCompletionServiceFailed = false;
	bool StructuralLastCompletedCompletionFailed = false;
	bool StructuralCompletionEvidenceInvalid = false;
	std::uint64_t UniqueReliableStreamBytesFirstSent = 0;
	// Positive UDP data sends, including GNS framing and encrypted payload.
	// IPv4/UDP headers can be added separately when proving wire reserve.
	std::uint64_t NativePacketsSent = 0;
	std::uint64_t NativePacketBytesSent = 0;
	std::uint64_t NativeMaximumPacketBytes = 0;
	std::uint64_t UniqueReliableStreamBytesAcked = 0;
	// Payload bytes belonging to the one native-attributed structural grant.
	// Reliable stream framing and ordinary reliable traffic are excluded.
	std::uint64_t StructuralPayloadBytesFirstSent = 0;
	// All ordinary reliable BODY bytes, including the adapter envelope but
	// excluding private GNS headers. Independent of optional H2 qualification.
	std::uint64_t OrdinaryPayloadBytesFirstSent = 0;
	std::uint64_t StructuralPayloadBytesAcked = 0;
	// F1 running time and deficit are charged within each finite accepted grant.
	// The maximum is the maximum of independent per-grant observations, never
	// a persistent deficit carried into the next grant.
	std::uint64_t StructuralQualifiedActiveMicroseconds = 0;
	std::uint64_t StructuralMaximumDeficitByteMicroseconds = 0;
	std::uint64_t StructuralCurrentDeficitByteMicroseconds = 0;
	std::uint64_t StructuralMaximumFiniteShortfallByteMicroseconds = 0;
	std::uint64_t StructuralActiveGrantBytes = 0;
	std::uint64_t StructuralActiveGrantFirstSentBytes = 0;
	std::uint64_t StructuralActiveSinceMicroseconds = 0;
	std::uint64_t StructuralActiveGrantStartedAtMicroseconds = 0;
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
	std::uint64_t StructuralGrantLastRunningMicroseconds = 0;
	bool StructuralServiceFailed = false;
	std::uint64_t ReliablePayloadBytesAcked = 0;
	std::uint64_t ReliableStreamBytesRetransmitted = 0;
	// Optional sender-local attribution for one bounded reliable obligation.
	// The token is never transmitted. The GNS message number is the stable
	// native identity retained through segment retry and final message release.
	std::uint64_t AttributedRetirementSequence = 0;
	std::uint64_t ActiveAttributedRetirementToken = 0;
	std::uint64_t ActiveAttributedMessageNumber = 0;
	std::uint64_t ActiveAttributedPayloadBytes = 0;
	std::uint64_t LastAttributedRetirementToken = 0;
	std::uint64_t LastAttributedRetirementMessageNumber = 0;
	std::uint64_t LastAttributedRetiredPayloadBytes = 0;
	bool Invalid = false;
	bool Purged = false;
	bool ProtectedOrdinaryEverFailed = false, ProtectedOrdinaryActiveDeadlineFailed = false;
	bool ProtectedOrdinaryEvidenceInvalid = false;
	struct QualifiedOrdinaryPending {
		std::uint64_t MessageNumber = 0, Sequence = 0, Phase = 0, AcceptedAtMicroseconds = 0;
		std::uint32_t ExpectedBytes = 0, FirstSentBytes = 0;
	};
	// One unresolved native packet can retain a timely-completed cohort whose
	// acceptance timestamps occupy its preceding 50-ms window. A second cohort
	// awaiting first-send occupies the current 50-ms window. Each is bounded by
	// ceil(16-message burst +64/s *50ms)=20 under the qualified arrival envelope.
	// No actor reconciliation or Main-opportunity bound is inferred here.
	// Metadata only; overflow invalidates the explicitly qualified observation,
	// never rejects legal messages or transfers their reliability ownership.
	static constexpr std::size_t MaximumQualifiedOrdinaryPending = 40;
	static constexpr std::uint64_t QualifiedOrdinaryDeadlineMicroseconds = 50'000;
	std::array<QualifiedOrdinaryPending, MaximumQualifiedOrdinaryPending> QualifiedOrdinaryPendingMessagesStorage{};
	std::uint64_t QualifiedOrdinaryRegisteredMessages = 0, QualifiedOrdinaryCompletedMessages = 0;
	std::uint64_t QualifiedOrdinaryPendingMessages = 0, QualifiedOrdinaryMaximumLatencyMicroseconds = 0;
	bool QualifiedOrdinaryEverFailed = false, QualifiedOrdinaryLastCompletedFailed = false;
	bool QualifiedOrdinaryActiveDeadlineFailed = false, QualifiedOrdinaryEvidenceInvalid = false;
	std::uint64_t QualifiedOrdinaryFailureSequence = 0, QualifiedOrdinaryFailurePhase = 0;
	std::uint64_t QualifiedOrdinaryFailureMessageNumber = 0, QualifiedOrdinaryFailureAcceptedAtMicroseconds = 0;
	std::uint64_t QualifiedOrdinaryFailureCompletedAtMicroseconds = 0;
	std::uint32_t QualifiedOrdinaryFailureExpectedBytes = 0, QualifiedOrdinaryFailureFirstSentBytes = 0;

	void FirstSend(int Bytes, int StructuralPayloadBytes = 0, std::uint64_t NowMicroseconds = 0,
		int OrdinaryPayloadBytes = 0) noexcept;
	void RegisterQualifiedOrdinary(std::int64_t MessageNumber, int PayloadBytes,
		std::uint64_t Sequence, std::uint64_t Phase, std::uint64_t AcceptedAtMicroseconds) noexcept;
	void FirstSendQualifiedOrdinary(std::int64_t MessageNumber, int PayloadBytes,
		std::uint64_t CompletedAtMicroseconds) noexcept;
	void ObserveQualifiedOrdinary(std::uint64_t ResolvedThroughMicroseconds) noexcept;
	void NativePacket(int Bytes) noexcept;
	void Retransmit(int Bytes) noexcept;
	void AckSegment(int Bytes, bool AlreadyAcked) noexcept;
	void AttributeMessage(std::uint64_t Token, std::int64_t MessageNumber,
		int PayloadBytes = 0, std::uint64_t ActivatedAtMicroseconds = 0) noexcept;
	void ObserveActiveService(std::uint64_t NowMicroseconds) noexcept;
	int SenderPacketQuantum(bool OtherStructuralRunning) const noexcept {
		// Retain the existing structural quantum, including its startup FIFO.
		if (StructuralActiveGrantBytes > StructuralActiveGrantFirstSentBytes) return 4;
		// Tokens are private POOLED ownership, never observer state or wire data.
		// FULL and default clients remain unmarked and keep the original path.
		if (OtherStructuralRunning && (ActiveAttributedRetirementToken || LastAttributedRetirementToken)) return 1;
		return 0;
	}
	void AckMessage(std::int64_t MessageNumber, int MessageBytes, int PrivateHeaderBytes) noexcept;
private:
	void Add(std::uint64_t &Value, int Bytes) noexcept;
	void SyncStructuralGrant() noexcept;
	void SyncCompletionGrant() noexcept;
	void LatchQualifiedOrdinaryFailure(const QualifiedOrdinaryPending &Message,
		std::uint64_t CompletedAtMicroseconds) noexcept;
};

struct GargantuanReliableServiceSnapshot {
	GargantuanReliableServiceCounters Counters;
	std::uint64_t ObservedAtMicroseconds = 0;
	std::uint64_t PendingReliableStreamBytes = 0;
	std::uint64_t SentUnackedReliableStreamBytes = 0;
	int NativeState = 0;
};

namespace SteamNetworkingSocketsLib {
class CSteamNetworkConnectionBase;
bool GargantuanRegisterNativePending(CSteamNetworkConnectionBase *Connection) noexcept;
void GargantuanUnregisterNativePending(CSteamNetworkConnectionBase *Connection) noexcept;
std::uint64_t GargantuanReliableServiceClock() noexcept;
// Socket-thread wake policy only needs to know whether any connection has a
// partly first-sent finite structural grant. The sender owns the local bool;
// this query never shares a connection pointer across threads.
void GargantuanSetRunningStructuralGrant(bool &SenderRunning, bool Running) noexcept;
bool GargantuanHasRunningStructuralGrant() noexcept;
struct GargantuanReliableAttribution {
	std::uint64_t Token = 0;
	std::uint64_t ActivatedAtMicroseconds = 0;
	gargantuan::network::detail::InitialServiceTrace InitialTrace{};
	bool SubmissionMetadata = false, StructuralClass = false;
	std::uint64_t QualifiedSequence = 0, QualifiedPhase = 0, SchedulerAcceptedAtMicroseconds = 0;
	bool ProtectedOrdinary = false;
	std::uint64_t ProtectedAcceptedAtMicroseconds = 0;
	unsigned ProtectedOrigin = 6;
};
bool GargantuanBeginReliableSubmissionMetadata(bool StructuralClass,
	std::uint64_t QualifiedSequence = 0, std::uint64_t QualifiedPhase = 0,
	std::uint64_t SchedulerAcceptedAtMicroseconds = 0, bool ProtectedOrdinary = false,
	std::uint64_t ProtectedAcceptedAtMicroseconds = 0, unsigned ProtectedOrigin = 6) noexcept;
bool GargantuanInitialServiceTimingEnabled() noexcept;
std::uint64_t GargantuanInitialServiceClock() noexcept;
void GargantuanInitialServiceApiStage(bool Locked) noexcept;
// Scope exactly one synchronous reliable submission for sender-local retirement
// attribution. Begin/End are thread-local and introduce no global GNS lock.
bool GargantuanBeginReliableRetirementAttribution(std::uint64_t Token,
	std::uint64_t ActivatedAtMicroseconds = 0,
	const gargantuan::network::detail::InitialServiceTrace *InitialTrace = nullptr) noexcept;
void GargantuanEndReliableRetirementAttribution() noexcept;
GargantuanReliableAttribution GargantuanTakeReliableRetirementAttribution() noexcept;
void GargantuanCopyReliableServiceFeedback(const GargantuanReliableServiceCounters &Counters,
	int Pending, int Unacked, int NativeState, GargantuanReliableServiceSnapshot &Result);
GargantuanReliableServiceSnapshot *GargantuanGetClosingFeedback();
bool GargantuanReadNativeFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	GargantuanReliableServiceSnapshot &Result);
// Cessation capture only: reconcile existing ownership without cancellation or
// changing the execution domain. Every caller shares one absolute deadline.
bool GargantuanReadResolvedNativeFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	std::uint64_t DeadlineMicroseconds, GargantuanReliableServiceSnapshot &Result);
bool GargantuanConfigureNativeEgress(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	bool Enabled, std::uint64_t *Generation = nullptr);
bool GargantuanConfigureNativeEgressWorkers(ISteamNetworkingSockets *Interface,
	bool Enabled, unsigned Workers);
bool GargantuanDrainNativeEgress(ISteamNetworkingSockets *Interface,
	std::uint32_t TimeoutMilliseconds);
bool GargantuanDrainNativeConnection(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation, std::uint32_t TimeoutMilliseconds);
bool GargantuanReadPendingNativeReceiveJournal(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint32_t &Count, std::uint32_t &HighWater, bool &Overflow);
bool GargantuanTakeNativeTerminalFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation, GargantuanReliableServiceSnapshot &Result);
std::uint32_t GargantuanNativeTerminalEvidenceCapacity() noexcept;
std::uint32_t GargantuanNativeTerminalEvidenceReserved() noexcept;
bool GargantuanReserveNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t &Generation) noexcept;
bool GargantuanStoreNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation,
	const GargantuanReliableServiceSnapshot &Result) noexcept;
bool GargantuanReleaseNativeTerminalEvidence(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, std::uint64_t Generation) noexcept;
bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	GargantuanReliableServiceSnapshot &Result);
bool GargantuanCloseWithReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	int Reason, const char *Debug, GargantuanReliableServiceSnapshot &Result);
}

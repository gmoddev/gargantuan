#pragma once
#include "../../src/network/FiniteGrantServiceCurve.hpp"
#include <cstdint>

class ISteamNetworkingSockets;

struct GargantuanReliableServiceCounters {
	gargantuan::network::FiniteGrantServiceCurve StructuralGrantCurve;
	std::uint64_t UniqueReliableStreamBytesFirstSent = 0;
	std::uint64_t UniqueReliableStreamBytesAcked = 0;
	// Payload bytes belonging to the one native-attributed structural grant.
	// Reliable stream framing and ordinary reliable traffic are excluded.
	std::uint64_t StructuralPayloadBytesFirstSent = 0;
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

	void FirstSend(int Bytes, int StructuralPayloadBytes = 0, std::uint64_t NowMicroseconds = 0) noexcept;
	void Retransmit(int Bytes) noexcept;
	void AckSegment(int Bytes, bool AlreadyAcked) noexcept;
	void AttributeMessage(std::uint64_t Token, std::int64_t MessageNumber,
		int PayloadBytes = 0, std::uint64_t ActivatedAtMicroseconds = 0) noexcept;
	void ObserveActiveService(std::uint64_t NowMicroseconds) noexcept;
	void AckMessage(std::int64_t MessageNumber, int MessageBytes, int PrivateHeaderBytes) noexcept;
private:
	void Add(std::uint64_t &Value, int Bytes) noexcept;
	void SyncStructuralGrant() noexcept;
};

struct GargantuanReliableServiceSnapshot {
	GargantuanReliableServiceCounters Counters;
	std::uint64_t ObservedAtMicroseconds = 0;
	std::uint64_t PendingReliableStreamBytes = 0;
	std::uint64_t SentUnackedReliableStreamBytes = 0;
	int NativeState = 0;
};

namespace SteamNetworkingSocketsLib {
struct GargantuanReliableAttribution {
	std::uint64_t Token = 0;
	std::uint64_t ActivatedAtMicroseconds = 0;
};
// Scope exactly one synchronous reliable submission for sender-local retirement
// attribution. Begin/End are thread-local and introduce no global GNS lock.
bool GargantuanBeginReliableRetirementAttribution(std::uint64_t Token,
	std::uint64_t ActivatedAtMicroseconds = 0) noexcept;
void GargantuanEndReliableRetirementAttribution() noexcept;
GargantuanReliableAttribution GargantuanTakeReliableRetirementAttribution() noexcept;
void GargantuanCopyReliableServiceFeedback(const GargantuanReliableServiceCounters &Counters,
	int Pending, int Unacked, int NativeState, GargantuanReliableServiceSnapshot &Result);
GargantuanReliableServiceSnapshot *GargantuanGetClosingFeedback();
bool GargantuanReadNativeFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	GargantuanReliableServiceSnapshot &Result);
bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	GargantuanReliableServiceSnapshot &Result);
bool GargantuanCloseWithReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	int Reason, const char *Debug, GargantuanReliableServiceSnapshot &Result);
}

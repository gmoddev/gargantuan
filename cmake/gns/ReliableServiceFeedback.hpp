#pragma once
#include <cstdint>

struct GargantuanReliableServiceCounters {
	std::uint64_t UniqueReliableStreamBytesFirstSent = 0;
	std::uint64_t UniqueReliableStreamBytesAcked = 0;
	std::uint64_t ReliablePayloadBytesAcked = 0;
	std::uint64_t ReliableStreamBytesRetransmitted = 0;
	// Optional sender-local attribution for one bounded reliable obligation.
	// The token is never transmitted. The GNS message number is the stable
	// native identity retained through segment retry and final message release.
	std::uint64_t AttributedRetirementSequence = 0;
	std::uint64_t ActiveAttributedRetirementToken = 0;
	std::uint64_t ActiveAttributedMessageNumber = 0;
	std::uint64_t LastAttributedRetirementToken = 0;
	std::uint64_t LastAttributedRetirementMessageNumber = 0;
	std::uint64_t LastAttributedRetiredPayloadBytes = 0;
	bool Invalid = false;
	bool Purged = false;

	void FirstSend(int Bytes) noexcept;
	void Retransmit(int Bytes) noexcept;
	void AckSegment(int Bytes, bool AlreadyAcked) noexcept;
	void AttributeMessage(std::uint64_t Token, std::int64_t MessageNumber) noexcept;
	void AckMessage(std::int64_t MessageNumber, int MessageBytes, int PrivateHeaderBytes) noexcept;
private:
	void Add(std::uint64_t &Value, int Bytes) noexcept;
};

struct GargantuanReliableServiceSnapshot {
	GargantuanReliableServiceCounters Counters;
	std::uint64_t ObservedAtMicroseconds = 0;
	std::uint64_t PendingReliableStreamBytes = 0;
	std::uint64_t SentUnackedReliableStreamBytes = 0;
	int NativeState = 0;
};

namespace SteamNetworkingSocketsLib {
class ISteamNetworkingSockets;
// Scope exactly one synchronous reliable submission for sender-local retirement
// attribution. Begin/End are thread-local and introduce no global GNS lock.
bool GargantuanBeginReliableRetirementAttribution(std::uint64_t Token) noexcept;
void GargantuanEndReliableRetirementAttribution() noexcept;
std::uint64_t GargantuanTakeReliableRetirementAttribution() noexcept;
void GargantuanCopyReliableServiceFeedback(const GargantuanReliableServiceCounters &Counters,
	int Pending, int Unacked, int NativeState, GargantuanReliableServiceSnapshot &Result);
bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	GargantuanReliableServiceSnapshot &Result);
bool GargantuanCloseWithReliableServiceFeedback(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	int Reason, const char *Debug, GargantuanReliableServiceSnapshot &Result);
}

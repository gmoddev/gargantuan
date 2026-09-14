#pragma once

#include <cstdint>

class ISteamNetworkingSockets;

// Private static-link integration. No GNS public ABI or wire representation.
struct GargantuanReliableServiceCounters {
	std::uint64_t UniqueReliableStreamBytesFirstSent = 0;
	std::uint64_t UniqueReliableStreamBytesAcked = 0;
	std::uint64_t ReliablePayloadBytesAcked = 0;
	std::uint64_t ReliableStreamBytesRetransmitted = 0;
	bool Invalid = false;
	bool Purged = false;

	void FirstSend(int Bytes) noexcept;
	void Retransmit(int Bytes) noexcept;
	void AckSegment(int Bytes, bool AlreadyAcked) noexcept;
	void AckMessage(int MessageBytes, int PrivateHeaderBytes) noexcept;
private:
	void Add(std::uint64_t &Counter, int Bytes) noexcept;
};

struct GargantuanReliableServiceSnapshot {
	GargantuanReliableServiceCounters Counters;
	std::uint64_t ObservedAtMicroseconds = 0;
	std::uint64_t PendingReliableStreamBytes = 0;
	std::uint64_t SentUnackedReliableStreamBytes = 0;
	int NativeState = 0;
};

namespace SteamNetworkingSocketsLib {
// Thin GNS ownership hooks pass plain values while retaining their connection
// lock. Owned snapshot/capture code needs no generated/private GNS headers.
void GargantuanCopyReliableServiceFeedback(const GargantuanReliableServiceCounters &Counters,
	int Pending, int Unacked, int NativeState, GargantuanReliableServiceSnapshot &Result);
GargantuanReliableServiceSnapshot *GargantuanGetClosingFeedback();
bool GargantuanReadNativeFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, GargantuanReliableServiceSnapshot &Result);
bool GargantuanGetReliableServiceFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, GargantuanReliableServiceSnapshot &Result);
bool GargantuanCloseWithReliableServiceFeedback(ISteamNetworkingSockets *Interface,
	std::uint32_t Handle, int Reason, const char *Diagnostic, GargantuanReliableServiceSnapshot &Result);
}

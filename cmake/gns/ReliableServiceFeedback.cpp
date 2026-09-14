#include "ReliableServiceFeedback.hpp"
#include <limits>

void GargantuanReliableServiceCounters::Add(std::uint64_t &Counter, int Bytes) noexcept {
	if (Invalid) return;
	if (Bytes < 0 || static_cast<std::uint64_t>(Bytes) > std::numeric_limits<std::uint64_t>::max() - Counter) {
		Invalid = true;
		return;
	}
	Counter += static_cast<std::uint64_t>(Bytes);
}

void GargantuanReliableServiceCounters::FirstSend(int Bytes) noexcept {
	Add(UniqueReliableStreamBytesFirstSent, Bytes);
}
void GargantuanReliableServiceCounters::Retransmit(int Bytes) noexcept {
	Add(ReliableStreamBytesRetransmitted, Bytes);
}
void GargantuanReliableServiceCounters::AckSegment(int Bytes, bool AlreadyAcked) noexcept {
	if (AlreadyAcked || Invalid) return;
	if (Bytes < 0 || UniqueReliableStreamBytesAcked > UniqueReliableStreamBytesFirstSent ||
		static_cast<std::uint64_t>(Bytes) > UniqueReliableStreamBytesFirstSent - UniqueReliableStreamBytesAcked) {
		Invalid = true;
		return;
	}
	Add(UniqueReliableStreamBytesAcked, Bytes);
}
void GargantuanReliableServiceCounters::AckMessage(int MessageBytes, int PrivateHeaderBytes) noexcept {
	if (PrivateHeaderBytes <= 0 || MessageBytes < PrivateHeaderBytes) {
		Invalid = true;
		return;
	}
	Add(ReliablePayloadBytesAcked, MessageBytes - PrivateHeaderBytes);
}

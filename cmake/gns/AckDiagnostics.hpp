#pragma once
#include <array>
#include <cstdint>

namespace SteamNetworkingSocketsLib {
std::uint64_t GargantuanReliableServiceClock() noexcept;
}

// Opt-in native observation only. Never consulted by ACK/service scheduling.
// Native timestamps describe GNS timers; AtMicroseconds uses the same steady
// clock as finite-grant service and the Main observer. Do not mix the epochs.
struct GargantuanAckDiagnostics {
	enum Kind : std::uint32_t { Deadline = 1, MessageReceived, AckSerialized,
		AckPacketSent, MessageAcked, FragmentedAckSerialized, PromptRequestSent, PromptRequestFailed };
	struct Event {
		std::uint64_t AtMicroseconds = 0;
		std::int64_t NativeAtMicroseconds = 0;
		std::int64_t Identity = 0;
		std::int64_t Value = 0;
		std::uint32_t Type = 0;
	};
	std::array<Event, 128> Events{};
	std::uint32_t Count = 0;
	bool Overflow = false;
	std::uint64_t FirstReliablePacketAt = 0, LastReliablePacketAt = 0, ReliablePackets = 0;
	std::int64_t LastReliablePacketNumber = 0;
	std::int64_t LastDeadline = 0, SerializedAckPacket = 0;
	std::int64_t LastRecordedSerializedAck = 0, LastRecordedSentAck = 0;
	std::uint64_t RepeatedAckSerializations = 0, AckPacketsSent = 0, AckPacketBytes = 0;
	std::uint64_t LastAckPacketSentAt = 0;
	std::int64_t LastAckPacketSentNativeAt = 0;
	std::uint64_t MaximumPromptReserveBytes = 0;
	void Record(Kind Type, std::int64_t NativeAt, std::int64_t Identity, std::int64_t Value) noexcept {
		// Every data packet may repeat the same ACK. Retain its first emission
		// and aggregate all repetitions without consuming the critical event log.
		if (Type == AckSerialized) {
			if (LastRecordedSerializedAck == Identity) { ++RepeatedAckSerializations; return; }
			LastRecordedSerializedAck = Identity;
		}
		if (Type == AckPacketSent) {
			++AckPacketsSent;
			if (Value > 0) AckPacketBytes += static_cast<std::uint64_t>(Value);
			LastAckPacketSentAt = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
			LastAckPacketSentNativeAt = NativeAt;
			if (LastRecordedSentAck == Identity) return;
			LastRecordedSentAck = Identity;
		}
		if (Count == Events.size()) { Overflow = true; return; }
		auto &Next = Events[Count++];
		Next.AtMicroseconds = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
		Next.NativeAtMicroseconds = NativeAt;
		Next.Identity = Identity;
		Next.Value = Value;
		Next.Type = Type;
	}
	void Received(std::int64_t NativeAt, std::int64_t Packet, std::int64_t Due) noexcept {
		LastReliablePacketAt = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
		if (!FirstReliablePacketAt) FirstReliablePacketAt = LastReliablePacketAt;
		++ReliablePackets;
		LastReliablePacketNumber = Packet;
		if (LastDeadline != Due) { LastDeadline = Due; Record(Deadline, NativeAt, Packet, Due); }
	}
};

class ISteamNetworkingSockets;
namespace SteamNetworkingSocketsLib {
bool GargantuanAccessAckDiagnostics(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	bool Reset, GargantuanAckDiagnostics &Result);
bool GargantuanConfigurePromptGrantAck(ISteamNetworkingSockets *Interface, std::uint32_t Handle, bool Enabled);
}

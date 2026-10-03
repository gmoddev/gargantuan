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
	std::int64_t LastRecordedFragmentedAck = -1;
	std::uint64_t FragmentedAckSerializations = 0, LastFragmentedAckAt = 0;
	std::int64_t LastFragmentedAckNativeAt = 0;
	std::uint64_t RepeatedAckSerializations = 0, AckPacketsSent = 0, AckPacketBytes = 0;
	std::uint64_t LastAckPacketSentAt = 0;
	std::int64_t LastAckPacketSentNativeAt = 0;
	std::uint64_t MaximumPromptReserveBytes = 0;
	std::uint64_t GrantWholeWireBytes = 0, GrantWholeWireCeiling = 0, PromptTailBudget = 0;
	bool GrantWireInvalid = false;
	bool PromptFinalWireAllowed = false;
	std::uint64_t PromptFinalPriorWireBytes = 0;
	std::uint64_t BackgroundRate = 0, BackgroundBurst = 0;
	bool FailNextPromptPacket = false;
	bool FailNextPromptSocketSend = false;
	std::uint64_t InjectedNativeSendFailures = 0, FirstSentBytesAtInjectedFailure = 0;
	std::uint64_t InjectedSocketSendFailures = 0;
	std::uint64_t FailedPacketReferencesReleased = 0;
	// Associated datagrams, including duplicates and packets rejected by crypto.
	// Kept independently of a grant so late duplicates remain observable.
	std::uint64_t AssociatedReceivedPackets = 0, AssociatedReceivedUdpBytes = 0;
	std::uint64_t StatsRequestsSent = 0, StatsImmediateSent = 0, StatsInstantaneousSent = 0, StatsLifetimeSent = 0;
	std::uint64_t StatsRequestsReceived = 0, StatsImmediateReceived = 0, StatsInstantaneousReceived = 0;
	std::uint64_t TracerRequestsSent = 0, FirstTracerAt = 0, FirstInstantaneousAt = 0;
	std::int64_t NativeSnapshotNow = 0, NativeLastPingSent = 0, NativeLastPingReceived = 0, NativeStatsInFlight = 0;
	int NativeTracerReady = 0, NativeActivity = 0;
	std::uint32_t ObservedStatsNeedMask = 0;
	void Stats(bool Sent, bool Request, bool Immediate, bool Instantaneous, bool Lifetime, bool Tracer) noexcept {
		if (Sent) {
			StatsRequestsSent += Request; StatsImmediateSent += Immediate;
			StatsInstantaneousSent += Instantaneous; StatsLifetimeSent += Lifetime;
			TracerRequestsSent += Tracer && Request;
			if (Tracer && Request && !FirstTracerAt) FirstTracerAt = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
			if (Instantaneous && !FirstInstantaneousAt) FirstInstantaneousAt = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
		} else {
			StatsRequestsReceived += Request; StatsImmediateReceived += Immediate;
			StatsInstantaneousReceived += Instantaneous;
		}
	}
	void Incoming(int Bytes) noexcept {
		if (Bytes <= 0 || AssociatedReceivedPackets == UINT64_MAX ||
			static_cast<std::uint64_t>(Bytes) > UINT64_MAX - AssociatedReceivedUdpBytes) {
			Overflow = true; return;
		}
		++AssociatedReceivedPackets;
		AssociatedReceivedUdpBytes += static_cast<std::uint64_t>(Bytes);
	}
	void Record(Kind Type, std::int64_t NativeAt, std::int64_t Identity, std::int64_t Value) noexcept {
		// Every data packet may repeat the same ACK. Retain its first emission
		// and aggregate all repetitions without consuming the critical event log.
		if (Type == AckSerialized) {
			if (LastRecordedSerializedAck == Identity) { ++RepeatedAckSerializations; return; }
			LastRecordedSerializedAck = Identity;
		}
		// A receive gap can serialize the same fragmented ACK on every outgoing
		// packet. Preserve its first event and exact repetition/last-time totals,
		// as for ordinary ACKs, without displacing message/retirement evidence.
		if (Type == FragmentedAckSerialized) {
			if (FragmentedAckSerializations == UINT64_MAX) { Overflow = true; return; }
			++FragmentedAckSerializations;
			LastFragmentedAckAt = SteamNetworkingSocketsLib::GargantuanReliableServiceClock();
			LastFragmentedAckNativeAt = NativeAt;
			if (LastRecordedFragmentedAck == Identity) return;
			LastRecordedFragmentedAck = Identity;
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
bool GargantuanConfigurePromptGrantAck(ISteamNetworkingSockets *Interface, std::uint32_t Handle, bool Enabled,
	std::uint64_t Reserve, std::uint64_t StructuralPool, std::uint64_t TailBudget, std::uint64_t Peers);
bool GargantuanArmPromptFailure(ISteamNetworkingSockets *Interface, std::uint32_t Handle, bool AtSocket);
bool GargantuanConfigureFundedGrantAck(ISteamNetworkingSockets *Interface, std::uint32_t Handle,
	std::uint64_t Reserve, std::uint64_t StructuralPool, std::uint64_t Peers);
}

#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace gargantuan::runtime_detail {
// Private Main-thread diagnostic tap. Only a scoped test-owned sink enables it;
// no payload ownership, queue, environment switch, wire field or production log.
struct PublicationLatencyRecord {
	const char *Stage = "";
	network::ConnectionId Connection;
	ObjectId Object;
	std::uint64_t Tick = 0, Sequence = 0, Due = 0, Epoch = 0, Nanoseconds = 0;
	std::uint32_t Kind = 0, Tier = 0, Bytes = 0, Operations = 0;
};
struct OrdinaryDemandTag {
	std::uint64_t Sequence = 0, Phase = 0, AcceptedAtMicroseconds = 0;
	// Explicit qualification scope; legal overload/bootstrap remain observable
	// demands without being assigned the ordinary 50-ms workload contract.
	bool Qualified = false;
};
struct PublicationLatencySink {
	void *Context = nullptr;
	bool (*Selected)(void *, network::ConnectionId) noexcept = nullptr;
	void (*Record)(void *, PublicationLatencyRecord) noexcept = nullptr;
	void (*Packet)(void *, const char *, network::ConnectionId, std::span<const std::byte>, std::uint64_t) noexcept = nullptr;
	OrdinaryDemandTag (*QueueOrdinary)(void *, network::ConnectionId, std::span<const std::byte>) noexcept = nullptr;
	OrdinaryDemandTag (*DirectOrdinary)(void *, network::ConnectionId) noexcept = nullptr;
	// Optional RPC-only dequeue metadata. Age starts when pinned GNS has a
	// complete ordered message, not at UDP arrival or sender native execution.
	void (*NativeReceive)(void *, network::ConnectionId, std::span<const std::byte>,
		std::uint64_t, std::uint64_t, std::int64_t, std::size_t, std::int64_t) noexcept = nullptr;
};
inline thread_local PublicationLatencySink *ActivePublicationLatency = nullptr;
inline thread_local OrdinaryDemandTag ActiveOrdinaryDemand;
class OrdinaryDemandScope final {
	OrdinaryDemandTag Previous;
public:
	explicit OrdinaryDemandScope(OrdinaryDemandTag Value) noexcept : Previous(ActiveOrdinaryDemand) { ActiveOrdinaryDemand = Value; }
	~OrdinaryDemandScope() { ActiveOrdinaryDemand = Previous; }
	OrdinaryDemandScope(const OrdinaryDemandScope &) = delete;
	OrdinaryDemandScope &operator=(const OrdinaryDemandScope &) = delete;
};
inline std::uint64_t PublicationLatencyNow() noexcept {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline bool PublicationLatencySelected(network::ConnectionId Connection) noexcept {
	return ActivePublicationLatency && ActivePublicationLatency->Selected(ActivePublicationLatency->Context, Connection);
}
inline OrdinaryDemandTag QueueOrdinaryDemand(network::ConnectionId Connection, std::span<const std::byte> Payload) noexcept {
	if (!PublicationLatencySelected(Connection) || !ActivePublicationLatency->QueueOrdinary) return {};
	auto Tag = ActivePublicationLatency->QueueOrdinary(ActivePublicationLatency->Context, Connection, Payload);
	if (Tag.Sequence && !Tag.AcceptedAtMicroseconds) Tag.AcceptedAtMicroseconds = PublicationLatencyNow() / 1'000;
	return Tag;
}
inline bool PublicationRpcNativeReceiveSelected(network::ConnectionId Connection,
	std::span<const std::byte> Payload) noexcept {
	if (!PublicationLatencySelected(Connection) || !ActivePublicationLatency->NativeReceive ||
		Payload.size() < 32 || std::memcmp(Payload.data(), "GRMT", 4) != 0) return false;
	const auto Kind = static_cast<network::RemoteMessageKind>(std::to_integer<std::uint8_t>(Payload[6]));
	return Kind == network::RemoteMessageKind::Request || Kind == network::RemoteMessageKind::Response ||
		Kind == network::RemoteMessageKind::RequestError;
}
inline void RecordPublicationRpcNativeReceive(network::ConnectionId Connection,
	std::span<const std::byte> Payload, std::uint64_t DequeuedAtNanoseconds,
	std::uint64_t AgeSampleEndedNanoseconds,
	std::int64_t ReadyQueueAgeMicroseconds, std::size_t AdapterPendingEventDepth,
	std::int64_t NativeMessageNumber) noexcept {
	if (!PublicationRpcNativeReceiveSelected(Connection, Payload)) return;
	ActivePublicationLatency->NativeReceive(ActivePublicationLatency->Context, Connection, Payload,
		DequeuedAtNanoseconds, AgeSampleEndedNanoseconds, ReadyQueueAgeMicroseconds,
		AdapterPendingEventDepth, NativeMessageNumber);
}
inline OrdinaryDemandTag DirectOrdinaryDemand(network::ConnectionId Connection) noexcept {
	if (!PublicationLatencySelected(Connection) || !ActivePublicationLatency->DirectOrdinary) return {};
	return ActivePublicationLatency->DirectOrdinary(ActivePublicationLatency->Context, Connection);
}
inline void RecordPublicationLatency(PublicationLatencyRecord Record) noexcept {
	if (!PublicationLatencySelected(Record.Connection)) return;
	Record.Nanoseconds = PublicationLatencyNow();
	ActivePublicationLatency->Record(ActivePublicationLatency->Context, Record);
}
inline void RecordPublicationPacket(const char *Stage, network::ConnectionId Connection,
	std::span<const std::byte> Payload) noexcept {
	if (!PublicationLatencySelected(Connection)) return;
	ActivePublicationLatency->Packet(ActivePublicationLatency->Context, Stage, Connection, Payload, PublicationLatencyNow());
}
class PublicationPacketScope final {
	const char *End;
	network::ConnectionId Connection;
	std::span<const std::byte> Payload;
public:
	PublicationPacketScope(const char *Begin, const char *End, network::ConnectionId Connection, std::span<const std::byte> Payload)
		: End(End), Connection(Connection), Payload(Payload) { RecordPublicationPacket(Begin, Connection, Payload); }
	~PublicationPacketScope() { RecordPublicationPacket(End, Connection, Payload); }
	PublicationPacketScope(const PublicationPacketScope &) = delete;
	PublicationPacketScope &operator=(const PublicationPacketScope &) = delete;
};
}

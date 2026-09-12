#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
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
struct PublicationLatencySink {
	void *Context = nullptr;
	bool (*Selected)(void *, network::ConnectionId) noexcept = nullptr;
	void (*Record)(void *, PublicationLatencyRecord) noexcept = nullptr;
	void (*Packet)(void *, const char *, network::ConnectionId, std::span<const std::byte>, std::uint64_t) noexcept = nullptr;
};
inline thread_local PublicationLatencySink *ActivePublicationLatency = nullptr;
inline std::uint64_t PublicationLatencyNow() noexcept {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline bool PublicationLatencySelected(network::ConnectionId Connection) noexcept {
	return ActivePublicationLatency && ActivePublicationLatency->Selected(ActivePublicationLatency->Context, Connection);
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

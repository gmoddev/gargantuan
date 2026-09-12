#pragma once
#include "gargantuan/network/Connection.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace gargantuan::network::detail {
// Private, borrowed Main-thread observation only. No backend handles, payload
// ownership, scheduling controls or production history. Negative = unavailable.
struct GnsServiceRecord {
	const char *Stage = "";
	ConnectionId Connection;
	std::uint64_t Nanoseconds = 0;
	std::int64_t MessageNumber = -1, ReceiveAgeUs = -1, QueueUs = -1;
	std::int32_t PendingReliable = -1, UnackedReliable = -1, PendingUnreliable = -1;
	std::int32_t Rate = -1, RateMin = -1, RateMax = -1, SendBuffer = -1, Ping = -1;
	float OutBytesPerSecond = -1, InBytesPerSecond = -1;
	std::uint32_t Bytes = 0;
	int Role = -1, Delivery = -1, Traffic = -1, Result = -1;
};
struct GnsServiceSink {
	void *Context = nullptr;
	void (*Record)(void *, GnsServiceRecord, std::span<const std::byte>) noexcept = nullptr;
};
inline thread_local GnsServiceSink *ActiveGnsService = nullptr;
}

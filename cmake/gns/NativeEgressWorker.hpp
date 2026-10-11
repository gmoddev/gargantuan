#pragma once

#include <cstdint>

struct iovec;

namespace SteamNetworkingSocketsLib {

class IBoundUDPSocket;
struct GargantuanNativeEgressTicket;

enum class GargantuanNativeEgressQueueStatus : std::uint8_t {
	Disabled,
	Queued,
	Rejected,
};

struct GargantuanNativeEgressResult {
	bool Positive = false;
	int Bytes = 0;
	int Error = 0;
	std::uint64_t CompletedAtMicroseconds = 0;
	std::int64_t CompletedAtGnsMicroseconds = 0;
	std::uint64_t SocketGeneration = 0;
	std::uint64_t QueuedAtMicroseconds = 0;
	std::uint64_t StartedAtMicroseconds = 0;
	std::uint64_t DispatchSequence = 0;
};

// All calls except the worker's internal OS operation require the GNS global
// lock. A queued ticket owns a pre-reserved result record and raw-socket lease.
// The result remains immutable until the actor releases the ticket.
bool GargantuanConfigureNativeEgress(bool Enabled, unsigned Workers) noexcept;
bool GargantuanCanQueueNativeEgress() noexcept;
void GargantuanFenceNativeEgress() noexcept;
bool GargantuanWaitNativeEgressIdle(std::uint32_t TimeoutMilliseconds) noexcept;
GargantuanNativeEgressQueueStatus GargantuanQueueNativeEgress(
	IBoundUDPSocket *BoundSocket, int ChunkCount, const iovec *Chunks,
	int TotalBytes, bool Protected, GargantuanNativeEgressTicket *&Ticket
) noexcept;
bool GargantuanReadNativeEgressResult(
	const GargantuanNativeEgressTicket *Ticket,
	GargantuanNativeEgressResult &Result
) noexcept;
bool GargantuanWaitNativeEgressResult(
	const GargantuanNativeEgressTicket *Ticket,
	std::uint32_t TimeoutMilliseconds,
	GargantuanNativeEgressResult &Result
) noexcept;
bool GargantuanCancelUnstartedNativeEgress(GargantuanNativeEgressTicket *Ticket) noexcept;
void GargantuanReleaseNativeEgress(GargantuanNativeEgressTicket *Ticket) noexcept;
[[noreturn]] void GargantuanFailStopNativeEgress(const char *Reason) noexcept;
// Called by the GNS service actor after worker wake and before thinker dispatch.
// The connection registry is owned by the SNP patch, not by worker threads.
void GargantuanWakeReadyNativeEgressConnections() noexcept;

#if defined(GARGANTUAN_NATIVE_EGRESS_TEST_HOOKS)
enum class GargantuanNativeEgressTestPausePoint : std::uint8_t {
	BeforeOsCall,
	AfterOsReturn,
};
enum class GargantuanNativeEgressTestTraffic : std::uint8_t {
	Any,
	Ordinary,
	Protected,
};
bool GargantuanArmNativeEgressPauseForTest(
	GargantuanNativeEgressTestPausePoint Point,
	std::uint32_t MaximumMilliseconds,
	GargantuanNativeEgressTestTraffic Traffic = GargantuanNativeEgressTestTraffic::Any,
	int MinimumDatagramBytes = 0
) noexcept;
bool GargantuanWaitNativeEgressPauseForTest(std::uint32_t TimeoutMilliseconds) noexcept;
void GargantuanResumeNativeEgressForTest() noexcept;
bool GargantuanFailNextNativeEgressForTest() noexcept;
std::uint64_t GargantuanNativeEgressFailuresForTest() noexcept;
#endif

}

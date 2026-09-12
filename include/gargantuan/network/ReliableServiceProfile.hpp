#pragma once

#include <cstdint>
#include <string_view>

namespace gargantuan::network {

inline constexpr std::uint64_t ReliableServiceEnvelopeBytes = 32;
inline constexpr std::uint64_t MaximumReliableServiceGroupBytes = 512 * 1024;

struct ReliableByteAdmissionMetrics {
	std::uint64_t ReservedBytes = 0, AcceptedBytes = 0, RolledBackBytes = 0;
	std::uint64_t CreditDeferrals = 0, BacklogDeferrals = 0, FeedbackDeferrals = 0;
	std::uint64_t SizeDeferrals = 0, DeferredBytes = 0, FairnessDeferrals = 0;
	std::uint64_t PeerBacklogHighWater = 0, GlobalBacklogHighWater = 0;
	std::uint64_t MaximumAdmissionWaitMicroseconds = 0, OldestWaitMicroseconds = 0;
	std::uint64_t GlobalCreditHighWater = 0, PeerCreditHighWater = 0;
};

// Trusted native deployment input, never a negotiated client preference. Rates
// are finite reservations, not measured throughput or a promise about a path.
struct ReliableServiceProfile {
	std::uint64_t ConnectionRate = 256 * 1024;
	std::uint64_t AggregateRate = 256 * 1024;
	// Backend on-wire ceiling is separate from the application reservation.
	// Default explicit profiles fund 100% additional packet/realtime headroom;
	// this is still not evidence of an actual path service lower bound.
	std::uint64_t BackendRate = 512 * 1024;
	std::uint32_t MaximumConnections = 1;
	std::uint32_t StructuralPermille = 750;
	std::uint64_t PeerBurst = MaximumReliableServiceGroupBytes;
	std::uint64_t GlobalBurst = MaximumReliableServiceGroupBytes;
	std::uint64_t GameplayBurst = 256 * 1024 + ReliableServiceEnvelopeBytes;
	std::uint64_t PeerBacklog = MaximumReliableServiceGroupBytes + 2 * (256 * 1024 + ReliableServiceEnvelopeBytes);
	std::uint64_t GlobalBacklog = MaximumReliableServiceGroupBytes + 2 * (256 * 1024 + ReliableServiceEnvelopeBytes);
	// Explicit deployment allowance for request/path/host service outside the
	// outbound FIFO. Capacity compatibility still requires path qualification.
	std::uint32_t NonQueueAllowanceMilliseconds = 100;
	bool RequireLatencyCompatibility = false;

	[[nodiscard]] std::string_view ValidationError() const;
	[[nodiscard]] bool IsValid() const { return ValidationError().empty(); }
	[[nodiscard]] bool IsLatencyCompatible() const;
	[[nodiscard]] std::uint64_t PeerStructuralRate() const { return ConnectionRate * StructuralPermille / 1000; }
	[[nodiscard]] std::uint64_t GlobalStructuralRate() const { return AggregateRate * StructuralPermille / 1000; }
};

}

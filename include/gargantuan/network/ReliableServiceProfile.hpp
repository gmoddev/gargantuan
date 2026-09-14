#pragma once

#include <cstdint>
#include <string_view>

namespace gargantuan::network {

inline constexpr std::uint64_t ReliableServiceEnvelopeBytes = 32;
inline constexpr std::uint64_t MaximumReliableServiceGroupBytes = 512 * 1024;

enum class ReliableServiceMode : std::uint8_t { FULL_RESERVATION, POOLED_SERVICE };

// The single accepted Option C candidate. These are eligibility and funded
// service bounds, never measurements of the configured host or path.
struct PooledReliableServiceProfile {
	std::uint64_t BackendCap = 96 * 1024 * 1024;
	std::uint64_t StructuralPool = 64 * 1024 * 1024;
	std::uint64_t GameplayReserve = 8 * 1024 * 1024;
	std::uint64_t ControlRealtimeReserve = 4 * 1024 * 1024;
	std::uint64_t RequiredTransportReserve = 8 * 1024 * 1024;
	std::uint64_t PeerDrainFloor = 16 * 1024 * 1024;
	std::uint64_t PeerCreditRate = 2 * 1024 * 1024;
	std::uint64_t GlobalCreditRate = 64 * 1024 * 1024;
	std::uint64_t PeerBurstCap = MaximumReliableServiceGroupBytes;
	std::uint64_t GlobalBurstCap = 4 * MaximumReliableServiceGroupBytes;
	std::uint64_t PeerPendingCap = MaximumReliableServiceGroupBytes;
	std::uint64_t GlobalPendingCap = 32 * MaximumReliableServiceGroupBytes;
	std::uint32_t MaximumDrainGrants = 4;
	std::uint64_t FeedbackFreshnessMicroseconds = 50'000;
	std::uint64_t RequalificationMicroseconds = 1'000'000;
	static constexpr std::uint64_t QueueWindowMicroseconds = 50'000;
	static constexpr std::uint64_t PeerGameplayBurst = 20 * 1024;
	static constexpr std::uint64_t GameplayBurst = 160 * 1024;
	static constexpr std::uint64_t GameplaySteadyRate = 256 * 1024;
	static constexpr std::uint64_t ControlBurst = 64 * 1024;
	[[nodiscard]] std::string_view ValidationError() const;
};

struct ReliableByteAdmissionMetrics {
	std::uint64_t ReservedBytes = 0, AcceptedBytes = 0, RolledBackBytes = 0;
	std::uint64_t CreditDeferrals = 0, BacklogDeferrals = 0, FeedbackDeferrals = 0;
	std::uint64_t SizeDeferrals = 0, DeferredBytes = 0, FairnessDeferrals = 0;
	std::uint64_t PeerBacklogHighWater = 0, GlobalBacklogHighWater = 0;
	std::uint64_t MaximumAdmissionWaitMicroseconds = 0, OldestWaitMicroseconds = 0;
	std::uint64_t GlobalCreditHighWater = 0, PeerCreditHighWater = 0;
	std::uint64_t VerifiedAttributedRetirement = 0, TerminalReleasedBytes = 0, OutstandingBytes = 0;
	std::uint64_t OutstandingHighWater = 0, ActiveDrainGrants = 0, DrainGrantsHighWater = 0;
	std::uint64_t GrantDeferrals = 0, FundedDeferrals = 0, QualificationGrants = 0;
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
	ReliableServiceMode Mode = ReliableServiceMode::FULL_RESERVATION;
	PooledReliableServiceProfile Pooled;

	[[nodiscard]] static ReliableServiceProfile PooledService() {
		ReliableServiceProfile Result;
		Result.Mode = ReliableServiceMode::POOLED_SERVICE;
		Result.MaximumConnections = 32;
		return Result;
	}
	[[nodiscard]] bool IsPooled() const { return Mode == ReliableServiceMode::POOLED_SERVICE; }
	// GNS charges physical packets, whereas PeerDrainFloor measures unique
	// reliable stream bytes. Fund each active grant's ceiling from the existing
	// structural and transport pools; never mistake a configured rate for service.
	[[nodiscard]] std::uint64_t BackendSendRate() const {
		return IsPooled() ? (Pooled.StructuralPool + Pooled.RequiredTransportReserve) / Pooled.MaximumDrainGrants : BackendRate;
	}
	[[nodiscard]] std::uint64_t PeerCreditCap() const { return IsPooled() ? Pooled.PeerBurstCap : PeerBurst; }
	[[nodiscard]] std::uint64_t GlobalCreditCap() const { return IsPooled() ? Pooled.GlobalBurstCap : GlobalBurst; }

	[[nodiscard]] std::string_view ValidationError() const;
	[[nodiscard]] bool IsValid() const { return ValidationError().empty(); }
	[[nodiscard]] bool IsLatencyCompatible() const;
	[[nodiscard]] std::uint64_t PeerStructuralRate() const { return ConnectionRate * StructuralPermille / 1000; }
	[[nodiscard]] std::uint64_t GlobalStructuralRate() const { return AggregateRate * StructuralPermille / 1000; }
};

}

#pragma once

#include "PooledReliableServiceFeedback.hpp"

namespace gargantuan::network::detail {
// Borrowed observation on the owning GameSession thread. A diagnostic sink
// may copy into bounded storage only; it must not reenter or alter admission.
struct PooledServiceRecord {
	ConnectionId Connection;
	std::uint64_t SimulationTick = 0, NowMicroseconds = 0;
	std::uint64_t DebtToken = 0, DebtBytes = 0;
	std::uint64_t StructuralJournalLag = 0;
	ReliableServiceAcceptedBytes Accepted;
	std::optional<ReliableServiceFeedback> Feedback;
	PooledReliableServiceFeedback::Result Result;
	ReliableByteAdmissionMetrics Admission;
};
struct PooledServiceSink {
	void *Context = nullptr;
	void (*Record)(void *, const PooledServiceRecord &) noexcept = nullptr;
	// Successful production admission, before transport submission. Bootstrap
	// keeps exact debt ownership but deliberately has no qualified F1 clock.
	void (*AcceptedGrant)(void *, ConnectionId, std::uint64_t Token,
		std::uint64_t Bytes, std::uint64_t ActivatedAtMicroseconds) noexcept = nullptr;
};
inline thread_local PooledServiceSink *ActivePooledService = nullptr;
}

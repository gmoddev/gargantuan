#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/Scheduler.hpp"

namespace gargantuan::network {
	// Internal native prepared-frame path: consumes the exact codec output used
	// for pre-acceptance sizing; ordering still comes from the prepared sequence.
	[[nodiscard]] SerializationResult<SchedulerSubmitResult> QueueEncodedReplicationFrame(
		ReliableReplicationSequence Sequence, std::vector<std::byte> Encoded,
		ConnectionId Destination, const NetworkLimits &Limits, INetworkScheduler &Scheduler);
	[[nodiscard]] SerializationResult<SchedulerSubmitResult> QueueReplicationFrame(
		const ReplicationFrame &Frame,
		ConnectionId Destination,
		const NetworkLimits &Limits,
		INetworkScheduler &Scheduler
	);
}

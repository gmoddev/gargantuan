#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/network/Scheduler.hpp"

namespace gargantuan::network {
	[[nodiscard]] SerializationResult<SchedulerSubmitResult> QueueReplicationFrame(
		const ReplicationFrame &Frame,
		ConnectionId Destination,
		const NetworkLimits &Limits,
		INetworkScheduler &Scheduler
	);
}

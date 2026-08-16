#include "gargantuan/network/ReplicationTransport.hpp"

namespace gargantuan::network {
	SerializationResult<SchedulerSubmitResult> QueueReplicationFrame(
		const ReplicationFrame &Frame,
		ConnectionId Destination,
		const NetworkLimits &Limits,
		INetworkScheduler &Scheduler
	) {
		auto Encoded = EncodeReplicationFrame(Frame);
		if (!Encoded) return std::unexpected(Encoded.error());
		auto Intent = MakeNetworkMessageIntent(
			Destination,
			DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication,
			ReliableReplicationOrder{Frame.Sequence},
			std::move(*Encoded),
			Limits
		);
		if (!Intent)
			return SerializationFailure(
				SerializationErrorCode::LimitExceeded,
				"Replication frame cannot be admitted under the negotiated network limits"
			);
		return Scheduler.Submit(std::move(*Intent));
	}
}

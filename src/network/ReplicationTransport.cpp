#include "gargantuan/network/ReplicationTransport.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"

namespace gargantuan::network {
	SerializationResult<SchedulerSubmitResult> QueueReplicationFrame(
		const ReplicationFrame &Frame,
		ConnectionId Destination,
		const NetworkLimits &Limits,
		INetworkScheduler &Scheduler
	) {
		auto Encoded = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralSubmissionEncode, [&] { return EncodeReplicationFrame(Frame); });
		if (!Encoded) return std::unexpected(Encoded.error());
		auto Intent = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralIntent, [&] { return MakeNetworkMessageIntent(
			Destination,
			DeliveryMode::ReliableOrdered,
			TrafficClass::StructuralReplication,
			ReliableReplicationOrder{Frame.Sequence},
			std::move(*Encoded),
			Limits
		); });
		if (!Intent)
			return SerializationFailure(
				SerializationErrorCode::LimitExceeded,
				"Replication frame cannot be admitted under the negotiated network limits"
			);
		return runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralSubmit, [&] { return Scheduler.Submit(std::move(*Intent)); });
	}
}

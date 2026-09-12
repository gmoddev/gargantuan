#include "gargantuan/network/ReliableServiceProfile.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"
#include "gargantuan/network/Scheduler.hpp"

#include <limits>

namespace gargantuan::network {
namespace {
std::string_view ResourceError(const ReliableServiceProfile &Value) {
	if (!Value.ConnectionRate || Value.ConnectionRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
		return "ConnectionRate must fit the positive backend rate range";
	if (!Value.MaximumConnections || Value.MaximumConnections > 4096)
		return "MaximumConnections must be in 1..4096";
	if (Value.BackendRate < Value.ConnectionRate || Value.BackendRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
		return "BackendRate must cover ConnectionRate and fit the positive backend rate range";
	if (Value.AggregateRate / Value.MaximumConnections < Value.ConnectionRate ||
		Value.AggregateRate > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) * Value.MaximumConnections)
		return "AggregateRate must fund MaximumConnections times ConnectionRate";
	if (!Value.StructuralPermille || Value.StructuralPermille > 750 || !Value.PeerStructuralRate())
		return "StructuralPermille must reserve at least 25 percent for gameplay and permit structural progress";
	if (Value.PeerBurst < MaximumReliableServiceGroupBytes || Value.GlobalBurst < MaximumReliableServiceGroupBytes)
		return "Burst credit must hold a complete 512 KiB supported atomic group";
	if (Value.PeerBacklog > NativeMaximumQueuedReliableBytes || Value.GlobalBacklog > NativeMaximumSchedulerQueuedReliableBytes)
		return "Reliable service backlog exceeds the native finite memory ceiling";
	if (Value.GameplayBurst < MaximumRemoteFrameBytes + ReliableServiceEnvelopeBytes ||
		Value.GameplayBurst > Value.PeerBacklog / 2 ||
		Value.PeerBurst > Value.PeerBacklog - 2 * Value.GameplayBurst)
		return "PeerBacklog must fund structural burst, existing gameplay burst and fresh gameplay reserve";
	if (Value.GlobalBacklog < Value.PeerBacklog || Value.GameplayBurst > Value.GlobalBacklog / Value.MaximumConnections / 2 ||
		Value.GlobalBurst > Value.GlobalBacklog - 2 * Value.GameplayBurst * Value.MaximumConnections)
		return "GlobalBacklog must fund the structural burst and gameplay headroom for every admitted peer";
	if (Value.NonQueueAllowanceMilliseconds == 0 || Value.NonQueueAllowanceMilliseconds >= 250)
		return "NonQueueAllowanceMilliseconds must leave positive time within the 250 ms Event/action target";
	return {};
}
}

bool ReliableServiceProfile::IsLatencyCompatible() const {
	if (!ResourceError(*this).empty()) return false;
	const auto QueueMilliseconds = 250 - NonQueueAllowanceMilliseconds;
	// Necessary worst-case capacity test, not a percentile inferred from a mean
	// and not a claim that the configured rate is an actual service lower bound.
	return BackendRate / 2 >= ConnectionRate &&
		PeerBacklog * 1000 <= ConnectionRate * QueueMilliseconds &&
		GlobalBacklog * 1000 <= AggregateRate * QueueMilliseconds;
}

std::string_view ReliableServiceProfile::ValidationError() const {
	if (auto Error = ResourceError(*this); !Error.empty()) return Error;
	if (RequireLatencyCompatibility && !IsLatencyCompatible())
		return "Unqualified reliable rate/backlog profile: the full atomic-group envelope cannot fit the approved latency target";
	return {};
}
}

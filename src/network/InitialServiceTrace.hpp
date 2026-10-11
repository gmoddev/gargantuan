#pragma once
#include <cstdint>

namespace gargantuan { namespace network { namespace detail {
// Private opt-in value evidence. One initial native-positive packet per grant;
// neither these timestamps nor their presence participates in service policy.
struct InitialServiceTrace {
	bool Enabled = false, PendingAtEnqueue = false, PendingAtFirstSnpVisit = false;
	std::uint64_t Token = 0, MessageNumber = 0, NativeHandle = 0;
	std::uint64_t TransportEntered = 0, AdapterLocked = 0, NativeApiEntered = 0, NativeApiLocked = 0;
	std::uint64_t NativeEnqueued = 0, FirstSnpVisit = 0, PacketPreparationEntered = 0;
	std::uint64_t WorkerQueued = 0, WorkerStarted = 0, NativeCompleted = 0, ActorReconciled = 0;
	std::uint64_t DispatchSequence = 0;
};
}}}

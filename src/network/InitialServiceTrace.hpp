#pragma once
#include <cstdint>

namespace gargantuan { namespace network { namespace detail {
// Private opt-in value evidence. One initial native-positive packet per grant;
// neither these timestamps nor their presence participates in service policy.
struct InitialServiceTrace {
	bool Enabled = false, PendingAtEnqueue = false, PendingAtFirstSnpVisit = false;
	bool PriorNativePositive = false;
	std::uint8_t EligibilityMode = 0; // 1=future data deadline, 2=ready/ASAP request.
	std::uint64_t Token = 0, MessageNumber = 0, NativeHandle = 0;
	std::uint64_t TransportEntered = 0, AdapterLocked = 0, NativeApiEntered = 0, NativeApiLocked = 0;
	std::uint64_t NativeEnqueued = 0, FirstSnpVisit = 0, PacketPreparationEntered = 0;
	std::uint64_t EligibilityRecorded = 0;
	std::int64_t EligibilityNativeSample = 0, RequestedDataThinkNative = 0;
	std::uint64_t PriorWorkerQueued = 0, PriorWorkerStarted = 0, PriorNativeCompleted = 0, PriorActorReconciled = 0;
	std::uint64_t WorkerQueued = 0, WorkerStarted = 0, NativeCompleted = 0, ActorReconciled = 0;
	std::uint64_t DispatchSequence = 0;
};
}}}

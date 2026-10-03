#pragma once
#include "gargantuan/network/RemoteManager.hpp"
#include <cstdint>
#include <ostream>
#include <string_view>

namespace gargantuan::host::detail {
inline bool ValidRemoteOwnership(const network::RemoteMetrics &Value) {
	const auto &M = Value.Ownership;
	return Value.Observed && !Value.QueuedDispatchMessages && !Value.QueuedDispatchBytes &&
		!Value.DeferredReliableMessages && !Value.DeferredReliableBytes && !Value.InFlightRequests && !Value.IncomingHandlers &&
		!M.BoundViolations && M.DispatchAccepted == M.DispatchReleased && M.DeferredAccepted == M.DeferredReleased &&
		M.HandlersStarted == M.HandlersReleased && Value.RequestsStarted == Value.RequestsCompleted &&
		M.DispatchMessagesHigh <= network::MaximumQueuedRemoteDispatchMessages && M.DispatchBytesHigh <= network::MaximumQueuedRemoteDispatchBytes &&
		M.DeferredMessagesHigh <= network::MaximumQueuedRemoteDispatchMessages && M.DeferredBytesHigh <= network::MaximumQueuedRemoteDispatchBytes &&
		M.OutgoingRequestsHigh <= network::MaximumRemoteInFlightRequestsPerManager &&
		M.IncomingHandlersHigh <= network::MaximumConcurrentRemoteHandlersPerManager &&
		M.PeerIncomingHandlersHigh <= network::MaximumConcurrentRemoteHandlersPerPeer;
}
inline void WriteFarmRemoteOwnership(std::ostream &Out, std::string_view Run, std::string_view Role,
	int Slot, std::uint64_t Nonce, const network::RemoteMetrics &V) {
	const auto &M = V.Ownership;
	Out << "[Qualification:RemoteOwnership] event=post_stop contract=remote_ownership_v1 run_id=" << Run
		<< " role=" << Role << " slot=" << Slot << " nonce=" << Nonce << " observed=" << V.Observed
		<< " dispatch_current=" << V.QueuedDispatchMessages << " dispatch_bytes_current=" << V.QueuedDispatchBytes
		<< " deferred_current=" << V.DeferredReliableMessages << " deferred_bytes_current=" << V.DeferredReliableBytes
		<< " outgoing_current=" << V.InFlightRequests << " handlers_current=" << V.IncomingHandlers
		<< " dispatch_high=" << M.DispatchMessagesHigh << " dispatch_bytes_high=" << M.DispatchBytesHigh
		<< " deferred_high=" << M.DeferredMessagesHigh << " deferred_bytes_high=" << M.DeferredBytesHigh
		<< " outgoing_high=" << M.OutgoingRequestsHigh << " handlers_high=" << M.IncomingHandlersHigh
		<< " peer_dispatch_high=" << M.PeerDispatchMessagesHigh << " peer_dispatch_bytes_high=" << M.PeerDispatchBytesHigh
		<< " peer_deferred_bytes_high=" << M.PeerDeferredBytesHigh << " peer_outgoing_high=" << M.PeerOutgoingRequestsHigh
		<< " peer_handlers_high=" << M.PeerIncomingHandlersHigh << " dispatch_accepted=" << M.DispatchAccepted
		<< " dispatch_released=" << M.DispatchReleased << " deferred_accepted=" << M.DeferredAccepted
		<< " deferred_released=" << M.DeferredReleased << " handlers_started=" << M.HandlersStarted
		<< " handlers_released=" << M.HandlersReleased << " handlers_expired=" << M.HandlersExpired
		<< " deferred_expired=" << M.DeferredExpired << " requests_started=" << V.RequestsStarted
		<< " requests_completed=" << V.RequestsCompleted << " requests_timed_out=" << V.RequestsTimedOut
		<< " requests_cancelled=" << V.RequestsCancelled << " handler_errors=" << V.HandlerErrors
		<< " resource_rejections=" << V.ResourceRejections << " bound_violations=" << M.BoundViolations
		<< " dispatch_residence_us=" << M.DispatchResidenceMaximumMicroseconds
		<< " deferred_residence_us=" << M.DeferredResidenceMaximumMicroseconds
		<< " outgoing_residence_us=" << M.OutgoingResidenceMaximumMicroseconds
		<< " handler_residence_us=" << M.HandlerResidenceMaximumMicroseconds
		<< " deadline_overshoot_us=" << M.DeadlineOvershootMaximumMicroseconds
		<< " valid=" << ValidRemoteOwnership(V) << '\n';
}
}

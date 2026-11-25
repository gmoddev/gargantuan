#include "gargantuan/network/RemoteManager.hpp"
#include "RemoteValueBudget.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"
#include "../runtime/PublicationLatencyDiagnostics.hpp"

#include "gargantuan/classes/Instance.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gargantuan::network {
	std::string_view GetRemoteSendStatusName(RemoteSendStatus Status) {
		switch (Status) {
		case RemoteSendStatus::Accepted:
			return "accepted";
		case RemoteSendStatus::DeferredForMaterialization:
			return "waiting for Object publication";
		case RemoteSendStatus::DroppedUnreliable:
			return "dropped by unreliable congestion policy";
		case RemoteSendStatus::InvalidPeer:
			return "peer is disconnected or invalid";
		case RemoteSendStatus::UnknownRemote:
			return "Remote is unknown or destroyed";
		case RemoteSendStatus::UnpublishedRemote:
			return "Remote is not published to the peer";
		case RemoteSendStatus::WrongRemoteKind:
			return "operation is invalid for this Remote class";
		case RemoteSendStatus::InvalidArguments:
			return "arguments are unsupported or exceed bounded payload limits";
		case RemoteSendStatus::InvisibleReference:
			return "Object reference is not visible to the peer";
		case RemoteSendStatus::ReferenceNotMaterialized:
			return "Object reference is not materialized for unreliable delivery";
		case RemoteSendStatus::RateLimited:
			return "per-peer or per-Remote call rate exceeded";
		case RemoteSendStatus::RequestLimitExceeded:
			return "in-flight request limit exceeded";
		case RemoteSendStatus::SchedulerRejected:
			return "scheduler queue or negotiated message limit rejected the send";
		}
		return "unknown Remote rejection";
	}

	std::string_view GetRemoteSendStatusCode(RemoteSendStatus Status) {
		switch (Status) {
			case RemoteSendStatus::Accepted: return "accepted";
			case RemoteSendStatus::DeferredForMaterialization: return "waiting_for_publication";
			case RemoteSendStatus::DroppedUnreliable: return "dropped_unreliable";
			case RemoteSendStatus::InvalidPeer: return "invalid_peer";
			case RemoteSendStatus::UnknownRemote: return "unknown_remote";
			case RemoteSendStatus::UnpublishedRemote: return "unpublished_remote";
			case RemoteSendStatus::WrongRemoteKind: return "wrong_remote_kind";
			case RemoteSendStatus::InvalidArguments: return "invalid_arguments";
			case RemoteSendStatus::InvisibleReference: return "invisible_reference";
			case RemoteSendStatus::ReferenceNotMaterialized: return "reference_not_materialized";
			case RemoteSendStatus::RateLimited: return "rate_limited";
			case RemoteSendStatus::RequestLimitExceeded: return "request_limit_exceeded";
			case RemoteSendStatus::SchedulerRejected: return "scheduler_rejected";
		}
		return "scheduler_rejected";
	}

	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter) {
			if (Counter != std::numeric_limits<Value>::max()) ++Counter;
		}

		StateChannelId RemoteStateChannel(ObjectId Remote) {
			return StateChannelId((static_cast<std::uint64_t>(Remote.Generation) << 32) | Remote.Slot);
		}

		bool IsCall(RemoteMessageKind Kind) {
			return Kind == RemoteMessageKind::ReliableEvent || Kind == RemoteMessageKind::UnreliableEvent ||
				   Kind == RemoteMessageKind::SequencedEvent || Kind == RemoteMessageKind::Request;
		}

		std::optional<ObjectId> DeferredOrderingStream(const RemoteMessage &Message) {
			// Request IDs are shared across all functions in one peer session.
			if (Message.Kind == RemoteMessageKind::Request) return ObjectId{};
			if (Message.Kind == RemoteMessageKind::ReliableEvent) return Message.Remote;
			return std::nullopt;
		}

		DeliveryMode DeliveryFor(RemoteMessageKind Kind) {
			if (Kind == RemoteMessageKind::UnreliableEvent) return DeliveryMode::UnreliableUnordered;
			if (Kind == RemoteMessageKind::SequencedEvent) return DeliveryMode::UnreliableSequenced;
			return DeliveryMode::ReliableOrdered;
		}

		TrafficClass TrafficFor(RemoteMessageKind Kind) {
			if (Kind == RemoteMessageKind::UnreliableEvent) return TrafficClass::EphemeralApplication;
			if (Kind == RemoteMessageKind::SequencedEvent) return TrafficClass::RealtimeState;
			return TrafficClass::ReliableApplication;
		}

		MessageOrder OrderFor(const RemoteMessage &Message) {
			if (Message.Kind != RemoteMessageKind::SequencedEvent) return {};
			return RemoteEventOrder{RemoteStateChannel(Message.Remote), Message.Publication, Message.Sequence};
		}

		RemoteRequestResult TerminalResult(
			RemoteRequestId Request,
			RemoteRequestTerminalStatus Status,
			std::vector<RemoteValue> Results = {},
			std::optional<StructuredRemoteError> Error = std::nullopt
		) {
			return {
				.Outcome = {.Request = Request, .Status = Status, .Error = std::move(Error)},
				.Results = std::move(Results),
			};
		}
	}

	struct RemoteManager::Implementation : std::enable_shared_from_this<Implementation> {
		struct QueuedMessage {
			ConnectionId Connection;
			RemoteMessage Message;
			std::size_t EncodedBytes = 0;
			std::size_t ArgumentBytes = 0;
			std::chrono::steady_clock::time_point StartedAt;
		};

		struct PeerState {
			ReplicationEpoch Epoch;
			NetworkLimits Limits;
			std::unordered_set<ObjectId> PublishedRemotes;
			std::unordered_set<ObjectId> MaterializedObjects;
			std::unordered_map<ObjectId, RemotePublicationId> Publications;
			std::unordered_map<ObjectId, RemoteEventSequence> NextOutgoingSequence;
			std::unordered_map<ObjectId, RemoteEventSequence> LatestIncomingSequence;
			RemoteRequestId NextRequest{1};
			RemoteRequestId LatestIncomingRequest;
			std::chrono::steady_clock::time_point RateWindow{};
			bool RateWindowInitialized = false;
			std::uint32_t TotalCalls = 0;
			std::unordered_map<ObjectId, std::uint32_t> CallsByRemote;
			std::uint32_t ConcurrentHandlers = 0;
			std::uint32_t PendingOutgoingRequests = 0;
			std::deque<QueuedMessage> DispatchQueue;
			std::optional<std::list<ConnectionId>::iterator> ReadyPosition;
			std::size_t EligibleDispatchMessages = 0;
			std::size_t QueuedDispatchBytes = 0;
			std::size_t QueuedDispatchArgumentBytes = 0;
			std::size_t DeferredReliableBytes = 0;
			std::size_t DeferredReliableArgumentBytes = 0;
			std::unordered_map<ObjectId, std::size_t> DeferredStreams;
		};

		struct PendingKey {
			ConnectionId Connection;
			RemoteRequestId Request;
			auto operator<=>(const PendingKey &) const = default;
		};

		struct PendingRequest {
			ObjectId Remote;
			std::chrono::steady_clock::time_point Deadline;
			RequestCompletion Completion;
			std::chrono::steady_clock::time_point StartedAt;
		};

		struct IncomingRequest {
			ObjectId Remote;
			std::chrono::steady_clock::time_point ReplyDeadline;
			std::chrono::steady_clock::time_point WorkDeadline;
			bool AcceptReply = true;
			std::chrono::steady_clock::time_point StartedAt;
		};

		struct DeferredMessage {
			ConnectionId Connection;
			RemoteMessage Message;
			std::vector<std::byte> Encoded;
			std::vector<ObjectId> Dependencies;
			std::size_t ArgumentBytes = 0;
			std::chrono::steady_clock::time_point Deadline;
			std::chrono::steady_clock::time_point StartedAt;
		};

		void ObserveOwnership(const PeerState &Peer) {
			auto &M = Metrics.Ownership;
			M.DispatchMessagesHigh = std::max<std::uint64_t>(M.DispatchMessagesHigh, Metrics.QueuedDispatchMessages);
			M.DispatchBytesHigh = std::max<std::uint64_t>(M.DispatchBytesHigh, Metrics.QueuedDispatchBytes);
			M.DeferredMessagesHigh = std::max<std::uint64_t>(M.DeferredMessagesHigh, Deferred.size());
			M.DeferredBytesHigh = std::max<std::uint64_t>(M.DeferredBytesHigh, Metrics.DeferredReliableBytes);
			M.OutgoingRequestsHigh = std::max<std::uint64_t>(M.OutgoingRequestsHigh, PendingRequests.size());
			M.IncomingHandlersHigh = std::max<std::uint64_t>(M.IncomingHandlersHigh, IncomingRequests.size());
			M.PeerDispatchMessagesHigh = std::max<std::uint64_t>(M.PeerDispatchMessagesHigh, Peer.DispatchQueue.size());
			M.PeerDispatchBytesHigh = std::max<std::uint64_t>(M.PeerDispatchBytesHigh, Peer.QueuedDispatchBytes);
			M.PeerDeferredBytesHigh = std::max<std::uint64_t>(M.PeerDeferredBytesHigh, Peer.DeferredReliableBytes);
			M.PeerOutgoingRequestsHigh = std::max<std::uint64_t>(M.PeerOutgoingRequestsHigh, Peer.PendingOutgoingRequests);
			M.PeerIncomingHandlersHigh = std::max<std::uint64_t>(M.PeerIncomingHandlersHigh, Peer.ConcurrentHandlers);
			if (Metrics.QueuedDispatchMessages > MaximumQueuedRemoteDispatchMessages || Metrics.QueuedDispatchBytes > MaximumQueuedRemoteDispatchBytes ||
				Deferred.size() > MaximumQueuedRemoteDispatchMessages || Metrics.DeferredReliableBytes > MaximumQueuedRemoteDispatchBytes ||
				PendingRequests.size() > MaximumRemoteInFlightRequestsPerManager || IncomingRequests.size() > MaximumConcurrentRemoteHandlersPerManager ||
				Peer.DispatchQueue.size() > Peer.Limits.MaximumMessagesPerTick || Peer.QueuedDispatchBytes > Peer.Limits.MaximumReceiveBytesPerTick ||
				Peer.DeferredReliableBytes > Peer.Limits.MaximumQueuedReliableBytes || Peer.PendingOutgoingRequests > Peer.Limits.MaximumInFlightRemoteRequests ||
				Peer.ConcurrentHandlers > MaximumConcurrentRemoteHandlersPerPeer || Peer.ConcurrentHandlers > Peer.Limits.MaximumInFlightRemoteRequests)
				SaturatingIncrement(M.BoundViolations);
		}
		void ObserveResidence(std::chrono::steady_clock::time_point Started, std::uint64_t &Maximum) {
			// Destructor cleanup must not introduce a new invocation of an
			// externally supplied Clock after the manager stops accepting work.
			if (!Active) return;
			const auto Now = GetTime();
			if (Now >= Started) Maximum = std::max(Maximum, static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(Now - Started).count()));
		}
		void QueueDispatch(PeerState &Peer, QueuedMessage Message) {
			const bool WasEmpty = Peer.DispatchQueue.empty();
			if (WasEmpty) Peer.ReadyPosition = ReadyPeers.insert(ReadyPeers.end(), Message.Connection);
			try {
				Peer.DispatchQueue.push_back(std::move(Message));
			} catch (...) {
				if (WasEmpty) ClearReadyPosition(Peer);
				throw;
			}
			const auto &Queued = Peer.DispatchQueue.back();
			++Metrics.QueuedDispatchMessages;
			Metrics.QueuedDispatchBytes += Queued.EncodedBytes;
			Metrics.QueuedDispatchArgumentBytes += Queued.ArgumentBytes;
			Peer.QueuedDispatchBytes += Queued.EncodedBytes;
			Peer.QueuedDispatchArgumentBytes += Queued.ArgumentBytes;
			SaturatingIncrement(Metrics.Ownership.DispatchAccepted);
			ObserveOwnership(Peer);
		}
		void ClearReadyPosition(PeerState &Peer) {
			if (!Peer.ReadyPosition) return;
			ReadyPeers.erase(*Peer.ReadyPosition);
			Peer.ReadyPosition.reset();
		}
		void ReleaseDispatch(PeerState &Peer, const QueuedMessage &Message) {
			--Metrics.QueuedDispatchMessages;
			Metrics.QueuedDispatchBytes -= Message.EncodedBytes;
			Metrics.QueuedDispatchArgumentBytes -= Message.ArgumentBytes;
			Peer.QueuedDispatchBytes -= Message.EncodedBytes;
			Peer.QueuedDispatchArgumentBytes -= Message.ArgumentBytes;
			SaturatingIncrement(Metrics.Ownership.DispatchReleased);
			ObserveResidence(Message.StartedAt, Metrics.Ownership.DispatchResidenceMaximumMicroseconds);
		}
		void RetireDispatch(PeerState &Peer, std::optional<ObjectId> Remote = std::nullopt) {
			std::size_t Index = 0;
			std::size_t RemovedEligible = 0;
			std::erase_if(Peer.DispatchQueue, [&](const QueuedMessage &Message) {
				const bool Eligible = Index++ < Peer.EligibleDispatchMessages;
				if (Remote && Message.Message.Remote != *Remote) return false;
				RemovedEligible += Eligible;
				ReleaseDispatch(Peer, Message);
				return true;
			});
			Peer.EligibleDispatchMessages -= RemovedEligible;
			if (Peer.DispatchQueue.empty()) ClearReadyPosition(Peer);
		}
		void ReleaseDeferred(const DeferredMessage &Message) {
			--Metrics.DeferredReliableMessages;
			Metrics.DeferredReliableBytes -= Message.Encoded.size();
			Metrics.DeferredReliableArgumentBytes -= Message.ArgumentBytes;
			if (auto Peer = Peers.find(Message.Connection); Peer != Peers.end()) {
				Peer->second.DeferredReliableBytes -= Message.Encoded.size();
				Peer->second.DeferredReliableArgumentBytes -= Message.ArgumentBytes;
				if (const auto Stream = DeferredOrderingStream(Message.Message)) {
					auto Count = Peer->second.DeferredStreams.find(*Stream);
					if (--Count->second == 0) Peer->second.DeferredStreams.erase(Count);
				}
			}
			SaturatingIncrement(Metrics.Ownership.DeferredReleased);
			ObserveResidence(Message.StartedAt, Metrics.Ownership.DeferredResidenceMaximumMicroseconds);
			if (Active && GetTime() >= Message.Deadline) {
				SaturatingIncrement(Metrics.Ownership.DeferredExpired);
				ObserveResidence(Message.Deadline, Metrics.Ownership.DeadlineOvershootMaximumMicroseconds);
			}
		}
		void ReleaseHandlerEvidence(const IncomingRequest &Request) {
			SaturatingIncrement(Metrics.Ownership.HandlersReleased);
			ObserveResidence(Request.StartedAt, Metrics.Ownership.HandlerResidenceMaximumMicroseconds);
			if (Active && GetTime() >= Request.WorkDeadline) {
				SaturatingIncrement(Metrics.Ownership.HandlersExpired);
				ObserveResidence(Request.WorkDeadline, Metrics.Ownership.DeadlineOvershootMaximumMicroseconds);
			}
		}

		struct PendingCompletion {
			RequestCompletion Completion;
			RemoteRequestResult Result;
		};

		Implementation(
			RemoteManagerRole Role,
			INetworkScheduler &Scheduler,
			VisibilityCheck IsVisible,
			ObjectResolver ResolveObject,
			Clock GetTime
		)
			: Role(Role), Scheduler(Scheduler), IsVisible(std::move(IsVisible)),
			  ResolveObject(std::move(ResolveObject)),
			  GetTime(GetTime ? std::move(GetTime) : [] { return std::chrono::steady_clock::now(); }) {}

		bool ArgumentsValid(
			ConnectionId Connection,
			std::span<const RemoteValue> Arguments,
			bool RequireMaterialized,
			std::vector<ObjectId> *Missing = nullptr
		) {
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return false;
			const auto CheckReference = [&](const RemoteValue &Argument) {
				const auto *Reference = std::get_if<WireObjectReference>(&Argument);
				if (!Reference) return true;
				const auto Object = Reference->Object.ToObjectId();
				const auto InstanceValue = Object.IsValid() && IsVisible && IsVisible(Connection, Object) &&
					ResolveObject ? ResolveObject(Object) : nullptr;
				if (!InstanceValue || InstanceValue->IsDestroying() || InstanceValue->GetDestroyed()) {
					SaturatingIncrement(Metrics.VisibilityRejections);
					return false;
				}
				if (RequireMaterialized && !Peer->second.MaterializedObjects.contains(Object)) {
					if (Missing)
						Missing->push_back(Object);
					else
						return false;
				}
				return true;
			};
			for (const auto &Argument : Arguments)
				if (!detail::VisitRemoteValues(Argument, CheckReference)) return false;
			return true;
		}

		bool RemoteAvailable(
			ConnectionId Connection,
			ObjectId Remote,
			RemoteInstanceKind *Kind = nullptr,
			RemotePublicationId *Publication = nullptr
		) {
			auto Peer = Peers.find(Connection);
			auto Registered = Remotes.find(Remote);
			if (Peer == Peers.end() || Registered == Remotes.end()) return false;
			if (!Peer->second.PublishedRemotes.contains(Remote) || !Peer->second.MaterializedObjects.contains(Remote))
				return false;
			if (!IsVisible || !IsVisible(Connection, Remote)) return false;
			if (Kind) *Kind = Registered->second;
			if (Publication) {
				auto Current = Peer->second.Publications.find(Remote);
				if (Current == Peer->second.Publications.end() || !Current->second.IsValid()) return false;
				*Publication = Current->second;
			}
			return true;
		}

		bool AdmitManagerCall() {
			const auto Now = GetTime();
			if (!ManagerRateWindowInitialized || Now < ManagerRateWindow ||
				Now - ManagerRateWindow >= std::chrono::seconds(1)) {
				ManagerRateWindow = Now;
				ManagerRateWindowInitialized = true;
				ManagerCalls = 0;
			}
			if (ManagerCalls >= MaximumRemoteCallsPerManagerPerSecond) {
				SaturatingIncrement(Metrics.RateLimitRejections);
				return false;
			}
			++ManagerCalls;
			return true;
		}

		bool AdmitPeerCall(PeerState &Peer, ObjectId Remote) {
			const auto Now = GetTime();
			if (!Peer.RateWindowInitialized || Now < Peer.RateWindow ||
				Now - Peer.RateWindow >= std::chrono::seconds(1)) {
				Peer.RateWindow = Now;
				Peer.RateWindowInitialized = true;
				Peer.TotalCalls = 0;
				Peer.CallsByRemote.clear();
			}
			auto &RemoteCalls = Peer.CallsByRemote[Remote];
			if (Peer.TotalCalls >= MaximumRemoteCallsPerPeerPerSecond ||
				RemoteCalls >= MaximumRemoteCallsPerRemotePerSecond) {
				SaturatingIncrement(Metrics.RateLimitRejections);
				return false;
			}
			++Peer.TotalCalls;
			++RemoteCalls;
			return true;
		}

		bool AdmitCall(PeerState &Peer, ObjectId Remote) {
			return AdmitManagerCall() && AdmitPeerCall(Peer, Remote);
		}

		bool AdmitGeneratedReliable(std::size_t Bytes) {
			const auto Now = GetTime();
			if (!GeneratedRateWindowInitialized || Now < GeneratedRateWindow ||
				Now - GeneratedRateWindow >= std::chrono::seconds(1)) {
				GeneratedRateWindow = Now;
				GeneratedRateWindowInitialized = true;
				GeneratedReliableMessages = 0;
				GeneratedReliableBytes = 0;
			}
			if (GeneratedReliableMessages >= MaximumRemoteGeneratedReliableMessagesPerSecond ||
				Bytes > MaximumRemoteGeneratedReliableBytesPerSecond - GeneratedReliableBytes) {
				SaturatingIncrement(Metrics.ResourceRejections);
				return false;
			}
			++GeneratedReliableMessages;
			GeneratedReliableBytes += Bytes;
			return true;
		}

		RemoteSendResult SubmitEncoded(
			ConnectionId Connection,
			const RemoteMessage &Message,
			std::vector<std::byte> Encoded,
			bool AllowDependencyDeferral
		) {
			const auto Producer = Message.Kind == RemoteMessageKind::Request ? runtime_detail::WorkProducer::RpcRequest :
				Message.Kind == RemoteMessageKind::Response || Message.Kind == RemoteMessageKind::RequestError ? runtime_detail::WorkProducer::RpcResponse :
				Message.Kind == RemoteMessageKind::ReliableEvent ? runtime_detail::WorkProducer::RemoteEventReliable :
				Message.Kind == RemoteMessageKind::UnreliableEvent || Message.Kind == RemoteMessageKind::SequencedEvent ? runtime_detail::WorkProducer::RemoteEventUnreliable :
				runtime_detail::WorkProducer::RemoteControl;
			runtime_detail::WorkProducerScope ProducerScope(Producer);
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return {RemoteSendStatus::InvalidPeer};
			std::vector<ObjectId> Missing;
			if (!ArgumentsValid(Connection, Message.Arguments, true, &Missing))
				return {RemoteSendStatus::InvisibleReference};
			const auto Stream = DeferredOrderingStream(Message);
			const bool WaitingForPredecessor = AllowDependencyDeferral && Stream &&
				Peer->second.DeferredStreams.contains(*Stream);
			if (!Missing.empty() || WaitingForPredecessor) {
				if (!AllowDependencyDeferral || DeliveryFor(Message.Kind) != DeliveryMode::ReliableOrdered)
					return {RemoteSendStatus::ReferenceNotMaterialized};
				const auto ArgumentBytes = GetRemoteArgumentStorageBytes(Message.Arguments);
				if (!ArgumentBytes || Deferred.size() >= MaximumQueuedRemoteDispatchMessages ||
					Encoded.size() > Peer->second.Limits.MaximumReliableMessageBytes ||
					Encoded.size() > MaximumQueuedRemoteDispatchBytes ||
					Metrics.DeferredReliableBytes > MaximumQueuedRemoteDispatchBytes - Encoded.size() ||
					Encoded.size() > Peer->second.Limits.MaximumQueuedReliableBytes ||
					Peer->second.DeferredReliableBytes >
						Peer->second.Limits.MaximumQueuedReliableBytes - Encoded.size() ||
					*ArgumentBytes > MaximumRetainedRemoteArgumentBytes - Metrics.DeferredReliableArgumentBytes ||
					*ArgumentBytes > Peer->second.Limits.MaximumQueuedReliableBytes * 4 -
						Peer->second.DeferredReliableArgumentBytes)
					return {RemoteSendStatus::SchedulerRejected};
				const auto Started = GetTime();
				auto &Streams = Peer->second.DeferredStreams;
				auto Entry = Stream ? Streams.try_emplace(*Stream).first : Streams.end();
				try {
					Deferred.push_back({
						Connection, Message, std::move(Encoded), std::move(Missing), *ArgumentBytes,
						Started + (Message.Kind == RemoteMessageKind::Request ? Message.Deadline : MaximumRemoteRequestDeadline),
						Started
					});
				} catch (...) {
					if (Entry != Streams.end() && Entry->second == 0) Streams.erase(Entry);
					throw;
				}
				if (Entry != Streams.end()) ++Entry->second;
				++Metrics.DeferredReliableMessages;
				Metrics.DeferredReliableBytes += Deferred.back().Encoded.size();
				Peer->second.DeferredReliableBytes += Deferred.back().Encoded.size();
				Metrics.DeferredReliableArgumentBytes += *ArgumentBytes;
				Peer->second.DeferredReliableArgumentBytes += *ArgumentBytes;
				SaturatingIncrement(Metrics.Ownership.DeferredAccepted);
				ObserveOwnership(Peer->second);
				return {RemoteSendStatus::DeferredForMaterialization};
			}
			const auto Delivery = DeliveryFor(Message.Kind);
			auto Intent = MakeNetworkMessageIntent(
				Connection,
				Delivery,
				TrafficFor(Message.Kind),
				OrderFor(Message),
				std::move(Encoded),
				Peer->second.Limits
			);
			if (!Intent) return {RemoteSendStatus::SchedulerRejected};
			auto Submitted = Scheduler.Submit(std::move(*Intent));
			if (Submitted.Status == SchedulerSubmitStatus::DroppedUnreliable)
				return {RemoteSendStatus::DroppedUnreliable};
			if (!Submitted.Accepted()) {
				auto Terminal = std::move(Submitted.TerminalDisconnect);
				if (Delivery == DeliveryMode::ReliableOrdered && !Terminal)
					Terminal = DisconnectInfo{
						DisconnectReason::ResourceExhaustion,
						"Reliable Remote message was rejected by the scheduler",
					};
				if (Terminal) TerminalConnections.try_emplace(Connection, *Terminal);
				return {
					.Status = RemoteSendStatus::SchedulerRejected,
					.TerminalDisconnect = std::move(Terminal),
				};
			}
			if (Submitted.Status == SchedulerSubmitStatus::AcceptedWithSupersession)
				SaturatingIncrement(Metrics.SequencedEventsSuperseded);
			return {RemoteSendStatus::Accepted};
		}

		RemoteSendResult
		SendMessage(ConnectionId Connection, RemoteMessage Message, bool AllowDependencyDeferral = true) {
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return {RemoteSendStatus::InvalidPeer};
			auto Publication = Peer->second.Publications.find(Message.Remote);
			if (Publication == Peer->second.Publications.end() || !Publication->second.IsValid())
				return {RemoteSendStatus::UnpublishedRemote};
			Message.Publication = Publication->second;
			auto Encoded = EncodeRemoteMessage(Message);
			if (!Encoded) return {RemoteSendStatus::InvalidArguments};
			if ((Message.Kind == RemoteMessageKind::Response || Message.Kind == RemoteMessageKind::RequestError ||
				 Message.Kind == RemoteMessageKind::Cancellation) &&
				!AdmitGeneratedReliable(Encoded->size()))
				return {RemoteSendStatus::RateLimited};
			return SubmitEncoded(Connection, Message, std::move(*Encoded), AllowDependencyDeferral);
		}

		std::optional<PendingCompletion> TakePending(PendingKey Key, RemoteRequestResult Result) {
			auto Pending = PendingRequests.find(Key);
			if (Pending == PendingRequests.end()) return std::nullopt;
			auto Completion = std::move(Pending->second.Completion);
			for (auto Iterator = Deferred.begin(); Iterator != Deferred.end();) {
				if (Iterator->Connection != Key.Connection || Iterator->Message.Kind != RemoteMessageKind::Request ||
					Iterator->Message.Request != Key.Request) {
					++Iterator;
					continue;
				}
				ReleaseDeferred(*Iterator);
				Iterator = Deferred.erase(Iterator);
			}
			if (auto Peer = Peers.find(Key.Connection);
				Peer != Peers.end() && Peer->second.PendingOutgoingRequests != 0)
				--Peer->second.PendingOutgoingRequests;
			ObserveResidence(Pending->second.StartedAt, Metrics.Ownership.OutgoingResidenceMaximumMicroseconds);
			if (Result.Outcome.Status == RemoteRequestTerminalStatus::Timeout)
				ObserveResidence(Pending->second.Deadline, Metrics.Ownership.DeadlineOvershootMaximumMicroseconds);
			PendingRequests.erase(Pending);
			Metrics.InFlightRequests = PendingRequests.size();
			SaturatingIncrement(Metrics.RequestsCompleted);
			if (Result.Outcome.Status == RemoteRequestTerminalStatus::Timeout)
				SaturatingIncrement(Metrics.RequestsTimedOut);
			if (Result.Outcome.Status == RemoteRequestTerminalStatus::Cancelled)
				SaturatingIncrement(Metrics.RequestsCancelled);
			return PendingCompletion{std::move(Completion), std::move(Result)};
		}

		void CompletePending(PendingKey Key, RemoteRequestResult Result) {
			auto Pending = TakePending(Key, std::move(Result));
			if (Pending && Pending->Completion) Pending->Completion(std::move(Pending->Result));
		}

		static void RunCompletions(std::vector<PendingCompletion> Completions) {
			for (auto &Pending : Completions)
				if (Pending.Completion) Pending.Completion(std::move(Pending.Result));
		}

		bool RemovePeerState(ConnectionId Connection) {
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return false;
			std::vector<PendingCompletion> Completions;
			std::vector<PendingKey> PendingKeys;
			for (const auto &[Key, Pending] : PendingRequests)
				if (Key.Connection == Connection) PendingKeys.push_back(Key);
			for (const auto &Key : PendingKeys) {
				auto Pending = TakePending(
					Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::Disconnected)
				);
				if (Pending) Completions.push_back(std::move(*Pending));
			}
			std::erase_if(IncomingRequests, [&](const auto &Entry) {
				if (Entry.first.Connection != Connection) return false;
				ReleaseHandlerEvidence(Entry.second); return true;
			});
			RetireDispatch(Peer->second);
			for (auto Iterator = Deferred.begin(); Iterator != Deferred.end();) {
				if (Iterator->Connection != Connection) {
					++Iterator;
					continue;
				}
				ReleaseDeferred(*Iterator);
				Iterator = Deferred.erase(Iterator);
			}
			Peers.erase(Peer);
			RunCompletions(std::move(Completions));
			return true;
		}

		void DrainTerminals() {
			while (!TerminalConnections.empty()) {
				auto Terminals = std::move(TerminalConnections);
				TerminalConnections.clear();
				for (const auto &[Connection, Information] : Terminals)
					(void)RemovePeerState(Connection);
				for (auto &[Connection, Information] : Terminals)
					if (OnTerminal) OnTerminal(Connection, Information);
			}
		}

		bool SendReply(
			PendingKey Key, ObjectId Remote, std::vector<RemoteValue> Results, std::optional<StructuredRemoteError> Error
		) {
			auto Incoming = IncomingRequests.find(Key);
			if (Incoming == IncomingRequests.end() || Incoming->second.Remote != Remote) return false;
			auto Peer = Peers.find(Key.Connection);
			if (Peer == Peers.end()) return false;
			const bool CanReply = Incoming->second.AcceptReply && GetTime() < Incoming->second.ReplyDeadline &&
								  RemoteAvailable(Key.Connection, Remote);
			ReleaseHandlerEvidence(Incoming->second);
			IncomingRequests.erase(Incoming);
			if (Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
			if (!CanReply) return false;
			RemoteMessage Message{
				.Kind = Error ? RemoteMessageKind::RequestError : RemoteMessageKind::Response,
				.Remote = Remote,
				.Request = Key.Request,
				.Arguments = Error ? std::vector<RemoteValue>{} : std::move(Results),
				.Error = std::move(Error),
			};
			if (!Message.IsValid() || !ArgumentsValid(Key.Connection, Message.Arguments, false)) {
				Message = {
					.Kind = RemoteMessageKind::RequestError,
					.Remote = Remote,
					.Request = Key.Request,
					.Error = StructuredRemoteError{"invalid_result", "Remote handler returned unsupported values"},
				};
			}
			runtime_detail::RecordPublicationLatency({.Stage = "RpcResponseProduced", .Connection = Key.Connection,
				.Object = Remote, .Sequence = Key.Request.Value(), .Kind = 104});
			const auto Accepted = SendMessage(Key.Connection, std::move(Message)).Accepted();
			if (Accepted)
				runtime_detail::RecordPublicationLatency({.Stage = "RpcResponseSchedulerAccepted",
					.Connection = Key.Connection, .Object = Remote, .Sequence = Key.Request.Value()});
			return Accepted;
		}

		void RejectRequest(ConnectionId Connection, const RemoteMessage &Message, std::string Code, std::string Text) {
			if (!Message.Request.IsValid()) return;
			RemoteMessage Response{
				.Kind = RemoteMessageKind::RequestError,
				.Remote = Message.Remote,
				.Request = Message.Request,
				.Error = StructuredRemoteError{std::move(Code), std::move(Text)},
			};
			(void)SendMessage(Connection, std::move(Response), false);
		}

		void Dispatch(QueuedMessage Queued) {
			auto Peer = Peers.find(Queued.Connection);
			if (Peer != Peers.end() && IsCall(Queued.Message.Kind) && !AdmitManagerCall()) return;
			auto Registered = Remotes.find(Queued.Message.Remote);
			RemotePublicationId Publication;
			if (Peer == Peers.end() || Registered == Remotes.end() ||
				!RemoteAvailable(Queued.Connection, Queued.Message.Remote, nullptr, &Publication) ||
				!IsRemoteMessageKindCompatible(Queued.Message.Kind, Registered->second)) {
				SaturatingIncrement(Metrics.VisibilityRejections);
				if (Queued.Message.Kind == RemoteMessageKind::Request)
					RejectRequest(Queued.Connection, Queued.Message, "remote_unavailable", "Remote is unavailable");
				return;
			}
			if (Queued.Message.Publication != Publication) {
				SaturatingIncrement(Metrics.ProtocolRejections);
				return;
			}
			if (!ArgumentsValid(Queued.Connection, Queued.Message.Arguments, true)) {
				if (Queued.Message.Kind == RemoteMessageKind::Request)
					RejectRequest(
						Queued.Connection, Queued.Message, "invalid_arguments", "Remote arguments were rejected"
					);
				else if (Queued.Message.Kind == RemoteMessageKind::Response) {
					const PendingKey Key{Queued.Connection, Queued.Message.Request};
					const auto Pending = PendingRequests.find(Key);
					if (Pending != PendingRequests.end() && Pending->second.Remote == Queued.Message.Remote)
						CompletePending(Key, TerminalResult(Queued.Message.Request, RemoteRequestTerminalStatus::ProtocolRejected));
				}
				return;
			}
			if (IsCall(Queued.Message.Kind) && !AdmitPeerCall(Peer->second, Queued.Message.Remote)) {
				return;
			}

			if (Queued.Message.Kind == RemoteMessageKind::SequencedEvent) {
				auto &Latest = Peer->second.LatestIncomingSequence[Queued.Message.Remote];
				if (Latest.IsValid() && !Queued.Message.Sequence.IsNewerThan(Latest)) {
					SaturatingIncrement(Metrics.SequencedEventsStaleRejected);
					return;
				}
				Latest = Queued.Message.Sequence;
			}

			if (Queued.Message.Kind == RemoteMessageKind::ReliableEvent ||
				Queued.Message.Kind == RemoteMessageKind::UnreliableEvent ||
				Queued.Message.Kind == RemoteMessageKind::SequencedEvent) {
				auto Handler = EventHandlers.find(Queued.Message.Remote);
				if (Handler == EventHandlers.end() || !Handler->second) return;
				auto Callback = Handler->second;
				try {
					Callback(
						{{Queued.Connection, Peer->second.Epoch},
						 Queued.Message.Remote,
						 std::move(Queued.Message.Arguments),
						 Publication}
					);
				} catch (...) {
					SaturatingIncrement(Metrics.HandlerErrors);
				}
				return;
			}

			const PendingKey Key{Queued.Connection, Queued.Message.Request};
			if (Queued.Message.Kind == RemoteMessageKind::Response ||
				Queued.Message.Kind == RemoteMessageKind::RequestError) {
				auto Pending = PendingRequests.find(Key);
				if (Pending == PendingRequests.end() || Pending->second.Remote != Queued.Message.Remote) {
					SaturatingIncrement(Metrics.ProtocolRejections);
					return;
				}
				runtime_detail::RecordPublicationLatency({.Stage = "RpcCompletion", .Connection = Key.Connection,
					.Object = Queued.Message.Remote, .Sequence = Key.Request.Value(), .Kind = 104});
				if (Queued.Message.Kind == RemoteMessageKind::Response)
					CompletePending(
						Key,
						TerminalResult(
							Key.Request, RemoteRequestTerminalStatus::Success, std::move(Queued.Message.Arguments)
						)
					);
				else
					CompletePending(
						Key,
						TerminalResult(
							Key.Request, RemoteRequestTerminalStatus::RemoteError, {}, std::move(Queued.Message.Error)
						)
					);
				return;
			}
			if (Queued.Message.Kind == RemoteMessageKind::Cancellation) {
				auto Incoming = IncomingRequests.find(Key);
				if (Incoming != IncomingRequests.end() && Incoming->second.Remote == Queued.Message.Remote)
					Incoming->second.AcceptReply = false;
				return;
			}
			if (Peer->second.LatestIncomingRequest.IsValid() &&
				!Queued.Message.Request.IsNewerThan(Peer->second.LatestIncomingRequest)) {
				SaturatingIncrement(Metrics.ProtocolRejections);
				RejectRequest(Queued.Connection, Queued.Message, "replayed_request", "Request ID was already consumed");
				return;
			}
			Peer->second.LatestIncomingRequest = Queued.Message.Request;

			if (IncomingRequests.contains(Key)) {
				SaturatingIncrement(Metrics.ProtocolRejections);
				RejectRequest(Queued.Connection, Queued.Message, "duplicate_request", "Duplicate request ID");
				return;
			}
			if (IncomingRequests.size() >= MaximumConcurrentRemoteHandlersPerManager ||
				Peer->second.ConcurrentHandlers >= MaximumConcurrentRemoteHandlersPerPeer ||
				Peer->second.ConcurrentHandlers >= Peer->second.Limits.MaximumInFlightRemoteRequests) {
				SaturatingIncrement(Metrics.ResourceRejections);
				RejectRequest(Queued.Connection, Queued.Message, "request_limit", "Remote request limit exceeded");
				return;
			}
			auto Handler = RequestHandlers.find(Queued.Message.Remote);
			if (Handler == RequestHandlers.end() || !Handler->second) {
				RejectRequest(Queued.Connection, Queued.Message, "no_handler", "Remote request has no handler");
				return;
			}
			const auto StartedAt = GetTime();
			IncomingRequests.emplace(
				Key,
				IncomingRequest{
					Queued.Message.Remote,
					StartedAt + Queued.Message.Deadline,
					StartedAt + MaximumRemoteRequestDeadline,
					true, StartedAt,
				}
			);
			++Peer->second.ConcurrentHandlers;
			SaturatingIncrement(Metrics.Ownership.HandlersStarted);
			ObserveOwnership(Peer->second);
			auto Weak = weak_from_this();
			RequestReply Reply = [Weak, Key, Remote = Queued.Message.Remote](
									 std::vector<RemoteValue> Results, std::optional<StructuredRemoteError> Error
								 ) mutable {
				auto Self = Weak.lock();
				return Self && Self->Active && Self->SendReply(Key, Remote, std::move(Results), std::move(Error));
			};
			auto Callback = Handler->second;
			try {
				runtime_detail::RecordPublicationLatency({.Stage = "RpcHandler", .Connection = Key.Connection,
					.Object = Queued.Message.Remote, .Sequence = Key.Request.Value(), .Kind = 103});
				Callback(
					{{Queued.Connection, Peer->second.Epoch},
					 Queued.Message.Remote,
					 std::move(Queued.Message.Arguments),
					 Publication},
					std::move(Reply)
				);
			} catch (...) {
				SaturatingIncrement(Metrics.HandlerErrors);
				(void)SendReply(
					Key, Queued.Message.Remote, {}, StructuredRemoteError{"handler_error", "Remote handler failed"}
				);
			}
		}

		RemoteManagerRole Role;
		INetworkScheduler &Scheduler;
		VisibilityCheck IsVisible;
		ObjectResolver ResolveObject;
		OutgoingReferenceMapper MapOutgoingReference;
		Clock GetTime;
		bool Active = true;
		std::map<ConnectionId, PeerState> Peers;
		std::unordered_map<std::uint32_t, std::uint32_t> HighestPeerGeneration;
		std::unordered_map<ObjectId, RemoteInstanceKind> Remotes;
		std::unordered_map<ObjectId, EventHandler> EventHandlers;
		std::unordered_map<ObjectId, RequestHandler> RequestHandlers;
		std::map<PendingKey, PendingRequest> PendingRequests;
		std::map<PendingKey, IncomingRequest> IncomingRequests;
		std::list<ConnectionId> ReadyPeers;
		bool Pumping = false;
		std::list<DeferredMessage> Deferred;
		std::chrono::steady_clock::time_point BroadcastRateWindow{};
		bool BroadcastRateWindowInitialized = false;
		std::size_t BroadcastBytes = 0;
		std::size_t BroadcastSubmissions = 0;
		std::chrono::steady_clock::time_point ManagerRateWindow{};
		bool ManagerRateWindowInitialized = false;
		std::uint32_t ManagerCalls = 0;
		std::chrono::steady_clock::time_point GeneratedRateWindow{};
		bool GeneratedRateWindowInitialized = false;
		std::size_t GeneratedReliableMessages = 0;
		std::size_t GeneratedReliableBytes = 0;
		std::map<ConnectionId, DisconnectInfo> TerminalConnections;
		TerminalHandler OnTerminal;
		RemoteMetrics Metrics;
	};

	RemoteManager::RemoteManager(
		RemoteManagerRole Role,
		INetworkScheduler &Scheduler,
		VisibilityCheck IsVisible,
		ObjectResolver ResolveObject,
		Clock GetTime
	)
		: State(
			  std::make_shared<Implementation>(
				  Role, Scheduler, std::move(IsVisible), std::move(ResolveObject), std::move(GetTime)
			  )
		  ) {}

	RemoteManager::~RemoteManager() {
		if (!State) return;
		State->Active = false;
		std::vector<Implementation::PendingCompletion> Completions;
		std::vector<Implementation::PendingKey> PendingKeys;
		PendingKeys.reserve(State->PendingRequests.size());
		for (const auto &[Key, Pending] : State->PendingRequests)
			PendingKeys.push_back(Key);
		for (const auto &Key : PendingKeys) {
			auto Pending = State->TakePending(
				Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::Disconnected)
			);
			if (Pending) Completions.push_back(std::move(*Pending));
		}
		for (const auto &[Key, Request] : State->IncomingRequests) { (void)Key; State->ReleaseHandlerEvidence(Request); }
		for (auto &[Connection, Peer] : State->Peers) State->RetireDispatch(Peer);
		for (const auto &Message : State->Deferred) State->ReleaseDeferred(Message);
		State->Peers.clear();
		State->IncomingRequests.clear();
		State->Deferred.clear();
		Implementation::RunCompletions(std::move(Completions));
	}

	bool RemoteManager::AddPeer(ConnectionId Connection, ReplicationEpoch Epoch, NetworkLimits Limits) {
		if (!State->Active || !Connection.IsValid() || !Epoch.IsValid() || !Limits.IsValid() ||
			State->Peers.size() >= MaximumRemotePeers)
			return false;
		auto Highest = State->HighestPeerGeneration.find(Connection.Slot);
		if ((Highest != State->HighestPeerGeneration.end() && Connection.Generation <= Highest->second) ||
			(Highest == State->HighestPeerGeneration.end() &&
			 State->HighestPeerGeneration.size() >= MaximumRemotePeerSlots))
			return false;
		for (const auto &[Existing, Peer] : State->Peers)
			if (Existing.Slot == Connection.Slot) return false;
		if (!State->Peers.emplace(Connection, Implementation::PeerState{.Epoch = Epoch, .Limits = Limits}).second)
			return false;
		State->HighestPeerGeneration[Connection.Slot] = Connection.Generation;
		return true;
	}

	bool RemoteManager::RemovePeer(ConnectionId Connection) {
		auto StateValue = State;
		return StateValue->RemovePeerState(Connection);
	}

	bool RemoteManager::RegisterRemote(ObjectId Remote, RemoteInstanceKind Kind) {
		return State->Active && Remote.IsValid() && Kind <= RemoteInstanceKind::Function &&
			   State->Remotes.emplace(Remote, Kind).second;
	}

	bool RemoteManager::UnregisterRemote(ObjectId Remote) {
		if (State->Remotes.erase(Remote) == 0) return false;
		std::vector<Implementation::PendingCompletion> Completions;
		State->EventHandlers.erase(Remote);
		State->RequestHandlers.erase(Remote);
		for (auto &[Connection, Peer] : State->Peers) {
			Peer.PublishedRemotes.erase(Remote);
			Peer.MaterializedObjects.erase(Remote);
			Peer.NextOutgoingSequence.erase(Remote);
			Peer.LatestIncomingSequence.erase(Remote);
		}
		std::vector<Implementation::PendingKey> PendingKeys;
		for (const auto &[Key, Pending] : State->PendingRequests)
			if (Pending.Remote == Remote) PendingKeys.push_back(Key);
		for (const auto &Key : PendingKeys) {
			auto Pending = State->TakePending(
				Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::ProtocolRejected)
			);
			if (Pending) Completions.push_back(std::move(*Pending));
		}
		for (auto Iterator = State->IncomingRequests.begin(); Iterator != State->IncomingRequests.end();) {
			if (Iterator->second.Remote != Remote) {
				++Iterator;
				continue;
			}
			if (Iterator->second.AcceptReply) {
				RemoteMessage Error{
					.Kind = RemoteMessageKind::RequestError,
					.Remote = Remote,
					.Request = Iterator->first.Request,
					.Error = StructuredRemoteError{"remote_unavailable", "Remote is unavailable"},
				};
				(void)State->SendMessage(Iterator->first.Connection, std::move(Error), false);
			}
			auto Peer = State->Peers.find(Iterator->first.Connection);
			if (Peer != State->Peers.end() && Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
			State->ReleaseHandlerEvidence(Iterator->second);
			Iterator = State->IncomingRequests.erase(Iterator);
		}
		for (auto &[Connection, Peer] : State->Peers) {
			Peer.Publications.erase(Remote);
			State->RetireDispatch(Peer, Remote);
		}
		for (auto Iterator = State->Deferred.begin(); Iterator != State->Deferred.end();) {
			if (Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			State->ReleaseDeferred(*Iterator);
			Iterator = State->Deferred.erase(Iterator);
		}
		auto StateValue = State;
		StateValue->DrainTerminals();
		Implementation::RunCompletions(std::move(Completions));
		return true;
	}

	bool RemoteManager::PublishRemote(ConnectionId Connection, ObjectId Remote, bool Materialized) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end() || !State->Remotes.contains(Remote) || !State->IsVisible ||
			!State->IsVisible(Connection, Remote))
			return false;
		const bool Inserted = Peer->second.PublishedRemotes.insert(Remote).second;
		if (Inserted) {
			auto &Publication = Peer->second.Publications[Remote];
			if (!Publication.IsValid())
				Publication = RemotePublicationId(1);
			else {
				auto Next = Publication.TryNext();
				if (!Next) {
					Peer->second.PublishedRemotes.erase(Remote);
					return false;
				}
				Publication = *Next;
			}
			Peer->second.NextOutgoingSequence.erase(Remote);
			Peer->second.LatestIncomingSequence.erase(Remote);
		}
		if (Materialized) Peer->second.MaterializedObjects.insert(Remote);
		return true;
	}

	bool RemoteManager::UnpublishRemote(ConnectionId Connection, ObjectId Remote) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end() || Peer->second.PublishedRemotes.erase(Remote) == 0) return false;
		std::vector<Implementation::PendingCompletion> Completions;
		Peer->second.MaterializedObjects.erase(Remote);
		Peer->second.NextOutgoingSequence.erase(Remote);
		Peer->second.LatestIncomingSequence.erase(Remote);
		std::vector<Implementation::PendingKey> PendingKeys;
		for (const auto &[Key, Pending] : State->PendingRequests)
			if (Key.Connection == Connection && Pending.Remote == Remote) PendingKeys.push_back(Key);
		for (const auto &Key : PendingKeys) {
			auto Pending = State->TakePending(
				Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::ProtocolRejected)
			);
			if (Pending) Completions.push_back(std::move(*Pending));
		}
		for (auto Iterator = State->IncomingRequests.begin(); Iterator != State->IncomingRequests.end();) {
			if (Iterator->first.Connection != Connection || Iterator->second.Remote != Remote) {
				++Iterator;
				continue;
			}
			if (Iterator->second.AcceptReply) {
				RemoteMessage Error{
					.Kind = RemoteMessageKind::RequestError,
					.Remote = Remote,
					.Request = Iterator->first.Request,
					.Error = StructuredRemoteError{"remote_unavailable", "Remote is unavailable"},
				};
				(void)State->SendMessage(Iterator->first.Connection, std::move(Error), false);
			}
			if (Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
			State->ReleaseHandlerEvidence(Iterator->second);
			Iterator = State->IncomingRequests.erase(Iterator);
		}
		State->RetireDispatch(Peer->second, Remote);
		for (auto Iterator = State->Deferred.begin(); Iterator != State->Deferred.end();) {
			if (Iterator->Connection != Connection || Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			State->ReleaseDeferred(*Iterator);
			Iterator = State->Deferred.erase(Iterator);
		}
		auto StateValue = State;
		StateValue->DrainTerminals();
		Implementation::RunCompletions(std::move(Completions));
		return true;
	}

	bool RemoteManager::MarkMaterialized(ConnectionId Connection, ObjectId Object) {
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end() || !Object.IsValid() || !State->IsVisible ||
			!State->IsVisible(Connection, Object))
			return false;
		Peer->second.MaterializedObjects.insert(Object);
		return true;
	}

	bool RemoteManager::MarkUnmaterialized(ConnectionId Connection, ObjectId Object) {
		auto Peer = State->Peers.find(Connection);
		if (Peer != State->Peers.end() && State->Remotes.contains(Object) &&
			Peer->second.PublishedRemotes.contains(Object))
			return UnpublishRemote(Connection, Object);
		return Peer != State->Peers.end() && Peer->second.MaterializedObjects.erase(Object) != 0;
	}

	bool RemoteManager::SetEventHandler(ObjectId Remote, EventHandler Handler) {
		auto Kind = State->Remotes.find(Remote);
		if (Kind == State->Remotes.end() || Kind->second == RemoteInstanceKind::Function) return false;
		State->EventHandlers[Remote] = std::move(Handler);
		return true;
	}

	bool RemoteManager::SetRequestHandler(ObjectId Remote, RequestHandler Handler) {
		auto Kind = State->Remotes.find(Remote);
		if (Kind == State->Remotes.end() || Kind->second != RemoteInstanceKind::Function) return false;
		State->RequestHandlers[Remote] = std::move(Handler);
		return true;
	}

	void RemoteManager::SetTerminalHandler(TerminalHandler Handler) {
		State->OnTerminal = std::move(Handler);
	}

	void RemoteManager::DrainSchedulerTerminals() {
		auto StateValue = State;
		StateValue->DrainTerminals();
	}

	RemoteSendResult
	RemoteManager::SendEvent(ConnectionId Connection, ObjectId Remote, std::vector<RemoteValue> Arguments) {
		if (!State->Active) return {RemoteSendStatus::InvalidPeer};
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end()) return {RemoteSendStatus::InvalidPeer};
		auto Registered = State->Remotes.find(Remote);
		if (Registered == State->Remotes.end()) return {RemoteSendStatus::UnknownRemote};
		if (!State->RemoteAvailable(Connection, Remote)) return {RemoteSendStatus::UnpublishedRemote};
		RemoteMessage Message{.Remote = Remote, .Arguments = std::move(Arguments)};
		switch (Registered->second) {
		case RemoteInstanceKind::ReliableEvent:
			Message.Kind = RemoteMessageKind::ReliableEvent;
			break;
		case RemoteInstanceKind::UnreliableEvent:
			Message.Kind = RemoteMessageKind::UnreliableEvent;
			break;
		case RemoteInstanceKind::UnreliableSequencedEvent: {
			Message.Kind = RemoteMessageKind::SequencedEvent;
			auto &Next = Peer->second.NextOutgoingSequence[Remote];
			if (!Next.IsValid()) Next = RemoteEventSequence(1);
			Message.Sequence = Next;
			auto Following = Next.TryNext();
			if (!Following) return {RemoteSendStatus::SchedulerRejected};
			Next = *Following;
			break;
		}
		case RemoteInstanceKind::Function:
			return {RemoteSendStatus::WrongRemoteKind};
		}
		if (!Message.IsValid()) return {RemoteSendStatus::InvalidArguments};
		if (!State->AdmitCall(Peer->second, Remote)) return {RemoteSendStatus::RateLimited};
		auto Result = State->SendMessage(Connection, std::move(Message));
		if (Registered->second == RemoteInstanceKind::ReliableEvent) {
			SaturatingIncrement(
				Result.Accepted() ? State->Metrics.ReliableEventsAccepted : State->Metrics.ReliableEventsRejected
			);
		} else if (Registered->second == RemoteInstanceKind::UnreliableEvent) {
			SaturatingIncrement(
				Result.Accepted() ? State->Metrics.UnreliableEventsAccepted : State->Metrics.UnreliableEventsDropped
			);
		} else {
			SaturatingIncrement(
				Result.Accepted() ? State->Metrics.SequencedEventsAccepted : State->Metrics.UnreliableEventsDropped
			);
		}
		DrainSchedulerTerminals();
		return Result;
	}

	std::vector<RemoteSendResult> RemoteManager::Broadcast(ObjectId Remote, std::vector<RemoteValue> Arguments) {
		if (!State->Active) return {};
		SaturatingIncrement(State->Metrics.BroadcastInvocations);
		std::vector<RemoteSendResult> Results;
		Results.reserve(State->Peers.size());
		std::vector<ConnectionId> Connections;
		Connections.reserve(State->Peers.size());
		for (const auto &[Connection, Peer] : State->Peers)
			Connections.push_back(Connection);
		if (Connections.empty()) return Results;
		auto Registered = State->Remotes.find(Remote);
		if (Registered == State->Remotes.end() || Registered->second == RemoteInstanceKind::Function)
			return std::vector<RemoteSendResult>(Connections.size(), {RemoteSendStatus::WrongRemoteKind});
		RemoteMessage Estimate{.Remote = Remote, .Arguments = std::move(Arguments)};
		if (Registered->second == RemoteInstanceKind::ReliableEvent)
			Estimate.Kind = RemoteMessageKind::ReliableEvent;
		else if (Registered->second == RemoteInstanceKind::UnreliableEvent)
			Estimate.Kind = RemoteMessageKind::UnreliableEvent;
		else {
			Estimate.Kind = RemoteMessageKind::SequencedEvent;
			Estimate.Sequence = RemoteEventSequence(1);
		}
		auto EncodedEstimate = EncodeRemoteMessage(Estimate);
		if (!EncodedEstimate)
			return std::vector<RemoteSendResult>(Connections.size(), {RemoteSendStatus::InvalidArguments});
		const auto Now = State->GetTime();
		if (!State->BroadcastRateWindowInitialized || Now < State->BroadcastRateWindow ||
			Now - State->BroadcastRateWindow >= std::chrono::seconds(1)) {
			State->BroadcastRateWindow = Now;
			State->BroadcastRateWindowInitialized = true;
			State->BroadcastBytes = 0;
			State->BroadcastSubmissions = 0;
		}
		const auto Fanout = Connections.size();
		const auto EncodedBytes = EncodedEstimate->size();
		if (Fanout > MaximumRemoteBroadcastSubmissionsPerSecond - State->BroadcastSubmissions ||
			EncodedBytes > MaximumRemoteBroadcastBytesPerSecond ||
			Fanout > (MaximumRemoteBroadcastBytesPerSecond - State->BroadcastBytes) / EncodedBytes) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			return std::vector<RemoteSendResult>(Connections.size(), {RemoteSendStatus::SchedulerRejected});
		}
		State->BroadcastSubmissions += Fanout;
		State->BroadcastBytes += Fanout * EncodedBytes;
		for (const auto Connection : Connections) {
			auto Result = SendEvent(Connection, Remote, Estimate.Arguments);
			if (Result.Accepted()) SaturatingIncrement(State->Metrics.BroadcastPeerSubmissions);
			Results.push_back(Result);
		}
		return Results;
	}

	RemoteSendResult RemoteManager::StartRequest(
		ConnectionId Connection,
		ObjectId Remote,
		std::vector<RemoteValue> Arguments,
		RequestCompletion Completion,
		std::chrono::milliseconds Deadline
	) {
		if (!State->Active) return {RemoteSendStatus::InvalidPeer};
		auto Peer = State->Peers.find(Connection);
		if (Peer == State->Peers.end()) return {RemoteSendStatus::InvalidPeer};
		auto Registered = State->Remotes.find(Remote);
		if (Registered == State->Remotes.end()) return {RemoteSendStatus::UnknownRemote};
		if (Registered->second != RemoteInstanceKind::Function) return {RemoteSendStatus::WrongRemoteKind};
		if (!State->RemoteAvailable(Connection, Remote)) return {RemoteSendStatus::UnpublishedRemote};
		if (Deadline <= std::chrono::milliseconds::zero() || Deadline > MaximumRemoteRequestDeadline)
			return {RemoteSendStatus::InvalidArguments};
		if (State->PendingRequests.size() >= MaximumRemoteInFlightRequestsPerManager ||
			Peer->second.PendingOutgoingRequests >= Peer->second.Limits.MaximumInFlightRemoteRequests)
			return {RemoteSendStatus::RequestLimitExceeded};
		if (!State->AdmitCall(Peer->second, Remote)) return {RemoteSendStatus::RateLimited};
		const auto Request = Peer->second.NextRequest;
		auto Next = Request.TryNext();
		if (!Request.IsValid() || !Next) return {RemoteSendStatus::RequestLimitExceeded};
		Peer->second.NextRequest = *Next;
		RemoteMessage Message{
			.Kind = RemoteMessageKind::Request,
			.Remote = Remote,
			.Request = Request,
			.Deadline = Deadline,
			.Arguments = std::move(Arguments),
		};
		if (!Message.IsValid()) return {RemoteSendStatus::InvalidArguments};
		Implementation::PendingKey Key{Connection, Request};
		runtime_detail::RecordPublicationLatency({.Stage = "RpcRequestStarted", .Connection = Connection,
			.Object = Remote, .Sequence = Request.Value()});
		auto Result = State->SendMessage(Connection, std::move(Message));
		Result.Request = Request;
		if (!Result.Accepted()) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			DrainSchedulerTerminals();
		} else {
			runtime_detail::RecordPublicationLatency({.Stage = "RpcRequestSchedulerAccepted",
				.Connection = Connection, .Object = Remote, .Sequence = Request.Value()});
			const auto Started = State->GetTime();
			State->PendingRequests.emplace(
				Key, Implementation::PendingRequest{Remote, Started + Deadline, std::move(Completion), Started}
			);
			++Peer->second.PendingOutgoingRequests;
			State->Metrics.InFlightRequests = State->PendingRequests.size();
			State->ObserveOwnership(Peer->second);
			SaturatingIncrement(State->Metrics.RequestsStarted);
		}
		return Result;
	}

	bool RemoteManager::CancelRequest(RemoteRequestHandle Handle) {
		const auto Runtime = State;
		if (!Runtime->Active) return false;
		Implementation::PendingKey Key{Handle.Connection, Handle.Request};
		auto Pending = Runtime->PendingRequests.find(Key);
		if (Pending == Runtime->PendingRequests.end()) return false;
		const auto Remote = Pending->second.Remote;
		Runtime->CompletePending(Key, TerminalResult(Handle.Request, RemoteRequestTerminalStatus::Cancelled));
		if (!Runtime->Active) return true;
		RemoteMessage Message{.Kind = RemoteMessageKind::Cancellation, .Remote = Remote, .Request = Handle.Request};
		(void)Runtime->SendMessage(Handle.Connection, std::move(Message), false);
		Runtime->DrainTerminals();
		return true;
	}

	bool RemoteManager::HandleTransportEvent(const TransportEvent &Event) {
		if (!State->Active) return false;
		if (const auto *Disconnected = std::get_if<DisconnectedEvent>(&Event))
			return RemovePeer(Disconnected->Connection);
		if (const auto *Changed = std::get_if<ConnectionStateEvent>(&Event);
			Changed && Changed->Current == ConnectionState::Closed)
			return RemovePeer(Changed->Connection);
		const auto *Received = std::get_if<ReceivedMessageEvent>(&Event);
		if (!Received) return false;
		auto Peer = State->Peers.find(Received->Connection);
		if (Peer == State->Peers.end()) return false;
		const auto DeliveryBytes = Received->Delivery == DeliveryMode::ReliableOrdered ?
			Peer->second.Limits.MaximumReliableMessageBytes : Peer->second.Limits.MaximumUnreliableMessageBytes;
		if (Received->Payload.size() > DeliveryBytes ||
			Received->Payload.size() > Peer->second.Limits.MaximumDecodedMessageBytes ||
			Peer->second.DispatchQueue.size() >= Peer->second.Limits.MaximumMessagesPerTick ||
			Received->Payload.size() > Peer->second.Limits.MaximumReceiveBytesPerTick ||
			Peer->second.QueuedDispatchBytes >
				Peer->second.Limits.MaximumReceiveBytesPerTick - Received->Payload.size() ||
			State->Metrics.QueuedDispatchMessages >= MaximumQueuedRemoteDispatchMessages ||
			Received->Payload.size() > MaximumQueuedRemoteDispatchBytes ||
			State->Metrics.QueuedDispatchBytes > MaximumQueuedRemoteDispatchBytes - Received->Payload.size()) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			return false;
		}
		std::size_t ArgumentBytes;
		auto Decoded = DecodeRemoteMessage(Received->Payload, &ArgumentBytes);
		if (!Decoded) {
			SaturatingIncrement(State->Metrics.ProtocolRejections);
			return false;
		}
		if (ArgumentBytes > MaximumRetainedRemoteArgumentBytes - State->Metrics.QueuedDispatchArgumentBytes ||
			ArgumentBytes > Peer->second.Limits.MaximumReceiveBytesPerTick * 4 - Peer->second.QueuedDispatchArgumentBytes) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			return false;
		}
		if (Received->Delivery != DeliveryFor(Decoded->Kind) || Received->Traffic != TrafficFor(Decoded->Kind)) {
			SaturatingIncrement(State->Metrics.ProtocolRejections);
			return false;
		}
		if (Decoded->Kind == RemoteMessageKind::SequencedEvent) {
			const auto *Order = std::get_if<RemoteEventOrder>(&Received->Order);
			if (!Order || Order->Channel != RemoteStateChannel(Decoded->Remote) ||
				Order->Publication != Decoded->Publication || Order->Sequence != Decoded->Sequence) {
				SaturatingIncrement(State->Metrics.ProtocolRejections);
				return false;
			}
		} else if (!std::holds_alternative<std::monostate>(Received->Order)) {
			SaturatingIncrement(State->Metrics.ProtocolRejections);
			return false;
		}
		if (Decoded->Kind == RemoteMessageKind::Request || Decoded->Kind == RemoteMessageKind::Response ||
			Decoded->Kind == RemoteMessageKind::RequestError)
			runtime_detail::RecordPublicationLatency({
				.Stage = Decoded->Kind == RemoteMessageKind::Request ? "RpcRequestReceived" : "RpcResponseReceived",
				.Connection = Received->Connection, .Object = Decoded->Remote,
				.Sequence = Decoded->Request.Value(),
				.Bytes = static_cast<std::uint32_t>(Received->Payload.size())});
		State->QueueDispatch(Peer->second, {
			Received->Connection, std::move(*Decoded), Received->Payload.size(), ArgumentBytes, State->GetTime()
		});
		return true;
	}

	std::size_t RemoteManager::Pump(std::size_t MaximumMessages) {
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RemotePump);
		const auto Runtime = State;
		if (!Runtime->Active || Runtime->Pumping || MaximumMessages == 0 ||
			MaximumMessages > NativeMaximumNetworkMessagesPerTick) return 0;
		struct PumpGuard {
			bool &Pumping;
			~PumpGuard() { Pumping = false; }
		} Guard{Runtime->Pumping};
		Runtime->Pumping = true;

		// Freeze the eligible prefix before callbacks can append work or retire a
		// lane. Only identifiers survive across dispatch; callbacks can erase peers.
		std::array<ConnectionId, MaximumRemotePeers> EligiblePeers;
		std::size_t PeerCount = 0;
		for (const auto Connection : Runtime->ReadyPeers) {
			EligiblePeers[PeerCount++] = Connection;
			auto &Peer = Runtime->Peers.at(Connection);
			Peer.EligibleDispatchMessages = Peer.DispatchQueue.size();
		}
		const auto Now = Runtime->GetTime();
		std::vector<Implementation::PendingKey> TimedOut;
		for (const auto &[Key, Pending] : Runtime->PendingRequests)
			if (Now >= Pending.Deadline) TimedOut.push_back(Key);
		for (const auto &Key : TimedOut) {
			auto Pending = Runtime->PendingRequests.find(Key);
			if (Pending == Runtime->PendingRequests.end()) continue;
			const auto Remote = Pending->second.Remote;
			Runtime->CompletePending(Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::Timeout));
			if (!Runtime->Active) return 0;
			RemoteMessage Cancellation{
				.Kind = RemoteMessageKind::Cancellation, .Remote = Remote, .Request = Key.Request
			};
			(void)Runtime->SendMessage(Key.Connection, std::move(Cancellation), false);
		}

		std::vector<Implementation::PendingKey> IncomingReplyTimedOut;
		std::vector<Implementation::PendingKey> IncomingWorkExpired;
		for (const auto &[Key, Incoming] : Runtime->IncomingRequests) {
			if (Incoming.AcceptReply && Now >= Incoming.ReplyDeadline) IncomingReplyTimedOut.push_back(Key);
			if (Now >= Incoming.WorkDeadline) IncomingWorkExpired.push_back(Key);
		}
		for (const auto &Key : IncomingReplyTimedOut) {
			auto Incoming = Runtime->IncomingRequests.find(Key);
			if (Incoming == Runtime->IncomingRequests.end()) continue;
			Incoming->second.AcceptReply = false;
			const auto Remote = Incoming->second.Remote;
			RemoteMessage Error{
				.Kind = RemoteMessageKind::RequestError,
				.Remote = Remote,
				.Request = Key.Request,
				.Error = StructuredRemoteError{"handler_timeout", "Remote handler deadline expired"}
			};
			(void)Runtime->SendMessage(Key.Connection, std::move(Error), false);
		}
		for (const auto &Key : IncomingWorkExpired) {
			auto Incoming = Runtime->IncomingRequests.find(Key);
			if (Incoming == Runtime->IncomingRequests.end()) continue;
			Runtime->ReleaseHandlerEvidence(Incoming->second);
			Runtime->IncomingRequests.erase(Incoming);
			auto Peer = Runtime->Peers.find(Key.Connection);
			if (Peer != Runtime->Peers.end() && Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
		}

		std::set<std::pair<ConnectionId, ObjectId>> BlockedStreams;
		for (auto Iterator = Runtime->Deferred.begin(); Iterator != Runtime->Deferred.end();) {
			const auto Connection = Iterator->Connection;
			const auto Peer = Runtime->Peers.find(Connection);
			bool LiveRequest = true;
			if (Iterator->Message.Kind == RemoteMessageKind::Request) {
				const auto Pending = Runtime->PendingRequests.find({Connection, Iterator->Message.Request});
				LiveRequest = Pending != Runtime->PendingRequests.end() && Pending->second.Remote == Iterator->Message.Remote;
			}
			RemotePublicationId Publication;
			const bool Live = Now < Iterator->Deadline && LiveRequest && Peer != Runtime->Peers.end() &&
				!Runtime->TerminalConnections.contains(Connection) &&
				Runtime->RemoteAvailable(Connection, Iterator->Message.Remote, nullptr, &Publication) &&
				Publication == Iterator->Message.Publication;
			const auto Stream = DeferredOrderingStream(Iterator->Message);
			const auto Key = std::pair{Connection, Stream.value_or(ObjectId{})};
			bool Ready = Live && (!Stream || !BlockedStreams.contains(Key));
			bool InvalidDependency = false;
			if (Live) {
				for (const auto Object : Iterator->Dependencies) {
					if (Peer->second.MaterializedObjects.contains(Object)) continue;
					Ready = false;
					if (!Runtime->IsVisible(Connection, Object)) {
						InvalidDependency = true;
						break;
					}
				}
			}
			if (Live && !Ready && !InvalidDependency) {
				if (Stream) BlockedStreams.insert(Key);
				++Iterator;
				continue;
			}
			auto Message = std::move(*Iterator);
			Iterator = Runtime->Deferred.erase(Iterator);
			Runtime->ReleaseDeferred(Message);
			if (Ready)
				(void)Runtime->SubmitEncoded(Connection, Message.Message, std::move(Message.Encoded), false);
			else
				SaturatingIncrement(Runtime->Metrics.ResourceRejections);
		}

		const auto PerPeerLimit = std::max<std::size_t>(
			1, MaximumMessages / std::max<std::size_t>(1, PeerCount)
		);
		std::size_t Processed = 0;
		for (std::size_t Index = 0; Index < PeerCount && Processed < MaximumMessages && Runtime->Active; ++Index) {
			const auto Connection = EligiblePeers[Index];
			const auto Limit = std::min(PerPeerLimit, MaximumMessages - Processed);
			for (std::size_t Turn = 0; Turn < Limit && Runtime->Active; ++Turn) {
				auto Found = Runtime->Peers.find(Connection);
				if (Found == Runtime->Peers.end() || Found->second.EligibleDispatchMessages == 0) break;
				auto &Peer = Found->second;
				if (Turn == 0)
					Runtime->ReadyPeers.splice(Runtime->ReadyPeers.end(), Runtime->ReadyPeers, *Peer.ReadyPosition);
				auto Queued = std::move(Peer.DispatchQueue.front());
				Peer.DispatchQueue.pop_front();
				--Peer.EligibleDispatchMessages;
				if (Peer.DispatchQueue.empty()) Runtime->ClearReadyPosition(Peer);
				Runtime->ReleaseDispatch(Peer, Queued);
				++Processed;
				Runtime->Dispatch(std::move(Queued));
			}
		}
		if (Runtime->Active) Runtime->DrainTerminals();
		return Processed;
	}

	RemoteMetrics RemoteManager::GetMetrics() const {
		auto Result = State->Metrics;
		Result.Observed = true;
		Result.IncomingHandlers = State->IncomingRequests.size();
		return Result;
	}
	RemoteManagerRole RemoteManager::GetRole() const {
		return State->Role;
	}
	std::shared_ptr<Instance> RemoteManager::ResolveObject(ObjectId Object) const {
		return State->ResolveObject ? State->ResolveObject(Object) : nullptr;
	}

	void RemoteManager::SetOutgoingReferenceMapper(OutgoingReferenceMapper Mapper) {
		if (State->Active) State->MapOutgoingReference = std::move(Mapper);
	}

	std::optional<ObjectId> RemoteManager::MapOutgoingReference(const Instance &Object) const {
		if (!State->Active || Object.IsDestroying() || Object.GetDestroyed())
			return std::nullopt;
		if (State->MapOutgoingReference) return State->MapOutgoingReference(Object);
		std::scoped_lock Lock(Object.IdentityMutex);
		return Object.Id.IsValid() ? std::optional{Object.Id} : std::nullopt;
	}
	void RemoteManager::SetPlayerResolvers(PlayerLookup ResolvePlayer, PeerLookup ResolvePeer) {
		if (State->Role != RemoteManagerRole::Server) return;
		FindPlayer = std::move(ResolvePlayer);
		FindPeer = std::move(ResolvePeer);
	}
	std::shared_ptr<Player> RemoteManager::ResolvePlayer(const RemotePeerContext &Peer) const {
		if (!State->Active || !FindPlayer) return nullptr;
		const auto Found = State->Peers.find(Peer.Connection);
		if (Found == State->Peers.end() || Found->second.Epoch != Peer.Epoch) return nullptr;
		return FindPlayer(Peer);
	}
	std::optional<RemotePeerContext> RemoteManager::ResolvePeer(const Player &PlayerValue) const {
		if (!State->Active || !FindPeer) return std::nullopt;
		const auto Peer = FindPeer(PlayerValue);
		if (!Peer) return std::nullopt;
		const auto Found = State->Peers.find(Peer->Connection);
		if (Found == State->Peers.end() || Found->second.Epoch != Peer->Epoch) return std::nullopt;
		return Peer;
	}
	bool RemoteManager::IsCurrentPublication(
		const RemotePeerContext &Peer, ObjectId Remote, RemotePublicationId Publication
	) const {
		if (!State->Active) return false;
		const auto Found = State->Peers.find(Peer.Connection);
		RemotePublicationId Current;
		return Found != State->Peers.end() && Found->second.Epoch == Peer.Epoch &&
			State->RemoteAvailable(Peer.Connection, Remote, nullptr, &Current) && Current == Publication;
	}
	std::weak_ptr<void> RemoteManager::GetLifetimeToken() const {
		return State;
	}
}

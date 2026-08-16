#include "gargantuan/network/RemoteManager.hpp"

#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <deque>
#include <limits>
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
			std::vector<WireValue> Results = {},
			std::optional<StructuredRemoteError> Error = std::nullopt
		) {
			return {
				.Outcome = {.Request = Request, .Status = Status, .Error = std::move(Error)},
				.Results = std::move(Results),
			};
		}
	}

	struct RemoteManager::Implementation : std::enable_shared_from_this<Implementation> {
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
			std::size_t QueuedDispatchMessages = 0;
			std::size_t QueuedDispatchBytes = 0;
			std::size_t DeferredReliableBytes = 0;
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
		};

		struct IncomingRequest {
			ObjectId Remote;
			std::chrono::steady_clock::time_point ReplyDeadline;
			std::chrono::steady_clock::time_point WorkDeadline;
			bool AcceptReply = true;
		};

		struct QueuedMessage {
			ConnectionId Connection;
			RemoteMessage Message;
			std::size_t EncodedBytes = 0;
		};

		struct DeferredMessage {
			ConnectionId Connection;
			RemoteMessage Message;
			std::vector<std::byte> Encoded;
			std::vector<ObjectId> Dependencies;
			std::chrono::steady_clock::time_point Deadline;
		};

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
			std::span<const WireValue> Arguments,
			bool RequireMaterialized,
			std::vector<ObjectId> *Missing = nullptr
		) {
			if (Arguments.size() > MaximumRemoteArguments) return false;
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return false;
			for (const auto &Argument : Arguments) {
				try {
					ValidateProtocolWireValue(Argument);
				} catch (...) {
					return false;
				}
				if (const auto *String = std::get_if<std::string>(&Argument);
					String && String->size() > MaximumRemoteStringBytes)
					return false;
				const auto *Reference = std::get_if<WireObjectReference>(&Argument);
				if (!Reference) continue;
				const auto Object = Reference->Object.ToObjectId();
				if (!Object.IsValid() || !IsVisible || !IsVisible(Connection, Object) || !ResolveObject ||
					!ResolveObject(Object)) {
					SaturatingIncrement(Metrics.VisibilityRejections);
					return false;
				}
				if (RequireMaterialized && !Peer->second.MaterializedObjects.contains(Object)) {
					if (Missing)
						Missing->push_back(Object);
					else
						return false;
				}
			}
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
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end()) return {RemoteSendStatus::InvalidPeer};
			std::vector<ObjectId> Missing;
			if (!ArgumentsValid(Connection, Message.Arguments, true, &Missing))
				return {RemoteSendStatus::InvisibleReference};
			if (!Missing.empty()) {
				if (!AllowDependencyDeferral || DeliveryFor(Message.Kind) != DeliveryMode::ReliableOrdered)
					return {RemoteSendStatus::ReferenceNotMaterialized};
				if (Deferred.size() >= MaximumQueuedRemoteDispatchMessages ||
					Encoded.size() > MaximumQueuedRemoteDispatchBytes ||
					Metrics.DeferredReliableBytes > MaximumQueuedRemoteDispatchBytes - Encoded.size() ||
					Encoded.size() > Peer->second.Limits.MaximumQueuedReliableBytes ||
					Peer->second.DeferredReliableBytes >
						Peer->second.Limits.MaximumQueuedReliableBytes - Encoded.size())
					return {RemoteSendStatus::SchedulerRejected};
				Deferred.push_back(
					{Connection,
					 Message,
					 std::move(Encoded),
					 std::move(Missing),
					 GetTime() +
						 (Message.Kind == RemoteMessageKind::Request ? Message.Deadline : MaximumRemoteRequestDeadline)}
				);
				Metrics.DeferredReliableMessages = Deferred.size();
				Metrics.DeferredReliableBytes += Deferred.back().Encoded.size();
				Peer->second.DeferredReliableBytes += Deferred.back().Encoded.size();
				return {RemoteSendStatus::DeferredForMaterialization};
			}
			auto Intent = MakeNetworkMessageIntent(
				Connection,
				DeliveryFor(Message.Kind),
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
				if (Submitted.TerminalDisconnect)
					TerminalConnections.try_emplace(Connection, *Submitted.TerminalDisconnect);
				return {
					.Status = RemoteSendStatus::SchedulerRejected,
					.TerminalDisconnect = std::move(Submitted.TerminalDisconnect),
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
				const auto Bytes = Iterator->Encoded.size();
				if (auto Peer = Peers.find(Key.Connection); Peer != Peers.end())
					Peer->second.DeferredReliableBytes -= Bytes;
				Metrics.DeferredReliableBytes -= Bytes;
				Iterator = Deferred.erase(Iterator);
			}
			Metrics.DeferredReliableMessages = Deferred.size();
			if (auto Peer = Peers.find(Key.Connection);
				Peer != Peers.end() && Peer->second.PendingOutgoingRequests != 0)
				--Peer->second.PendingOutgoingRequests;
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
			std::erase_if(IncomingRequests, [&](const auto &Entry) { return Entry.first.Connection == Connection; });
			for (auto Iterator = DispatchQueue.begin(); Iterator != DispatchQueue.end();) {
				if (Iterator->Connection != Connection) {
					++Iterator;
					continue;
				}
				Metrics.QueuedDispatchBytes -= Iterator->EncodedBytes;
				Iterator = DispatchQueue.erase(Iterator);
			}
			for (auto Iterator = Deferred.begin(); Iterator != Deferred.end();) {
				if (Iterator->Connection != Connection) {
					++Iterator;
					continue;
				}
				Metrics.DeferredReliableBytes -= Iterator->Encoded.size();
				Iterator = Deferred.erase(Iterator);
			}
			Metrics.DeferredReliableMessages = Deferred.size();
			Metrics.QueuedDispatchMessages = DispatchQueue.size();
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
			PendingKey Key, ObjectId Remote, std::vector<WireValue> Results, std::optional<StructuredRemoteError> Error
		) {
			auto Incoming = IncomingRequests.find(Key);
			if (Incoming == IncomingRequests.end() || Incoming->second.Remote != Remote) return false;
			auto Peer = Peers.find(Key.Connection);
			if (Peer == Peers.end()) return false;
			const bool CanReply = Incoming->second.AcceptReply && GetTime() < Incoming->second.ReplyDeadline &&
								  RemoteAvailable(Key.Connection, Remote);
			IncomingRequests.erase(Incoming);
			if (Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
			if (!CanReply) return false;
			RemoteMessage Message{
				.Kind = Error ? RemoteMessageKind::RequestError : RemoteMessageKind::Response,
				.Remote = Remote,
				.Request = Key.Request,
				.Arguments = Error ? std::vector<WireValue>{} : std::move(Results),
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
			return SendMessage(Key.Connection, std::move(Message)).Accepted();
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
						 std::move(Queued.Message.Arguments)}
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
				}
			);
			++Peer->second.ConcurrentHandlers;
			auto Weak = weak_from_this();
			RequestReply Reply = [Weak, Key, Remote = Queued.Message.Remote](
									 std::vector<WireValue> Results, std::optional<StructuredRemoteError> Error
								 ) mutable {
				auto Self = Weak.lock();
				return Self && Self->Active && Self->SendReply(Key, Remote, std::move(Results), std::move(Error));
			};
			auto Callback = Handler->second;
			try {
				Callback(
					{{Queued.Connection, Peer->second.Epoch},
					 Queued.Message.Remote,
					 std::move(Queued.Message.Arguments)},
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
		Clock GetTime;
		bool Active = true;
		std::map<ConnectionId, PeerState> Peers;
		std::unordered_map<std::uint32_t, std::uint32_t> HighestPeerGeneration;
		std::unordered_map<ObjectId, RemoteInstanceKind> Remotes;
		std::unordered_map<ObjectId, EventHandler> EventHandlers;
		std::unordered_map<ObjectId, RequestHandler> RequestHandlers;
		std::map<PendingKey, PendingRequest> PendingRequests;
		std::map<PendingKey, IncomingRequest> IncomingRequests;
		std::deque<QueuedMessage> DispatchQueue;
		std::deque<DeferredMessage> Deferred;
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
		State->Peers.clear();
		State->IncomingRequests.clear();
		State->DispatchQueue.clear();
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
			Iterator = State->IncomingRequests.erase(Iterator);
		}
		for (auto &[Connection, Peer] : State->Peers)
			Peer.Publications.erase(Remote);
		for (auto Iterator = State->DispatchQueue.begin(); Iterator != State->DispatchQueue.end();) {
			if (Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			auto Peer = State->Peers.find(Iterator->Connection);
			if (Peer != State->Peers.end()) {
				--Peer->second.QueuedDispatchMessages;
				Peer->second.QueuedDispatchBytes -= Iterator->EncodedBytes;
			}
			State->Metrics.QueuedDispatchBytes -= Iterator->EncodedBytes;
			Iterator = State->DispatchQueue.erase(Iterator);
		}
		for (auto Iterator = State->Deferred.begin(); Iterator != State->Deferred.end();) {
			if (Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			auto Peer = State->Peers.find(Iterator->Connection);
			if (Peer != State->Peers.end()) Peer->second.DeferredReliableBytes -= Iterator->Encoded.size();
			State->Metrics.DeferredReliableBytes -= Iterator->Encoded.size();
			Iterator = State->Deferred.erase(Iterator);
		}
		State->Metrics.QueuedDispatchMessages = State->DispatchQueue.size();
		State->Metrics.DeferredReliableMessages = State->Deferred.size();
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
			Iterator = State->IncomingRequests.erase(Iterator);
		}
		for (auto Iterator = State->DispatchQueue.begin(); Iterator != State->DispatchQueue.end();) {
			if (Iterator->Connection != Connection || Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			--Peer->second.QueuedDispatchMessages;
			Peer->second.QueuedDispatchBytes -= Iterator->EncodedBytes;
			State->Metrics.QueuedDispatchBytes -= Iterator->EncodedBytes;
			Iterator = State->DispatchQueue.erase(Iterator);
		}
		for (auto Iterator = State->Deferred.begin(); Iterator != State->Deferred.end();) {
			if (Iterator->Connection != Connection || Iterator->Message.Remote != Remote) {
				++Iterator;
				continue;
			}
			Peer->second.DeferredReliableBytes -= Iterator->Encoded.size();
			State->Metrics.DeferredReliableBytes -= Iterator->Encoded.size();
			Iterator = State->Deferred.erase(Iterator);
		}
		State->Metrics.QueuedDispatchMessages = State->DispatchQueue.size();
		State->Metrics.DeferredReliableMessages = State->Deferred.size();
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
	RemoteManager::SendEvent(ConnectionId Connection, ObjectId Remote, std::vector<WireValue> Arguments) {
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

	std::vector<RemoteSendResult> RemoteManager::Broadcast(ObjectId Remote, std::vector<WireValue> Arguments) {
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
		RemoteMessage Estimate{.Remote = Remote, .Arguments = Arguments};
		if (Registered == State->Remotes.end() || Registered->second == RemoteInstanceKind::Function)
			return std::vector<RemoteSendResult>(Connections.size(), {RemoteSendStatus::WrongRemoteKind});
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
			auto Result = SendEvent(Connection, Remote, Arguments);
			if (Result.Accepted()) SaturatingIncrement(State->Metrics.BroadcastPeerSubmissions);
			Results.push_back(Result);
		}
		return Results;
	}

	RemoteSendResult RemoteManager::StartRequest(
		ConnectionId Connection,
		ObjectId Remote,
		std::vector<WireValue> Arguments,
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
		auto Result = State->SendMessage(Connection, std::move(Message));
		Result.Request = Request;
		if (!Result.Accepted()) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			DrainSchedulerTerminals();
		} else {
			State->PendingRequests.emplace(
				Key, Implementation::PendingRequest{Remote, State->GetTime() + Deadline, std::move(Completion)}
			);
			++Peer->second.PendingOutgoingRequests;
			State->Metrics.InFlightRequests = State->PendingRequests.size();
			SaturatingIncrement(State->Metrics.RequestsStarted);
		}
		return Result;
	}

	bool RemoteManager::CancelRequest(RemoteRequestHandle Handle) {
		if (!State->Active) return false;
		Implementation::PendingKey Key{Handle.Connection, Handle.Request};
		auto Pending = State->PendingRequests.find(Key);
		if (Pending == State->PendingRequests.end()) return false;
		const auto Remote = Pending->second.Remote;
		State->CompletePending(Key, TerminalResult(Handle.Request, RemoteRequestTerminalStatus::Cancelled));
		RemoteMessage Message{.Kind = RemoteMessageKind::Cancellation, .Remote = Remote, .Request = Handle.Request};
		(void)State->SendMessage(Handle.Connection, std::move(Message), false);
		DrainSchedulerTerminals();
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
		if (Received->Payload.size() > Peer->second.Limits.MaximumDecodedMessageBytes ||
			Peer->second.QueuedDispatchMessages >= Peer->second.Limits.MaximumMessagesPerTick ||
			Received->Payload.size() > Peer->second.Limits.MaximumReceiveBytesPerTick ||
			Peer->second.QueuedDispatchBytes >
				Peer->second.Limits.MaximumReceiveBytesPerTick - Received->Payload.size() ||
			State->DispatchQueue.size() >= MaximumQueuedRemoteDispatchMessages ||
			Received->Payload.size() > MaximumQueuedRemoteDispatchBytes ||
			State->Metrics.QueuedDispatchBytes > MaximumQueuedRemoteDispatchBytes - Received->Payload.size()) {
			SaturatingIncrement(State->Metrics.ResourceRejections);
			return false;
		}
		auto Decoded = DecodeRemoteMessage(Received->Payload);
		if (!Decoded) {
			SaturatingIncrement(State->Metrics.ProtocolRejections);
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
		State->Metrics.QueuedDispatchBytes += Received->Payload.size();
		Peer->second.QueuedDispatchBytes += Received->Payload.size();
		++Peer->second.QueuedDispatchMessages;
		State->DispatchQueue.push_back({Received->Connection, std::move(*Decoded), Received->Payload.size()});
		State->Metrics.QueuedDispatchMessages = State->DispatchQueue.size();
		return true;
	}

	std::size_t RemoteManager::Pump(std::size_t MaximumMessages) {
		if (!State->Active || MaximumMessages == 0 || MaximumMessages > NativeMaximumNetworkMessagesPerTick) return 0;
		const auto Now = State->GetTime();
		std::vector<Implementation::PendingKey> TimedOut;
		for (const auto &[Key, Pending] : State->PendingRequests)
			if (Now >= Pending.Deadline) TimedOut.push_back(Key);
		for (const auto &Key : TimedOut) {
			auto Pending = State->PendingRequests.find(Key);
			if (Pending == State->PendingRequests.end()) continue;
			const auto Remote = Pending->second.Remote;
			State->CompletePending(Key, TerminalResult(Key.Request, RemoteRequestTerminalStatus::Timeout));
			RemoteMessage Cancellation{
				.Kind = RemoteMessageKind::Cancellation, .Remote = Remote, .Request = Key.Request
			};
			(void)State->SendMessage(Key.Connection, std::move(Cancellation), false);
		}

		std::vector<Implementation::PendingKey> IncomingReplyTimedOut;
		std::vector<Implementation::PendingKey> IncomingWorkExpired;
		for (const auto &[Key, Incoming] : State->IncomingRequests) {
			if (Incoming.AcceptReply && Now >= Incoming.ReplyDeadline) IncomingReplyTimedOut.push_back(Key);
			if (Now >= Incoming.WorkDeadline) IncomingWorkExpired.push_back(Key);
		}
		for (const auto &Key : IncomingReplyTimedOut) {
			auto Incoming = State->IncomingRequests.find(Key);
			if (Incoming == State->IncomingRequests.end()) continue;
			Incoming->second.AcceptReply = false;
			const auto Remote = Incoming->second.Remote;
			RemoteMessage Error{
				.Kind = RemoteMessageKind::RequestError,
				.Remote = Remote,
				.Request = Key.Request,
				.Error = StructuredRemoteError{"handler_timeout", "Remote handler deadline expired"}
			};
			(void)State->SendMessage(Key.Connection, std::move(Error), false);
		}
		for (const auto &Key : IncomingWorkExpired) {
			auto Incoming = State->IncomingRequests.find(Key);
			if (Incoming == State->IncomingRequests.end()) continue;
			State->IncomingRequests.erase(Incoming);
			auto Peer = State->Peers.find(Key.Connection);
			if (Peer != State->Peers.end() && Peer->second.ConcurrentHandlers != 0) --Peer->second.ConcurrentHandlers;
		}

		for (auto Iterator = State->Deferred.begin(); Iterator != State->Deferred.end();) {
			auto Peer = State->Peers.find(Iterator->Connection);
			const Implementation::PendingKey RequestKey{Iterator->Connection, Iterator->Message.Request};
			const bool LiveRequest = Iterator->Message.Kind != RemoteMessageKind::Request ||
									 (State->PendingRequests.contains(RequestKey) &&
									  State->PendingRequests.find(RequestKey)->second.Remote ==
										  Iterator->Message.Remote);
			const bool Ready = LiveRequest && Peer != State->Peers.end() &&
							   State->RemoteAvailable(Iterator->Connection, Iterator->Message.Remote) &&
							   std::ranges::all_of(Iterator->Dependencies, [&](ObjectId Object) {
								   return Peer->second.MaterializedObjects.contains(Object);
							   });
			if (LiveRequest && !Ready && Now < Iterator->Deadline) {
				++Iterator;
				continue;
			}
			const auto Bytes = Iterator->Encoded.size();
			if (Ready)
				(void)State->SubmitEncoded(
					Iterator->Connection, Iterator->Message, std::move(Iterator->Encoded), false
				);
			else
				SaturatingIncrement(State->Metrics.ResourceRejections);
			if (Peer != State->Peers.end()) Peer->second.DeferredReliableBytes -= Bytes;
			State->Metrics.DeferredReliableBytes -= Bytes;
			Iterator = State->Deferred.erase(Iterator);
		}
		State->Metrics.DeferredReliableMessages = State->Deferred.size();

		std::unordered_set<ConnectionId> ActivePeers;
		for (const auto &Queued : State->DispatchQueue)
			ActivePeers.insert(Queued.Connection);
		const auto PerPeerLimit = std::max<std::size_t>(
			1, MaximumMessages / std::max<std::size_t>(1, ActivePeers.size())
		);
		std::unordered_map<ConnectionId, std::size_t> ProcessedByPeer;
		std::size_t Processed = 0;
		std::size_t Candidates = State->DispatchQueue.size();
		while (Processed < MaximumMessages && Candidates-- != 0 && !State->DispatchQueue.empty()) {
			auto Queued = std::move(State->DispatchQueue.front());
			State->DispatchQueue.pop_front();
			if (ProcessedByPeer[Queued.Connection] >= PerPeerLimit) {
				State->DispatchQueue.push_back(std::move(Queued));
				continue;
			}
			const auto QueuedConnection = Queued.Connection;
			State->Metrics.QueuedDispatchBytes -= Queued.EncodedBytes;
			if (auto Peer = State->Peers.find(Queued.Connection); Peer != State->Peers.end()) {
				--Peer->second.QueuedDispatchMessages;
				Peer->second.QueuedDispatchBytes -= Queued.EncodedBytes;
			}
			State->Dispatch(std::move(Queued));
			++ProcessedByPeer[QueuedConnection];
			++Processed;
		}
		State->Metrics.QueuedDispatchMessages = State->DispatchQueue.size();
		DrainSchedulerTerminals();
		return Processed;
	}

	RemoteMetrics RemoteManager::GetMetrics() const {
		return State->Metrics;
	}
	RemoteManagerRole RemoteManager::GetRole() const {
		return State->Role;
	}
	std::shared_ptr<Instance> RemoteManager::ResolveObject(ObjectId Object) const {
		return State->ResolveObject ? State->ResolveObject(Object) : nullptr;
	}
	std::weak_ptr<void> RemoteManager::GetLifetimeToken() const {
		return State;
	}
}

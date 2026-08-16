#pragma once

#include "gargantuan/network/RemoteProtocol.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/Transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace gargantuan {
	class Instance;
}

namespace gargantuan::network {
	inline constexpr std::uint32_t MaximumRemoteCallsPerPeerPerSecond = 1024;
	inline constexpr std::uint32_t MaximumRemoteCallsPerRemotePerSecond = 256;
	inline constexpr std::uint32_t MaximumConcurrentRemoteHandlersPerPeer = 64;
	inline constexpr std::size_t MaximumRemotePeers = 512;
	inline constexpr std::size_t MaximumQueuedRemoteDispatchMessages = 8192;
	inline constexpr std::size_t MaximumQueuedRemoteDispatchBytes = 32 * 1024 * 1024;
	inline constexpr std::size_t MaximumRemoteBroadcastSubmissionsPerSecond = 4096;
	inline constexpr std::size_t MaximumRemoteBroadcastBytesPerSecond = 16 * 1024 * 1024;

	enum class RemoteManagerRole : std::uint8_t { Server, Client };

	enum class RemoteSendStatus : std::uint8_t {
		Accepted,
		DeferredForMaterialization,
		DroppedUnreliable,
		InvalidPeer,
		UnknownRemote,
		UnpublishedRemote,
		WrongRemoteKind,
		InvalidArguments,
		InvisibleReference,
		ReferenceNotMaterialized,
		RateLimited,
		RequestLimitExceeded,
		SchedulerRejected,
	};

	struct RemoteSendResult {
		RemoteSendStatus Status = RemoteSendStatus::SchedulerRejected;
		std::optional<RemoteRequestId> Request;
		[[nodiscard]] bool Accepted() const {
			return Status == RemoteSendStatus::Accepted || Status == RemoteSendStatus::DeferredForMaterialization;
		}
	};

	[[nodiscard]] std::string_view GetRemoteSendStatusName(RemoteSendStatus Status);

	struct RemotePeerContext {
		ConnectionId Connection;
		ReplicationEpoch Epoch;
	};

	struct RemoteInvocation {
		RemotePeerContext Peer;
		ObjectId Remote;
		std::vector<WireValue> Arguments;
	};

	struct RemoteRequestResult {
		RemoteRequestOutcome Outcome;
		std::vector<WireValue> Results;
	};

	struct RemoteRequestHandle {
		ConnectionId Connection;
		RemoteRequestId Request;
		auto operator<=>(const RemoteRequestHandle &) const = default;
	};

	struct RemoteMetrics {
		std::uint64_t ReliableEventsAccepted = 0;
		std::uint64_t ReliableEventsRejected = 0;
		std::uint64_t UnreliableEventsAccepted = 0;
		std::uint64_t UnreliableEventsDropped = 0;
		std::uint64_t SequencedEventsAccepted = 0;
		std::uint64_t SequencedEventsSuperseded = 0;
		std::uint64_t SequencedEventsStaleRejected = 0;
		std::uint64_t RequestsStarted = 0;
		std::uint64_t RequestsCompleted = 0;
		std::uint64_t RequestsTimedOut = 0;
		std::uint64_t RequestsCancelled = 0;
		std::uint64_t HandlerErrors = 0;
		std::uint64_t RateLimitRejections = 0;
		std::uint64_t VisibilityRejections = 0;
		std::uint64_t ProtocolRejections = 0;
		std::uint64_t ResourceRejections = 0;
		std::uint64_t BroadcastInvocations = 0;
		std::uint64_t BroadcastPeerSubmissions = 0;
		std::size_t QueuedDispatchMessages = 0;
		std::size_t QueuedDispatchBytes = 0;
		std::size_t DeferredReliableMessages = 0;
		std::size_t DeferredReliableBytes = 0;
		std::size_t InFlightRequests = 0;
	};

	class RemoteManager {
	  public:
		using VisibilityCheck = std::function<bool(ConnectionId, ObjectId)>;
		using ObjectResolver = std::function<std::shared_ptr<Instance>(ObjectId)>;
		using Clock = std::function<std::chrono::steady_clock::time_point()>;
		using EventHandler = std::function<void(const RemoteInvocation &)>;
		using RequestReply = std::function<bool(std::vector<WireValue>, std::optional<StructuredRemoteError>)>;
		using RequestHandler = std::function<void(const RemoteInvocation &, RequestReply)>;
		using RequestCompletion = std::function<void(RemoteRequestResult)>;

		RemoteManager(
			RemoteManagerRole Role,
			INetworkScheduler &Scheduler,
			VisibilityCheck IsVisible,
			ObjectResolver ResolveObject,
			Clock GetTime = {}
		);
		~RemoteManager();

		bool AddPeer(ConnectionId Connection, ReplicationEpoch Epoch, NetworkLimits Limits);
		bool RemovePeer(ConnectionId Connection);
		bool RegisterRemote(ObjectId Remote, RemoteInstanceKind Kind);
		bool UnregisterRemote(ObjectId Remote);
		bool PublishRemote(ConnectionId Connection, ObjectId Remote, bool Materialized = true);
		bool UnpublishRemote(ConnectionId Connection, ObjectId Remote);
		bool MarkMaterialized(ConnectionId Connection, ObjectId Object);
		bool MarkUnmaterialized(ConnectionId Connection, ObjectId Object);

		bool SetEventHandler(ObjectId Remote, EventHandler Handler);
		bool SetRequestHandler(ObjectId Remote, RequestHandler Handler);

		RemoteSendResult SendEvent(ConnectionId Connection, ObjectId Remote, std::vector<WireValue> Arguments);
		std::vector<RemoteSendResult> Broadcast(ObjectId Remote, std::vector<WireValue> Arguments);
		RemoteSendResult StartRequest(
			ConnectionId Connection,
			ObjectId Remote,
			std::vector<WireValue> Arguments,
			RequestCompletion Completion,
			std::chrono::milliseconds Deadline = DefaultRemoteRequestDeadline
		);
		bool CancelRequest(RemoteRequestHandle Handle);

		bool HandleTransportEvent(const TransportEvent &Event);
		std::size_t Pump(std::size_t MaximumMessages = NativeMaximumNetworkMessagesPerTick);
		[[nodiscard]] RemoteMetrics GetMetrics() const;
		[[nodiscard]] RemoteManagerRole GetRole() const;
		[[nodiscard]] std::shared_ptr<Instance> ResolveObject(ObjectId Object) const;
		[[nodiscard]] std::weak_ptr<void> GetLifetimeToken() const;

	  private:
		struct Implementation;
		std::shared_ptr<Implementation> State;
	};
}

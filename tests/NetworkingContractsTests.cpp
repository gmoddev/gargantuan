#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Delivery.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/network/Outcome.hpp"
#include "gargantuan/network/Remote.hpp"
#include "gargantuan/network/Replication.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/network/Statistics.hpp"
#include "gargantuan/network/Transport.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <type_traits>
#include <unordered_map>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (!Condition) {
			std::cerr << "FAIL: " << Message << '\n';
			++Failures;
		}
	}

	gargantuan::network::NetworkLimits SmallLimits() {
		return {
			.MaximumReliableMessageBytes = 1024,
			.MaximumUnreliableMessageBytes = 512,
			.MaximumQueuedReliableBytes = 4096,
			.MaximumInFlightRemoteRequests = 32,
			.MaximumDecodedMessageBytes = 2048,
			.MaximumSendBytesPerTick = 4096,
			.MaximumReceiveBytesPerTick = 4096,
			.MaximumMessagesPerTick = 128,
		};
	}
}

int main() {
	using namespace gargantuan;
	using namespace gargantuan::network;

	static_assert(!std::is_convertible_v<ReplicationEpoch, ReliableReplicationSequence>);
	static_assert(!std::is_convertible_v<ReliableReplicationSequence, RealtimeStateSequence>);
	static_assert(!std::is_convertible_v<RealtimeStateSequence, RemoteEventSequence>);
	static_assert(!std::is_convertible_v<RealtimeStateSequence, CharacterInputSequence>);
	static_assert(!std::is_convertible_v<CharacterInputSequence, CharacterActionSequence>);
	static_assert(!std::is_convertible_v<CharacterActionSequence, CharacterControlEpoch>);
	static_assert(!std::is_convertible_v<RemoteEventSequence, RemoteRequestId>);
	static_assert(!std::is_convertible_v<RemotePublicationId, RemoteEventSequence>);
	static_assert(!std::is_constructible_v<RemoteRequestId, ReplicationEpoch>);
	static_assert(!std::is_pointer_v<ConnectionId> && std::is_trivially_copyable_v<ConnectionId>);
	static_assert(std::is_abstract_v<IGameTransport>);

	ConnectionId InvalidConnection;
	ConnectionId FirstConnection{4, 1};
	ConnectionId ReusedConnection{4, 2};
	Check(!InvalidConnection.IsValid() && FirstConnection.IsValid(), "connection identity has an explicit invalid state");
	Check(FirstConnection != ReusedConnection, "a reused connection slot cannot alias a stale generation");
	std::unordered_map<ConnectionId, int> Connections{{FirstConnection, 1}, {ReusedConnection, 2}};
	Check(Connections.size() == 2, "connection identity is a stable map key");

	Check(IsLegalConnectionTransition(ConnectionState::Connecting, ConnectionState::Authenticating),
		"connecting may advance to authentication");
	Check(IsLegalConnectionTransition(ConnectionState::Authenticating, ConnectionState::Connected),
		"authentication may advance to connected");
	Check(IsLegalConnectionTransition(ConnectionState::Connected, ConnectionState::Closing) &&
		IsLegalConnectionTransition(ConnectionState::Closing, ConnectionState::Closed),
		"connected shutdown follows closing to closed");
	Check(!IsLegalConnectionTransition(ConnectionState::Connected, ConnectionState::Authenticating) &&
		!IsLegalConnectionTransition(ConnectionState::Closed, ConnectionState::Connecting) &&
		IsTerminalConnectionState(ConnectionState::Closed), "illegal lifecycle reversal and terminal state fail closed");

	Check(DeliveryMode::ReliableOrdered != DeliveryMode::UnreliableUnordered &&
		DeliveryMode::UnreliableUnordered != DeliveryMode::UnreliableSequenced &&
		RequiresSequenceMetadata(DeliveryMode::UnreliableSequenced) &&
		!MayDropUnderCongestion(DeliveryMode::ReliableOrdered), "delivery requirements remain semantically distinct");
	Check(!IsValidDeliveryMode(static_cast<DeliveryMode>(255)) &&
		!IsValidTrafficClass(static_cast<TrafficClass>(255)), "unknown delivery and traffic enum values fail closed");

	auto FirstLimits = SmallLimits();
	auto SecondLimits = SmallLimits();
	SecondLimits.MaximumReliableMessageBytes = 768;
	SecondLimits.MaximumUnreliableMessageBytes = 256;
	SecondLimits.MaximumQueuedReliableBytes = 2048;
	SecondLimits.MaximumInFlightRemoteRequests = 8;
	SecondLimits.MaximumDecodedMessageBytes = 1536;
	SecondLimits.MaximumSendBytesPerTick = 2048;
	SecondLimits.MaximumReceiveBytesPerTick = 3072;
	SecondLimits.MaximumMessagesPerTick = 64;
	auto Negotiated = NegotiateNetworkLimits(FirstLimits, SecondLimits);
	Check(Negotiated && Negotiated->IsValid() && Negotiated->MaximumReliableMessageBytes == 768 &&
		Negotiated->MaximumUnreliableMessageBytes == 256 && Negotiated->MaximumInFlightRemoteRequests == 8,
		"network limits negotiate to the component-wise safe minimum");
	auto InvalidLimits = FirstLimits;
	InvalidLimits.MaximumReliableMessageBytes = 0;
	Check(!InvalidLimits.IsValid() && !NegotiateNetworkLimits(FirstLimits, InvalidLimits),
		"zero or invalid advertised limits cannot participate in negotiation");
	InvalidLimits = FirstLimits;
	InvalidLimits.MaximumReliableMessageBytes = 256;
	InvalidLimits.MaximumSendBytesPerTick = InvalidLimits.MaximumUnreliableMessageBytes - 1;
	Check(!InvalidLimits.IsValid(),
		"a per-tick send budget cannot advertise an unreliable message it cannot submit");

	ReplicationEpoch Epoch(1);
	auto NextEpoch = Epoch.TryNext();
	Check(NextEpoch && NextEpoch->IsNewerThan(Epoch), "strong sequences compare monotonically without wrap");
	Check(!ReplicationEpoch(std::numeric_limits<std::uint64_t>::max()).TryNext(),
		"sequence exhaustion fails closed instead of wrapping");

	NetworkStatistics UnavailableStatistics;
	NetworkStatistics ZeroStatistics;
	ZeroStatistics.BytesSent = 0;
	ZeroStatistics.EstimatedRoundTripTime = std::chrono::microseconds(0);
	Check(UnavailableStatistics.IsValid() && ZeroStatistics.IsValid() &&
		!UnavailableStatistics.BytesSent && ZeroStatistics.BytesSent.has_value(),
		"unavailable metrics remain distinct from measured zero");
	UnavailableStatistics.MessageLossRatio = std::numeric_limits<double>::infinity();
	Check(!UnavailableStatistics.IsValid(), "invalid loss estimates fail validation");

	for (auto Reason : std::array{
		DisconnectReason::LocalShutdown, DisconnectReason::RemoteShutdown, DisconnectReason::Timeout,
		DisconnectReason::AuthenticationFailure, DisconnectReason::ProtocolViolation,
		DisconnectReason::ResourceExhaustion, DisconnectReason::TransportFailure,
		DisconnectReason::IncompatibleVersion
	}) Check(IsTerminalDisconnectReason(Reason), "every structured disconnect reason is terminal");
	Check(!IsTerminalDisconnectReason(static_cast<DisconnectReason>(255)) &&
		!DisconnectInfo{static_cast<DisconnectReason>(255), {}}.IsValid(),
		"unknown disconnect reasons fail closed");

	const ObjectId Object{10, 2};
	ReplicationView View{
		.Connection = FirstConnection,
		.Epoch = Epoch,
		.KnownObjects = {Object},
		.RelevantObjects = {Object},
		.LatestStateSequences = {
			{ReplicaStateChannel{Object, StateChannelId(1)}, RealtimeStateSequence(3)},
			{ReplicaStateChannel{Object, StateChannelId(2)}, RealtimeStateSequence(7)},
		},
	};
	Check(View.IsValid() && View.Knows(Object) && View.LatestStateSequences.size() == 2,
		"replication view stores independent realtime sequences per object and state channel");
	auto InvalidView = View;
	InvalidView.LatestStateSequences.emplace(
		ReplicaStateChannel{Object, StateChannelId()}, RealtimeStateSequence(8));
	Check(!InvalidView.IsValid(), "replication view rejects an invalid state-channel identity");
	View.ForgetReplica(Object);
	Check(!View.Knows(Object) && View.RelevantObjects.contains(Object) &&
		View.LatestStateSequences.empty() && Object.IsValid(),
		"forgetting a replica clears every channel sequence without destroying identity or relevance intent");
	ReplicationOperation Unpublish{
		.Epoch = Epoch,
		.Intent = UnpublishReplication{Object},
	};
	Check(Unpublish.IsValid() && std::holds_alternative<UnpublishReplication>(Unpublish.Intent),
		"replica unpublish is a distinct semantic operation, not authoritative destroy");
	ReplicationOperation InvalidOperation{
		.Epoch = Epoch,
		.Intent = ReparentReplication{{}, std::nullopt},
	};
	Check(!InvalidOperation.IsValid(), "replication operations reject invalid ObjectIds");
	ReplicationOperation CustomProperty{
		.Epoch = Epoch,
		.Intent = PropertyReplicationUpdate{
			.Object = Object,
			.PropertyName = "Health",
			.Value = 90,
			.DeclaringClassSchemaId = SchemaId::FromCustomClassName("Game", "CombatFolder"),
			.DefinitionVersion = 4,
		},
	};
	Check(CustomProperty.IsValid(), "replication property intent retains stable custom schema identity and version");

	for (auto Status : std::array{
		RemoteRequestTerminalStatus::Success, RemoteRequestTerminalStatus::Timeout,
		RemoteRequestTerminalStatus::Cancelled, RemoteRequestTerminalStatus::Disconnected,
		RemoteRequestTerminalStatus::RemoteError, RemoteRequestTerminalStatus::ProtocolRejected,
		RemoteRequestTerminalStatus::ResourceRejected
	}) {
		RemoteRequestOutcome Outcome{.Request = RemoteRequestId(1), .Status = Status};
		if (Status == RemoteRequestTerminalStatus::RemoteError)
			Outcome.Error = StructuredRemoteError{"RemoteFailure", "handler rejected the request"};
		Check(Outcome.IsTerminal() && Outcome.IsValid(FirstLimits), "every remote request outcome is terminal and structured");
	}
	RemoteRequestOutcome InvalidRequest{.Request = RemoteRequestId(), .Status = RemoteRequestTerminalStatus::Timeout};
	Check(!InvalidRequest.IsValid(FirstLimits), "remote request identity zero is invalid");
	RemoteRequestOutcome UnknownRequestOutcome{
		.Request = RemoteRequestId(1),
		.Status = static_cast<RemoteRequestTerminalStatus>(255),
	};
	Check(!UnknownRequestOutcome.IsValid(FirstLimits), "unknown remote terminal outcomes fail closed");

	std::vector<std::byte> ReliablePayload(FirstLimits.MaximumReliableMessageBytes, std::byte{1});
	Check(MakeNetworkMessageIntent(
		FirstConnection, DeliveryMode::ReliableOrdered, TrafficClass::StructuralReplication,
		ReliableReplicationOrder{ReliableReplicationSequence(1)}, ReliablePayload, FirstLimits
	).has_value(), "bounded reliable scheduling intent is accepted");
	std::vector<std::byte> OversizedPayload(FirstLimits.MaximumUnreliableMessageBytes + 1, std::byte{1});
	Check(!MakeNetworkMessageIntent(
		FirstConnection, DeliveryMode::UnreliableUnordered, TrafficClass::EphemeralApplication,
		std::monostate{}, std::move(OversizedPayload), FirstLimits
	), "oversized unreliable intent is rejected against validated limits");
	Check(!MakeNetworkMessageIntent(
		FirstConnection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
		std::monostate{}, {std::byte{1}}, FirstLimits
	), "sequenced delivery requires explicit semantic sequence metadata");
	Check(MakeNetworkMessageIntent(
		FirstConnection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
		RealtimeStateOrder{StateChannelId(1), RealtimeStateSequence(7)}, {std::byte{1}}, FirstLimits
	).has_value(), "newest-wins state intent carries a distinct state channel and sequence");

	TransportStartConfiguration Start{
		.Role = TransportRole::Server,
		.Endpoint = {"127.0.0.1", 7777},
		.AdvertisedLimits = FirstLimits,
	};
	Check(Start.IsValid(), "transport startup uses backend-neutral endpoint, role, limits, and opaque handshake material");
	TransportEvent LegalState = ConnectionStateEvent{
		.Connection = FirstConnection,
		.Previous = ConnectionState::Connecting,
		.Current = ConnectionState::Authenticating,
	};
	TransportEvent IllegalState = ConnectionStateEvent{
		.Connection = FirstConnection,
		.Previous = ConnectionState::Closed,
		.Current = ConnectionState::Connected,
	};
	Check(IsValidTransportEvent(LegalState, FirstLimits) && !IsValidTransportEvent(IllegalState, FirstLimits),
		"transport lifecycle events enforce the same legal state machine");
	TransportEvent OversizedReceive = ReceivedMessageEvent{
		.Connection = FirstConnection,
		.Delivery = DeliveryMode::UnreliableUnordered,
		.Traffic = TrafficClass::EphemeralApplication,
		.Order = std::monostate{},
		.Payload = std::vector<std::byte>(FirstLimits.MaximumUnreliableMessageBytes + 1),
	};
	Check(!IsValidTransportEvent(OversizedReceive, FirstLimits),
		"received events enforce delivery-specific negotiated message ceilings");

	if (Failures == 0) std::cout << "All networking contract tests passed\n";
	return Failures == 0 ? 0 : 1;
}

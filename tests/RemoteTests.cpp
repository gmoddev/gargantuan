#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	NetworkLimits TestLimits(std::uint32_t Requests = 16) {
		return {
			.MaximumReliableMessageBytes = MaximumRemoteFrameBytes,
			.MaximumUnreliableMessageBytes = 1200,
			.MaximumQueuedReliableBytes = 1024 * 1024,
			.MaximumInFlightRemoteRequests = Requests,
			.MaximumDecodedMessageBytes = MaximumRemoteFrameBytes,
			.MaximumSendBytesPerTick = 1024 * 1024,
			.MaximumReceiveBytesPerTick = 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
		};
	}

	std::vector<TransportEvent> Drain(const std::shared_ptr<SimulatedTransport> &Transport) {
		std::vector<TransportEvent> Result;
		std::array<TransportEvent, 256> Buffer;
		for (;;) {
			const auto Count = Transport->PollEvents(Buffer);
			if (Count == 0) break;
			for (std::size_t Index = 0; Index < Count; ++Index)
				Result.push_back(std::move(Buffer[Index]));
		}
		return Result;
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events) {
			const auto *State = std::get_if<ConnectionStateEvent>(&Event);
			if (State && State->Current == ConnectionState::Connected) return State->Connection;
		}
		return {};
	}

	struct RecordingScheduler final : INetworkScheduler {
		std::vector<NetworkMessageIntent> Messages;
		std::optional<SchedulerSubmitResult> NextSubmitResult;
		bool RegisterConnection(ConnectionId, const NetworkLimits &) override {
			return true;
		}
		SchedulerSubmitResult Submit(NetworkMessageIntent Intent) override {
			Messages.push_back(std::move(Intent));
			if (NextSubmitResult) {
				auto Result = std::move(*NextSubmitResult);
				NextSubmitResult.reset();
				return Result;
			}
			return {SchedulerSubmitStatus::Accepted};
		}
		SchedulerFlushResult Flush(ConnectionId, SchedulerTickBudget) override {
			return {SchedulerFlushStatus::Drained};
		}
		bool CancelConnection(ConnectionId) override {
			return true;
		}
		std::optional<SchedulerStatistics> GetStatistics(ConnectionId) const override {
			return std::nullopt;
		}
	};

	struct RemoteFixture {
		std::shared_ptr<SimulatedNetwork> Network;
		std::shared_ptr<SimulatedTransport> ServerTransport;
		std::shared_ptr<SimulatedTransport> ClientTransport;
		std::unique_ptr<NetworkScheduler> ServerScheduler;
		std::unique_ptr<NetworkScheduler> ClientScheduler;
		std::unique_ptr<RemoteManager> Server;
		std::unique_ptr<RemoteManager> Client;
		NetworkLimits Limits;
		ConnectionId ServerConnection;
		ConnectionId ClientConnection;
		std::chrono::steady_clock::time_point Time{};
		std::set<ObjectId> Visible;
		std::shared_ptr<Folder> ReferencedObject;

		explicit RemoteFixture(
			SimulatedTransportConfiguration Configuration = {}, NetworkLimits NewLimits = TestLimits()
		)
			: Limits(NewLimits) {
			Network = SimulatedNetwork::Create(Configuration);
			ServerTransport = Network->CreateTransport();
			ClientTransport = Network->CreateTransport();
			Check(
				ServerTransport->Start({TransportRole::Server, {"remotes", 1}, Limits, {}}).Succeeded(),
				"simulated Remote server starts"
			);
			Check(
				ClientTransport->Start({TransportRole::Client, {"remotes", 1}, Limits, {}}).Succeeded(),
				"simulated Remote client starts"
			);
			Network->Pump();
			ServerConnection = ConnectedId(Drain(ServerTransport));
			ClientConnection = ConnectedId(Drain(ClientTransport));
			Check(ServerConnection.IsValid() && ClientConnection.IsValid(), "simulated Remote peers connect");
			ServerScheduler = std::make_unique<NetworkScheduler>(*ServerTransport);
			ClientScheduler = std::make_unique<NetworkScheduler>(*ClientTransport);
			Check(
				ServerScheduler->RegisterConnection(ServerConnection, Limits), "server scheduler registers Remote peer"
			);
			Check(
				ClientScheduler->RegisterConnection(ClientConnection, Limits), "client scheduler registers Remote peer"
			);
			auto Visibility = [this](ConnectionId, ObjectId Object) { return Visible.contains(Object); };
			auto Resolver = [this](ObjectId Object) -> std::shared_ptr<Instance> {
				return ReferencedObject && ReferencedObject->GetObjectId() == Object ? ReferencedObject : nullptr;
			};
			auto Clock = [this] { return Time; };
			Server = std::make_unique<RemoteManager>(
				RemoteManagerRole::Server, *ServerScheduler, Visibility, Resolver, Clock
			);
			Client = std::make_unique<RemoteManager>(
				RemoteManagerRole::Client, *ClientScheduler, Visibility, Resolver, Clock
			);
			Check(Server->AddPeer(ServerConnection, ReplicationEpoch(1), Limits), "server RemoteManager adds peer");
			Check(Client->AddPeer(ClientConnection, ReplicationEpoch(1), Limits), "client RemoteManager adds peer");
		}

		void Register(ObjectId Remote, RemoteInstanceKind Kind) {
			Visible.insert(Remote);
			Check(
				Server->RegisterRemote(Remote, Kind) && Client->RegisterRemote(Remote, Kind),
				"both RemoteManagers register semantic Remote identity"
			);
			Check(
				Server->PublishRemote(ServerConnection, Remote) && Client->PublishRemote(ClientConnection, Remote),
				"both peers materialize the Remote identity"
			);
		}

		void Cycle(std::chrono::milliseconds Step = 5ms) {
			(void)ServerScheduler->Flush(ServerConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			(void)ClientScheduler->Flush(ClientConnection, SchedulerTickBudget::FromNetworkLimits(Limits));
			Time += Step;
			(void)Network->Advance(Step);
			Network->Pump();
			for (const auto &Event : Drain(ServerTransport))
				(void)Server->HandleTransportEvent(Event);
			for (const auto &Event : Drain(ClientTransport))
				(void)Client->HandleTransportEvent(Event);
			(void)Server->Pump();
			(void)Client->Pump();
		}

		void SetReferenceVisible(bool Materialized = true) {
			ReferencedObject = std::make_shared<Folder>();
			const auto Object = ReferencedObject->GetObjectId();
			Visible.insert(Object);
			if (Materialized) {
				Check(Server->MarkMaterialized(ServerConnection, Object), "server reference becomes materialized");
				Check(Client->MarkMaterialized(ClientConnection, Object), "client reference becomes materialized");
			}
		}
	};

	RemoteMessage Message(RemoteMessageKind Kind, ObjectId Remote) {
		RemoteMessage Result{.Kind = Kind, .Remote = Remote};
		if (Kind == RemoteMessageKind::SequencedEvent) Result.Sequence = RemoteEventSequence(1);
		if (Kind == RemoteMessageKind::Request) {
			Result.Request = RemoteRequestId(1);
			Result.Deadline = 100ms;
		} else if (Kind == RemoteMessageKind::Response || Kind == RemoteMessageKind::RequestError ||
				   Kind == RemoteMessageKind::Cancellation)
			Result.Request = RemoteRequestId(1);
		if (Kind == RemoteMessageKind::RequestError)
			Result.Error = StructuredRemoteError{"handler_error", "bounded error"};
		return Result;
	}

	ReceivedMessageEvent Incoming(ConnectionId Connection, const RemoteMessage &MessageValue) {
		auto Bytes = EncodeRemoteMessage(MessageValue);
		if (!Bytes) std::abort();
		DeliveryMode Delivery = DeliveryMode::ReliableOrdered;
		TrafficClass Traffic = TrafficClass::ReliableApplication;
		MessageOrder Order;
		if (MessageValue.Kind == RemoteMessageKind::UnreliableEvent) {
			Delivery = DeliveryMode::UnreliableUnordered;
			Traffic = TrafficClass::EphemeralApplication;
		} else if (MessageValue.Kind == RemoteMessageKind::SequencedEvent) {
			Delivery = DeliveryMode::UnreliableSequenced;
			Traffic = TrafficClass::RealtimeState;
			const StateChannelId Channel(
				(static_cast<std::uint64_t>(MessageValue.Remote.Generation) << 32) | MessageValue.Remote.Slot
			);
			Order = RemoteEventOrder{Channel, MessageValue.Publication, MessageValue.Sequence};
		}
		return {Connection, Delivery, Traffic, std::move(Order), std::move(*Bytes)};
	}

	void TestCodec() {
		const ObjectId Remote{91, 7};
		for (std::uint8_t Value = 0; Value <= static_cast<std::uint8_t>(RemoteMessageKind::Cancellation); ++Value) {
			auto Original = Message(static_cast<RemoteMessageKind>(Value), Remote);
			if (Original.Kind != RemoteMessageKind::RequestError && Original.Kind != RemoteMessageKind::Cancellation)
				Original.Arguments = {
					std::monostate{},
					true,
					42,
					3.5,
					std::string("hello"),
					WireVector3{1, 2, 3},
					WireObjectReference{{2, 3}}
				};
			auto Encoded = EncodeRemoteMessage(Original);
			Check(Encoded.has_value(), "every Remote opcode encodes");
			if (!Encoded) continue;
			auto Decoded = DecodeRemoteMessage(*Encoded);
			Check(
				Decoded && Decoded->Kind == Original.Kind && Decoded->Arguments == Original.Arguments,
				"every Remote opcode round trips independently of backend memory layout"
			);
			for (std::size_t Boundary = 0; Boundary < std::min<std::size_t>(52, Encoded->size()); ++Boundary)
				Check(
					!DecodeRemoteMessage(std::span<const std::byte>(*Encoded).first(Boundary)),
					"every truncated Remote header boundary fails closed"
				);
		}

		auto Reliable = EncodeRemoteMessage(Message(RemoteMessageKind::ReliableEvent, Remote));
		Check(Reliable.has_value(), "valid Remote corpus seed encodes");
		if (!Reliable) return;
		auto InvalidVersion = *Reliable;
		InvalidVersion[4] = std::byte{3};
		Check(!DecodeRemoteMessage(InvalidVersion), "unknown Remote protocol versions fail closed");
		auto InvalidOpcode = *Reliable;
		InvalidOpcode[6] = std::byte{0xff};
		Check(!DecodeRemoteMessage(InvalidOpcode), "unknown Remote opcodes fail closed");
		auto InvalidCount = *Reliable;
		InvalidCount[44] = std::byte{33};
		Check(!DecodeRemoteMessage(InvalidCount), "excessive Remote argument counts fail before allocation");
		auto Trailing = *Reliable;
		Trailing.push_back(std::byte{0});
		Check(!DecodeRemoteMessage(Trailing), "Remote trailing garbage fails closed");
		std::vector<std::byte> Oversized(MaximumRemoteFrameBytes + 1);
		Check(!DecodeRemoteMessage(Oversized), "one byte over the Remote frame maximum fails before parsing");
		auto Tagged = Message(RemoteMessageKind::ReliableEvent, Remote);
		Tagged.Arguments = {42};
		auto TaggedBytes = EncodeRemoteMessage(Tagged);
		Check(TaggedBytes.has_value(), "tagged malformed-input corpus seed encodes");
		if (TaggedBytes) {
			(*TaggedBytes)[52] = std::byte{0xff};
			Check(!DecodeRemoteMessage(*TaggedBytes), "invalid WireValue tags fail before application dispatch");
			TaggedBytes->pop_back();
			Check(!DecodeRemoteMessage(*TaggedBytes), "truncated Remote argument fails closed");
		}
		auto InvalidReference = Message(RemoteMessageKind::ReliableEvent, Remote);
		InvalidReference.Arguments = {WireObjectReference{{55, 0}}};
		Check(!EncodeRemoteMessage(InvalidReference), "invalid ObjectId generation fails before encoding");
		auto RequestBytes = EncodeRemoteMessage(Message(RemoteMessageKind::Request, Remote));
		if (RequestBytes) {
			for (std::size_t Index = 24; Index < 32; ++Index)
				(*RequestBytes)[Index] = std::byte{0};
			Check(!DecodeRemoteMessage(*RequestBytes), "malformed zero RequestId fails closed");
		}
		auto SequenceBytes = EncodeRemoteMessage(Message(RemoteMessageKind::SequencedEvent, Remote));
		if (SequenceBytes) {
			for (std::size_t Index = 32; Index < 40; ++Index)
				(*SequenceBytes)[Index] = std::byte{0};
			Check(!DecodeRemoteMessage(*SequenceBytes), "malformed zero sequence metadata fails closed");
		}
		auto PublicationBytes = EncodeRemoteMessage(Message(RemoteMessageKind::ReliableEvent, Remote));
		if (PublicationBytes) {
			for (std::size_t Index = 16; Index < 24; ++Index)
				(*PublicationBytes)[Index] = std::byte{0};
			Check(!DecodeRemoteMessage(*PublicationBytes), "malformed zero publication lifetime fails closed");
		}

		auto MaximumString = Message(RemoteMessageKind::ReliableEvent, Remote);
		MaximumString.Arguments = {std::string(MaximumRemoteStringBytes, 'x')};
		Check(EncodeRemoteMessage(MaximumString).has_value(), "maximum bounded Remote string encodes");
		MaximumString.Arguments = {std::string(MaximumRemoteStringBytes + 1, 'x')};
		Check(!EncodeRemoteMessage(MaximumString), "oversized Remote strings fail before queueing");
		auto InvalidUtf8 = Message(RemoteMessageKind::ReliableEvent, Remote);
		InvalidUtf8.Arguments = {std::string("\xc3\x28", 2)};
		Check(!EncodeRemoteMessage(InvalidUtf8), "invalid UTF-8 Remote strings fail before queueing");
		auto OversizedError = Message(RemoteMessageKind::RequestError, Remote);
		OversizedError.Error->Message.assign(MaximumRemoteErrorMessageBytes + 1, 'x');
		Check(!EncodeRemoteMessage(OversizedError), "oversized untrusted error responses are rejected");
	}

	void TestReliableAndRequests() {
		RemoteFixture Fixture;
		const ObjectId Event{101, 1};
		const ObjectId Function{102, 1};
		const ObjectId NestedFunction{103, 1};
		Fixture.Register(Event, RemoteInstanceKind::ReliableEvent);
		Fixture.Register(Function, RemoteInstanceKind::Function);
		Fixture.Register(NestedFunction, RemoteInstanceKind::Function);
		std::vector<int> Received;
		Fixture.Server->SetEventHandler(Event, [&](const RemoteInvocation &Invocation) {
			Received.push_back(std::get<int>(Invocation.Arguments.front()));
		});
		for (int Value = 0; Value < 20; ++Value)
			Check(
				Fixture.Client->SendEvent(Fixture.ClientConnection, Event, {Value}).Accepted(),
				"reliable RemoteEvent is admitted"
			);
		for (int Index = 0; Index < 10; ++Index)
			Fixture.Cycle();
		Check(Received.size() == 20, "reliable RemoteEvent delivers every healthy-connection message");
		bool Ordered = true;
		for (int Value = 0; Value < static_cast<int>(Received.size()); ++Value)
			Ordered &= Received[Value] == Value;
		Check(Ordered, "reliable RemoteEvent preserves its logical-channel order");

		Fixture.Server->SetRequestHandler(
			Function, [](const RemoteInvocation &Invocation, RemoteManager::RequestReply Reply) {
				Reply(
					{Invocation.Arguments.empty() ? 1 : std::get<int>(Invocation.Arguments.front()) + 1}, std::nullopt
				);
			}
		);
		std::optional<RemoteRequestResult> Completed;
		auto Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{41},
			[&](RemoteRequestResult Result) { Completed = std::move(Result); },
			500ms
		);
		Check(Started.Accepted() && Started.Request, "bounded reliable RemoteFunction request starts with an ID");
		for (int Index = 0; Index < 10 && !Completed; ++Index)
			Fixture.Cycle();
		Check(
			Completed && Completed->Outcome.Status == RemoteRequestTerminalStatus::Success &&
				std::get<int>(Completed->Results.front()) == 42,
			"RemoteFunction request correlates a successful response"
		);

		Fixture.Client->SetRequestHandler(
			NestedFunction, [](const RemoteInvocation &Invocation, RemoteManager::RequestReply Reply) {
				Reply({std::get<int>(Invocation.Arguments.front()) + 1}, std::nullopt);
			}
		);
		Fixture.Server->SetRequestHandler(
			Function, [&](const RemoteInvocation &Invocation, RemoteManager::RequestReply OuterReply) {
				auto Nested = Fixture.Server->StartRequest(
					Fixture.ServerConnection,
					NestedFunction,
					Invocation.Arguments,
					[OuterReply](RemoteRequestResult Result) mutable {
						if (Result.Outcome.Status == RemoteRequestTerminalStatus::Success)
							OuterReply({std::get<int>(Result.Results.front()) + 1}, std::nullopt);
						else
							OuterReply({}, StructuredRemoteError{"nested_failed", "Nested request failed"});
					},
					500ms
				);
				if (!Nested.Accepted())
					OuterReply({}, StructuredRemoteError{"nested_rejected", "Nested request rejected"});
			}
		);
		Completed.reset();
		Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{5},
			[&](RemoteRequestResult Result) { Completed = std::move(Result); },
			1s
		);
		for (int Index = 0; Index < 20 && !Completed; ++Index)
			Fixture.Cycle();
		Check(
			Completed && Completed->Outcome.Status == RemoteRequestTerminalStatus::Success &&
				std::get<int>(Completed->Results.front()) == 7,
			"nested bidirectional requests retain independent bounded bookkeeping"
		);

		Fixture.Server->SetRequestHandler(Function, [](const RemoteInvocation &, RemoteManager::RequestReply) {
			throw std::runtime_error("sensitive native handler detail");
		});
		Completed.reset();
		Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{},
			[&](RemoteRequestResult Result) { Completed = std::move(Result); },
			500ms
		);
		for (int Index = 0; Index < 10 && !Completed; ++Index)
			Fixture.Cycle();
		Check(
			Completed && Completed->Outcome.Status == RemoteRequestTerminalStatus::RemoteError &&
				Completed->Outcome.Error && Completed->Outcome.Error->Code == "handler_error" &&
				Completed->Outcome.Error->Message.find("sensitive") == std::string::npos,
			"handler failure becomes a bounded sanitized remote error"
		);
		Fixture.Server->SetRequestHandler(Function, [](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
			Reply({1}, std::nullopt);
		});

		std::size_t Completions = 0;
		Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(
					Result.Outcome.Status == RemoteRequestTerminalStatus::Timeout,
					"expired RemoteFunction request reports timeout"
				);
			},
			20ms
		);
		Fixture.Time += 21ms;
		Fixture.Client->Pump();
		Check(Completions == 1, "timeout completes a RemoteFunction request exactly once");
		Fixture.Cycle();
		Check(Completions == 1, "late RemoteFunction response cannot complete a timed-out request again");

		Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(
					Result.Outcome.Status == RemoteRequestTerminalStatus::Cancelled,
					"explicit cancellation has a structured terminal outcome"
				);
			},
			500ms
		);
		Check(
			Started.Request && Fixture.Client->CancelRequest({Fixture.ClientConnection, *Started.Request}),
			"active RemoteFunction request can be cancelled"
		);
		Check(Completions == 2, "cancellation resumes exactly once");

		Started = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Function,
			{},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(
					Result.Outcome.Status == RemoteRequestTerminalStatus::Disconnected,
					"disconnect terminates a pending RemoteFunction request"
				);
			},
			500ms
		);
		Check(
			Fixture.Client->HandleTransportEvent(
				DisconnectedEvent{Fixture.ClientConnection, {DisconnectReason::RemoteShutdown, "test disconnect"}}
			),
			"RemoteManager consumes peer disconnect"
		);
		Check(Completions == 3, "disconnect completion runs exactly once");
	}

	void TestUnreliableSequencedVisibilityAndLimits() {
		SimulatedTransportConfiguration Lossy;
		Lossy.UnreliableLossProbability = 1.0;
		std::vector<RemoteRequestTerminalStatus> Terminals;
		RemoteFixture Fixture(Lossy, TestLimits(2));
		const ObjectId Unreliable{201, 1};
		const ObjectId Sequenced{202, 1};
		const ObjectId Function{203, 1};
		const ObjectId ReliableReference{205, 1};
		Fixture.Register(Unreliable, RemoteInstanceKind::UnreliableEvent);
		Fixture.Register(Sequenced, RemoteInstanceKind::UnreliableSequencedEvent);
		Fixture.Register(Function, RemoteInstanceKind::Function);
		Fixture.Register(ReliableReference, RemoteInstanceKind::ReliableEvent);
		int UnreliableDeliveries = 0;
		Fixture.Server->SetEventHandler(Unreliable, [&](const RemoteInvocation &) { ++UnreliableDeliveries; });
		Check(
			Fixture.Client->SendEvent(Fixture.ClientConnection, Unreliable, {1}).Accepted(),
			"best-effort RemoteEvent admission does not imply delivery"
		);
		for (int Index = 0; Index < 4; ++Index)
			Fixture.Cycle();
		Check(UnreliableDeliveries == 0, "simulator loss drops unreliable RemoteEvents without retry");

		int Latest = 0;
		Fixture.Server->SetEventHandler(Sequenced, [&](const RemoteInvocation &Invocation) {
			Latest = std::get<int>(Invocation.Arguments.front());
		});
		auto InjectSequence = [&](std::uint64_t Sequence, int Value) {
			auto Remote = Message(RemoteMessageKind::SequencedEvent, Sequenced);
			Remote.Sequence = RemoteEventSequence(Sequence);
			Remote.Arguments = {Value};
			auto Bytes = EncodeRemoteMessage(Remote);
			const StateChannelId Channel((static_cast<std::uint64_t>(Sequenced.Generation) << 32) | Sequenced.Slot);
			Check(
				Bytes && Fixture.Server->HandleTransportEvent(
							 ReceivedMessageEvent{
								 Fixture.ServerConnection,
								 DeliveryMode::UnreliableSequenced,
								 TrafficClass::RealtimeState,
								 RemoteEventOrder{Channel, Remote.Publication, RemoteEventSequence(Sequence)},
								 std::move(*Bytes)
							 }
						 ),
				"sequenced Remote frame is structurally admitted"
			);
			Fixture.Server->Pump();
		};
		InjectSequence(4, 4);
		InjectSequence(2, 2);
		InjectSequence(4, 44);
		InjectSequence(5, 5);
		Check(
			Latest == 5 && Fixture.Server->GetMetrics().SequencedEventsStaleRejected == 2,
			"sequenced RemoteEvent ignores reordered and duplicate stale state"
		);

		Fixture.SetReferenceVisible(false);
		const auto Object = Fixture.ReferencedObject->GetObjectId();
		bool ResolvedReference = false;
		Fixture.Server->SetEventHandler(ReliableReference, [&](const RemoteInvocation &Invocation) {
			const auto Reference = std::get<WireObjectReference>(Invocation.Arguments.front()).Object.ToObjectId();
			ResolvedReference = Reference == Object && Fixture.Server->ResolveObject(Reference) != nullptr;
		});
		Check(
			Fixture.Client
					->SendEvent(
						Fixture.ClientConnection,
						ReliableReference,
						{WireObjectReference{WireObjectId::FromObjectId(Object)}}
					)
					.Status == RemoteSendStatus::DeferredForMaterialization,
			"reliable Object reference creates only an explicit materialization dependency"
		);
		Check(
			Fixture.Client->MarkMaterialized(Fixture.ClientConnection, Object) &&
				Fixture.Server->MarkMaterialized(Fixture.ServerConnection, Object),
			"replication publication resolves the explicit Remote dependency"
		);
		Fixture.Client->Pump();
		for (int Index = 0; Index < 5 && !ResolvedReference; ++Index)
			Fixture.Cycle();
		Check(ResolvedReference, "reliable handler never observes an unresolved newly published Object reference");
		Check(
			Fixture.Server->MarkUnmaterialized(Fixture.ServerConnection, Object),
			"unpublication removes receiver materialization state"
		);
		Check(
			Fixture.Client->MarkUnmaterialized(Fixture.ClientConnection, Object),
			"sender observes that the Object publication dependency is no longer satisfied"
		);
		Check(
			Fixture.Client
					->SendEvent(
						Fixture.ClientConnection, Unreliable, {WireObjectReference{WireObjectId::FromObjectId(Object)}}
					)
					.Status == RemoteSendStatus::ReferenceNotMaterialized,
			"unreliable Object reference never waits for publication"
		);
		Check(
			Fixture.Client->SendEvent(Fixture.ClientConnection, Sequenced, {WireObjectReference{{9999, 1}}}).Status ==
				RemoteSendStatus::InvisibleReference,
			"guessed hidden ObjectId fails before scheduler submission"
		);

		for (int Index = 0; Index < 3; ++Index) {
			auto Result = Fixture.Client->StartRequest(
				Fixture.ClientConnection,
				Function,
				{},
				[&](RemoteRequestResult Outcome) { Terminals.push_back(Outcome.Outcome.Status); },
				1s
			);
			Check((Index < 2) == Result.Accepted(), "per-peer RemoteFunction in-flight ceiling is enforced");
		}
		Check(
			Fixture.Client->GetMetrics().InFlightRequests == 2,
			"rejected RemoteFunction work does not enter request tracking"
		);

		const ObjectId RateLimited{204, 1};
		Fixture.Register(RateLimited, RemoteInstanceKind::ReliableEvent);
		std::size_t Accepted = 0;
		for (std::uint32_t Index = 0; Index < MaximumRemoteCallsPerRemotePerSecond + 8; ++Index)
			Accepted +=
				Fixture.Client->SendEvent(Fixture.ClientConnection, RateLimited, {static_cast<int>(Index)}).Accepted();
		Check(
			Accepted == MaximumRemoteCallsPerRemotePerSecond,
			"per-peer/per-Remote deterministic rate limit rejects reliable overload"
		);
	}

	void TestLifecycleAndEpochIsolation() {
		{
			RemoteFixture Reentrant;
			const ObjectId ReentrantRemote{300, 1};
			Reentrant.Register(ReentrantRemote, RemoteInstanceKind::Function);
			std::size_t ReentrantCompletions = 0;
			Check(
				Reentrant.Client
					->StartRequest(
						Reentrant.ClientConnection,
						ReentrantRemote,
						{},
						[&](RemoteRequestResult Result) {
							++ReentrantCompletions;
							Check(
								Result.Outcome.Status == RemoteRequestTerminalStatus::Disconnected,
								"reentrant teardown preserves the terminal request outcome"
							);
							Check(
								!Reentrant.Client->RemovePeer(Reentrant.ClientConnection),
								"peer structure is removed before a request completion can reenter teardown"
							);
						},
						1s
					)
					.Accepted(),
				"request starts before reentrant peer teardown"
			);
			Check(
				Reentrant.Client->RemovePeer(Reentrant.ClientConnection),
				"outer peer teardown succeeds without retaining an iterator across callbacks"
			);
			Check(ReentrantCompletions == 1, "reentrant peer teardown completes the request exactly once");
		}

		{
			RecordingScheduler TerminalScheduler;
			const ConnectionId TerminalConnection{75, 1};
			const ObjectId Function{303, 1};
			const ObjectId Event{304, 1};
			std::set<ObjectId> Visible{Function, Event};
			RemoteManager Manager(
				RemoteManagerRole::Server,
				TerminalScheduler,
				[&](ConnectionId, ObjectId Object) { return Visible.contains(Object); },
				[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
			);
			Check(
				Manager.AddPeer(TerminalConnection, ReplicationEpoch(1), TestLimits()) &&
					Manager.RegisterRemote(Function, RemoteInstanceKind::Function) &&
					Manager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent) &&
					Manager.PublishRemote(TerminalConnection, Function) &&
					Manager.PublishRemote(TerminalConnection, Event),
				"terminal scheduler fixture initializes"
			);
			std::size_t PendingDisconnected = 0;
			Check(
				Manager
					.StartRequest(
						TerminalConnection,
						Function,
						{},
						[&](RemoteRequestResult Result) {
							if (Result.Outcome.Status == RemoteRequestTerminalStatus::Disconnected)
								++PendingDisconnected;
						}
					)
					.Accepted(),
				"request is pending before terminal scheduler exhaustion"
			);
			std::size_t TerminalCallbacks = 0;
			Manager.SetTerminalHandler([&](ConnectionId Connection, const DisconnectInfo &Information) {
				Check(
					Connection == TerminalConnection && Information.Reason == DisconnectReason::ResourceExhaustion,
					"terminal scheduler outcome retains connection identity and reason"
				);
				++TerminalCallbacks;
			});
			TerminalScheduler.NextSubmitResult = SchedulerSubmitResult{
				SchedulerSubmitStatus::ReliableBacklogExhausted,
				DisconnectInfo{DisconnectReason::ResourceExhaustion, "test reliable backlog exhausted"},
			};
			auto TerminalSend = Manager.SendEvent(TerminalConnection, Event, {});
			Check(
				TerminalSend.TerminalDisconnect && PendingDisconnected == 1 && TerminalCallbacks == 1 &&
					Manager.SendEvent(TerminalConnection, Event, {}).Status == RemoteSendStatus::InvalidPeer,
				"terminal scheduler exhaustion tears down Remote state and completes pending requests once"
			);
		}

		{
			RecordingScheduler RejectedScheduler;
			const ConnectionId RejectedConnection{76, 1};
			const ObjectId Event{305, 1};
			std::set<ObjectId> Visible{Event};
			RemoteManager Manager(
				RemoteManagerRole::Server,
				RejectedScheduler,
				[&](ConnectionId, ObjectId Object) { return Visible.contains(Object); },
				[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
			);
			Check(
				Manager.AddPeer(RejectedConnection, ReplicationEpoch(1), TestLimits()) &&
					Manager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent) &&
					Manager.PublishRemote(RejectedConnection, Event),
				"nonterminal scheduler-rejection fixture initializes"
			);
			std::size_t TerminalCallbacks = 0;
			Manager.SetTerminalHandler([&](ConnectionId Connection, const DisconnectInfo &Information) {
				if (Connection == RejectedConnection && Information.Reason == DisconnectReason::ResourceExhaustion)
					++TerminalCallbacks;
			});
			RejectedScheduler.NextSubmitResult = {SchedulerSubmitStatus::IntentRejected};
			auto Rejected = Manager.SendEvent(RejectedConnection, Event, {});
			Check(
				Rejected.TerminalDisconnect && TerminalCallbacks == 1 &&
					Manager.SendEvent(RejectedConnection, Event, {}).Status == RemoteSendStatus::InvalidPeer,
				"every rejected reliable Remote admission becomes an owning-peer terminal failure"
			);
		}

		RemoteFixture Fixture;
		const ObjectId Remote{301, 1};
		Fixture.Register(Remote, RemoteInstanceKind::Function);
		std::size_t Completions = 0;
		auto Old = Fixture.Client->StartRequest(
			Fixture.ClientConnection,
			Remote,
			{},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(
					Result.Outcome.Status == RemoteRequestTerminalStatus::Disconnected,
					"old epoch request terminates on disconnect"
				);
			},
			1s
		);
		Check(Old.Accepted(), "old epoch request starts");
		const auto OldConnection = Fixture.ClientConnection;
		Fixture.Client->RemovePeer(OldConnection);
		Check(
			!Fixture.Client->AddPeer(OldConnection, ReplicationEpoch(2), Fixture.Limits),
			"a retired ConnectionId generation cannot be rebound to a newer replication epoch"
		);
		Check(
			Fixture.ClientScheduler->CancelConnection(OldConnection),
			"old epoch scheduler queue is cancelled at the session boundary"
		);
		const ConnectionId NewConnection{OldConnection.Slot, static_cast<std::uint32_t>(OldConnection.Generation + 1)};
		Check(
			Fixture.ClientScheduler->RegisterConnection(NewConnection, Fixture.Limits),
			"new epoch scheduler state is isolated from the stale ConnectionId"
		);
		Check(
			Fixture.Client->AddPeer(NewConnection, ReplicationEpoch(2), Fixture.Limits),
			"new connection generation creates a fresh Remote session"
		);
		Fixture.ClientConnection = NewConnection;
		Check(Fixture.Client->PublishRemote(NewConnection, Remote), "Remote visibility is rebuilt in the new epoch");
		Check(Completions == 1, "old epoch request completed once during reset");
		auto Late = Message(RemoteMessageKind::Response, Remote);
		Late.Request = *Old.Request;
		Late.Arguments = {99};
		auto Bytes = EncodeRemoteMessage(Late);
		Check(
			Bytes && !Fixture.Client->HandleTransportEvent(
						 ReceivedMessageEvent{
							 OldConnection,
							 DeliveryMode::ReliableOrdered,
							 TrafficClass::ReliableApplication,
							 {},
							 std::move(*Bytes)
						 }
					 ),
			"stale ConnectionId cannot inject a late response into the new epoch"
		);
		Check(Completions == 1, "late old response never resumes a new coroutine/request");

		std::optional<RemoteRequestTerminalStatus> UnpublishedStatus;
		Check(
			Fixture.Client
				->StartRequest(
					NewConnection,
					Remote,
					{},
					[&](RemoteRequestResult Result) { UnpublishedStatus = Result.Outcome.Status; },
					1s
				)
				.Accepted(),
			"new epoch request starts before Remote unpublication"
		);
		Check(Fixture.Client->UnpublishRemote(NewConnection, Remote), "unpublication removes Remote authority");
		Check(
			UnpublishedStatus == RemoteRequestTerminalStatus::ProtocolRejected,
			"unpublishing a Remote terminates its pending request exactly once"
		);
		Check(
			Fixture.Client->StartRequest(NewConnection, Remote, {}, [](RemoteRequestResult) {}).Status ==
				RemoteSendStatus::UnpublishedRemote,
			"unpublished Remote fails closed before request tracking"
		);

		const ObjectId RepublishedEvent{302, 1};
		Fixture.Register(RepublishedEvent, RemoteInstanceKind::ReliableEvent);
		std::size_t RepublishedDeliveries = 0;
		Fixture.Client->SetEventHandler(
			RepublishedEvent, [&](const RemoteInvocation &) { ++RepublishedDeliveries; }
		);
		auto OldPublication = Message(RemoteMessageKind::ReliableEvent, RepublishedEvent);
		Check(
			Fixture.Client->UnpublishRemote(NewConnection, RepublishedEvent) &&
				Fixture.Client->PublishRemote(NewConnection, RepublishedEvent),
			"Remote can be republished with a fresh publication lifetime"
		);
		Check(
			Fixture.Client->HandleTransportEvent(Incoming(NewConnection, OldPublication)) &&
				Fixture.Client->Pump() == 1 && RepublishedDeliveries == 0,
			"a delayed frame from the old publication lifetime cannot dispatch after republish"
		);
		auto CurrentPublication = OldPublication;
		CurrentPublication.Publication = RemotePublicationId(2);
		Check(
			Fixture.Client->HandleTransportEvent(Incoming(NewConnection, CurrentPublication)) &&
				Fixture.Client->Pump() == 1 && RepublishedDeliveries == 1,
			"the current publication lifetime remains dispatchable after stale-frame rejection"
		);
		Check(Fixture.Client->UnregisterRemote(Remote), "destroyed Remote identity unregisters");
		Check(
			Fixture.Client->StartRequest(NewConnection, Remote, {}, [](RemoteRequestResult) {}).Status ==
				RemoteSendStatus::UnknownRemote,
			"destroyed Remote identity cannot be invoked"
		);
	}

	void TestBroadcastIsolation() {
		RecordingScheduler Scheduler;
		const ConnectionId PeerA{1, 1};
		const ConnectionId PeerB{2, 1};
		const ObjectId Remote{401, 1};
		auto Referenced = std::make_shared<Folder>();
		const auto Object = Referenced->GetObjectId();
		RemoteManager Manager(
			RemoteManagerRole::Server,
			Scheduler,
			[&](ConnectionId Peer, ObjectId Candidate) {
				return Candidate == Remote || (Peer == PeerA && Candidate == Object);
			},
			[&](ObjectId Candidate) -> std::shared_ptr<Instance> { return Candidate == Object ? Referenced : nullptr; }
		);
		Check(
			Manager.AddPeer(PeerA, ReplicationEpoch(1), TestLimits()) &&
				Manager.AddPeer(PeerB, ReplicationEpoch(1), TestLimits()),
			"broadcast manager tracks peers independently"
		);
		Check(
			Manager.RegisterRemote(Remote, RemoteInstanceKind::ReliableEvent) && Manager.PublishRemote(PeerA, Remote) &&
				Manager.PublishRemote(PeerB, Remote),
			"broadcast Remote is visible to both eligible peers"
		);
		Check(Manager.MarkMaterialized(PeerA, Object), "broadcast Object reference is materialized only for peer A");
		auto Results = Manager.Broadcast(Remote, {WireObjectReference{WireObjectId::FromObjectId(Object)}});
		Check(
			Results.size() == 2 && Results[0].Accepted() && Results[1].Status == RemoteSendStatus::InvisibleReference,
			"broadcast validates visibility per peer and rejects only the ineligible submission"
		);
		Check(
			Scheduler.Messages.size() == 1 && Scheduler.Messages.front().Destination() == PeerA,
			"broadcast never leaks a peer-specific Object reference to another peer"
		);
		auto Metrics = Manager.GetMetrics();
		Check(
			Metrics.BroadcastInvocations == 1 && Metrics.BroadcastPeerSubmissions == 1,
			"broadcast diagnostics distinguish logical invocation from peer submissions"
		);
	}

	void TestTerminalDeferralAndHandlerAdmission() {
		RecordingScheduler Scheduler;
		const ConnectionId Peer{71, 4};
		const ObjectId Function{701, 1};
		auto Referenced = std::make_shared<Folder>();
		const auto Object = Referenced->GetObjectId();
		std::set<ObjectId> Visible{Function, Object};
		auto Time = std::chrono::steady_clock::time_point{};
		auto Limits = TestLimits(1);
		RemoteManager Manager(
			RemoteManagerRole::Client,
			Scheduler,
			[&](ConnectionId, ObjectId Candidate) { return Visible.contains(Candidate); },
			[&](ObjectId Candidate) -> std::shared_ptr<Instance> { return Candidate == Object ? Referenced : nullptr; },
			[&] { return Time; }
		);
		Check(
			Manager.AddPeer(Peer, ReplicationEpoch(1), Limits) &&
				Manager.RegisterRemote(Function, RemoteInstanceKind::Function) && Manager.PublishRemote(Peer, Function),
			"deferred request test publishes one RemoteFunction"
		);
		std::size_t Completions = 0;
		auto Deferred = Manager.StartRequest(
			Peer,
			Function,
			{WireObjectReference{WireObjectId::FromObjectId(Object)}},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(
					Result.Outcome.Status == RemoteRequestTerminalStatus::Cancelled, "deferred cancellation is terminal"
				);
			},
			1s
		);
		Check(
			Deferred.Status == RemoteSendStatus::DeferredForMaterialization && Deferred.Request,
			"request with an unpublished Object dependency is explicitly deferred"
		);
		Check(
			Manager.CancelRequest({Peer, *Deferred.Request}) && Manager.MarkMaterialized(Peer, Object),
			"cancelled deferred request can no longer wait on later materialization"
		);
		Manager.Pump();
		std::size_t SubmittedRequests = 0;
		for (const auto &Intent : Scheduler.Messages) {
			auto Decoded = DecodeRemoteMessage(Intent.Payload());
			if (Decoded && Decoded->Kind == RemoteMessageKind::Request) ++SubmittedRequests;
		}
		Check(Completions == 1 && SubmittedRequests == 0, "cancelled deferred request is never transmitted later");

		Check(Manager.MarkUnmaterialized(Peer, Object), "ordinary Object can become unmaterialized again");
		auto TimedOut = Manager.StartRequest(
			Peer,
			Function,
			{WireObjectReference{WireObjectId::FromObjectId(Object)}},
			[&](RemoteRequestResult Result) {
				++Completions;
				Check(Result.Outcome.Status == RemoteRequestTerminalStatus::Timeout, "deferred request times out");
			},
			5ms
		);
		Check(TimedOut.Accepted(), "second materialization-dependent request starts");
		Time += 6ms;
		Manager.Pump();
		Check(Manager.MarkMaterialized(Peer, Object), "dependency may materialize after request timeout");
		Manager.Pump();
		SubmittedRequests = 0;
		for (const auto &Intent : Scheduler.Messages) {
			auto Decoded = DecodeRemoteMessage(Intent.Payload());
			if (Decoded && Decoded->Kind == RemoteMessageKind::Request) ++SubmittedRequests;
		}
		Check(Completions == 2 && SubmittedRequests == 0, "timed-out deferred request is never transmitted later");

		std::optional<RemoteRequestTerminalStatus> Unmaterialized;
		auto Active = Manager.StartRequest(
			Peer, Function, {}, [&](RemoteRequestResult Result) { Unmaterialized = Result.Outcome.Status; }, 1s
		);
		Check(Active.Accepted(), "request starts before Remote unmaterialization");
		Check(Manager.MarkUnmaterialized(Peer, Function), "Remote unmaterialization revokes publication state");
		Check(
			Unmaterialized == RemoteRequestTerminalStatus::ProtocolRejected,
			"Remote unmaterialization terminates pending request state"
		);

		RecordingScheduler ServerScheduler;
		auto ServerTime = std::chrono::steady_clock::time_point{};
		RemoteManager Server(
			RemoteManagerRole::Server,
			ServerScheduler,
			[&](ConnectionId, ObjectId Candidate) { return Candidate == Function; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; },
			[&] { return ServerTime; }
		);
		Check(
			Server.AddPeer(Peer, ReplicationEpoch(1), Limits) &&
				Server.RegisterRemote(Function, RemoteInstanceKind::Function) && Server.PublishRemote(Peer, Function),
			"handler admission test publishes its RemoteFunction"
		);
		std::vector<RemoteManager::RequestReply> HeldReplies;
		int HandlerCalls = 0;
		Server.SetRequestHandler(Function, [&](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
			++HandlerCalls;
			HeldReplies.push_back(std::move(Reply));
		});
		auto First = Message(RemoteMessageKind::Request, Function);
		First.Deadline = 1ms;
		Check(
			Server.HandleTransportEvent(Incoming(Peer, First)) && Server.Pump() == 1,
			"short request starts handler work"
		);
		ServerTime += 2ms;
		Server.Pump();
		auto Second = First;
		Second.Request = RemoteRequestId(2);
		Check(Server.HandleTransportEvent(Incoming(Peer, Second)) && Server.Pump() == 1, "second request is processed");
		Check(HandlerCalls == 1, "timed-out handler retains its concurrency charge until work acknowledges completion");
		Check(!HeldReplies.front()({}, std::nullopt), "late handler acknowledgement cannot send a response");
		auto Third = First;
		Third.Request = RemoteRequestId(3);
		Third.Deadline = 1s;
		Check(
			Server.HandleTransportEvent(Incoming(Peer, Third)) && Server.Pump() == 1 && HandlerCalls == 2,
			"acknowledgement releases one handler slot"
		);
		auto Cancel = Message(RemoteMessageKind::Cancellation, Function);
		Cancel.Request = Third.Request;
		Check(
			Server.HandleTransportEvent(Incoming(Peer, Cancel)) && Server.Pump() == 1,
			"handler cancellation is admitted"
		);
		auto Fourth = Third;
		Fourth.Request = RemoteRequestId(4);
		Check(
			Server.HandleTransportEvent(Incoming(Peer, Fourth)) && Server.Pump() == 1,
			"post-cancellation request is processed"
		);
		Check(HandlerCalls == 2, "cancelled handler retains bounded work admission until acknowledgement");
		Check(!HeldReplies.back()({}, std::nullopt), "cancelled handler acknowledgement is terminal and sanitized");
	}

	void TestDispatchFairnessAndBroadcastBudget() {
		RecordingScheduler Scheduler;
		const ConnectionId Noisy{81, 1};
		const ConnectionId Quiet{82, 1};
		const ObjectId Function{801, 1};
		const ObjectId Event{802, 1};
		RemoteManager Manager(
			RemoteManagerRole::Server,
			Scheduler,
			[&](ConnectionId, ObjectId Object) { return Object == Function || Object == Event; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
		);
		auto Limits = TestLimits();
		Check(
			Manager.AddPeer(Noisy, ReplicationEpoch(1), Limits) &&
				Manager.AddPeer(Quiet, ReplicationEpoch(1), Limits) &&
				Manager.RegisterRemote(Function, RemoteInstanceKind::Function) &&
				Manager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent),
			"fair dispatch manager registers two peers and two Remotes"
		);
		for (const auto Peer : {Noisy, Quiet}) {
			Check(
				Manager.PublishRemote(Peer, Function) && Manager.PublishRemote(Peer, Event),
				"fairness peer publishes Remotes"
			);
		}
		int QuietEvents = 0;
		Manager.SetEventHandler(Event, [&](const RemoteInvocation &Invocation) {
			if (Invocation.Peer.Connection == Quiet) ++QuietEvents;
		});
		auto Forged = Message(RemoteMessageKind::Response, Function);
		Forged.Request = RemoteRequestId(999);
		for (int Index = 0; Index < 20; ++Index)
			Check(
				Manager.HandleTransportEvent(Incoming(Noisy, Forged)),
				"noisy forged terminal frame is structurally queued"
			);
		auto Valid = Message(RemoteMessageKind::ReliableEvent, Event);
		Check(
			Manager.HandleTransportEvent(Incoming(Quiet, Valid)), "quiet peer queues a valid event behind noisy traffic"
		);
		Check(
			Manager.Pump(2) == 2 && QuietEvents == 1,
			"per-peer fair pump dispatches quiet traffic within one bounded pass"
		);

		RecordingScheduler BroadcastScheduler;
		RemoteManager BroadcastManager(
			RemoteManagerRole::Server,
			BroadcastScheduler,
			[&](ConnectionId, ObjectId Object) { return Object == Event; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
		);
		Check(
			BroadcastManager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent),
			"broadcast budget Remote registers"
		);
		for (std::uint32_t Slot = 1; Slot <= 100; ++Slot) {
			const ConnectionId Peer{Slot, 1};
			Check(
				BroadcastManager.AddPeer(Peer, ReplicationEpoch(1), Limits) &&
					BroadcastManager.PublishRemote(Peer, Event),
				"broadcast budget peer registers"
			);
		}
		std::vector<WireValue> LargeArguments;
		for (int Index = 0; Index < 15; ++Index)
			LargeArguments.emplace_back(std::string(MaximumRemoteStringBytes, 'b'));
		auto Results = BroadcastManager.Broadcast(Event, std::move(LargeArguments));
		Check(
			Results.size() == 100 &&
				std::ranges::all_of(
					Results, [](const auto &Result) { return Result.Status == RemoteSendStatus::SchedulerRejected; }
				) &&
				BroadcastScheduler.Messages.empty(),
			"aggregate broadcast budget rejects fanout before allocating per-peer payload copies"
		);

		RecordingScheduler AggregateScheduler;
		RemoteManager AggregateManager(
			RemoteManagerRole::Server,
			AggregateScheduler,
			[&](ConnectionId, ObjectId Object) { return Object == Event; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
		);
		Check(
			AggregateManager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent),
			"aggregate call budget Remote registers"
		);
		std::vector<ConnectionId> AggregatePeers;
		for (std::uint32_t Slot = 200; Slot < 233; ++Slot) {
			const ConnectionId Peer{Slot, 1};
			AggregatePeers.push_back(Peer);
			Check(
				AggregateManager.AddPeer(Peer, ReplicationEpoch(1), Limits) &&
					AggregateManager.PublishRemote(Peer, Event),
				"aggregate call budget peer registers"
			);
		}
		std::size_t AggregateAccepted = 0;
		for (const auto Peer : AggregatePeers)
			for (std::uint32_t Call = 0; Call < MaximumRemoteCallsPerRemotePerSecond; ++Call)
				AggregateAccepted += AggregateManager.SendEvent(Peer, Event, {}).Accepted();
		Check(
			AggregateAccepted == MaximumRemoteCallsPerManagerPerSecond &&
				AggregateScheduler.Messages.size() == MaximumRemoteCallsPerManagerPerSecond,
			"coordinated peers cannot exceed the manager-wide Remote call ceiling"
		);
	}

	void TestRejectionBudgetAndReentrantLifecycle() {
		RecordingScheduler Scheduler;
		const ConnectionId Peer{91, 2};
		const ObjectId Function{901, 1};
		auto Time = std::chrono::steady_clock::time_point{};
		RemoteManager Manager(
			RemoteManagerRole::Server,
			Scheduler,
			[&](ConnectionId, ObjectId Object) { return Object == Function; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; },
			[&] { return Time; }
		);
		auto Limits = TestLimits();
		Check(
			Manager.AddPeer(Peer, ReplicationEpoch(1), Limits) &&
				Manager.RegisterRemote(Function, RemoteInstanceKind::Function) && Manager.PublishRemote(Peer, Function),
			"rejection budget RemoteFunction is published"
		);
		Manager.SetRequestHandler(Function, [](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
			Reply({}, std::nullopt);
		});
		for (std::uint64_t Request = 1; Request <= MaximumRemoteCallsPerRemotePerSecond + 8; ++Request) {
			auto IncomingRequest = Message(RemoteMessageKind::Request, Function);
			IncomingRequest.Request = RemoteRequestId(Request);
			Check(
				Manager.HandleTransportEvent(Incoming(Peer, IncomingRequest)),
				"request flood frame is structurally admitted"
			);
			Manager.Pump();
		}
		Check(
			Scheduler.Messages.size() == MaximumRemoteCallsPerRemotePerSecond,
			"over-rate requests cannot amplify into an unthrottled reliable error stream"
		);

		Time += 1s;
		bool Reentered = false;
		Manager.SetRequestHandler(Function, [&](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
			Reentered = Manager.UnregisterRemote(Function);
			Check(!Reply({}, std::nullopt), "destroyed Remote cannot emit a reentrant late reply");
		});
		auto ReentrantRequest = Message(RemoteMessageKind::Request, Function);
		ReentrantRequest.Request = RemoteRequestId(MaximumRemoteCallsPerRemotePerSecond + 9);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, ReentrantRequest)) && Manager.Pump() == 1 && Reentered,
			"handler may unregister its own Remote without invalidating the active callback"
		);
		Check(
			Manager.StartRequest(Peer, Function, {}, [](RemoteRequestResult) {}).Status ==
				RemoteSendStatus::UnknownRemote,
			"reentrant Remote destruction leaves no callable identity"
		);
	}

	void TestHostileDispatch() {
		RecordingScheduler Scheduler;
		const ConnectionId Peer{51, 2};
		const ObjectId Event{501, 1};
		const ObjectId Function{502, 1};
		const ObjectId Hidden{503, 1};
		const ObjectId Unknown{599, 1};
		RemoteManager Manager(
			RemoteManagerRole::Server,
			Scheduler,
			[&](ConnectionId, ObjectId Remote) { return Remote != Hidden && Remote != Unknown; },
			[](ObjectId) -> std::shared_ptr<Instance> { return nullptr; }
		);
		auto Bounded = TestLimits();
		Bounded.MaximumMessagesPerTick = 1;
		Check(Manager.AddPeer(Peer, ReplicationEpoch(1), Bounded), "hostile test peer enters a bounded session");
		Check(
			Manager.RegisterRemote(Event, RemoteInstanceKind::ReliableEvent) &&
				Manager.RegisterRemote(Function, RemoteInstanceKind::Function) &&
				Manager.RegisterRemote(Hidden, RemoteInstanceKind::ReliableEvent),
			"hostile test registers distinct Remote classes"
		);
		Check(
			Manager.PublishRemote(Peer, Event) && Manager.PublishRemote(Peer, Function),
			"hostile test publishes only authorized Remotes"
		);
		int EventCalls = 0;
		int FunctionCalls = 0;
		Manager.SetEventHandler(Event, [&](const RemoteInvocation &) { ++EventCalls; });
		RemoteManager::RequestReply HeldReply;
		Manager.SetRequestHandler(Function, [&](const RemoteInvocation &, RemoteManager::RequestReply Reply) {
			++FunctionCalls;
			HeldReply = std::move(Reply);
		});

		auto UnknownRequest = Message(RemoteMessageKind::Request, Unknown);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, UnknownRequest)) && Manager.Pump() == 1 && FunctionCalls == 0,
			"unknown Remote request fails before gameplay handler dispatch"
		);
		auto HiddenEvent = Message(RemoteMessageKind::ReliableEvent, Hidden);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, HiddenEvent)) && Manager.Pump() == 1 && EventCalls == 0,
			"hidden and unpublished Remote fails before gameplay handler dispatch"
		);
		auto WrongClass = Message(RemoteMessageKind::ReliableEvent, Function);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, WrongClass)) && Manager.Pump() == 1 && FunctionCalls == 0,
			"malformed Remote class/message-kind combination fails closed"
		);

		auto Request = Message(RemoteMessageKind::Request, Function);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, Request)) && Manager.Pump() == 1 && FunctionCalls == 1,
			"first unique request reaches its bounded handler"
		);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, Request)) && Manager.Pump() == 1 && FunctionCalls == 1,
			"replayed duplicate RequestId never creates a second handler"
		);
		auto ForgedResponse = Message(RemoteMessageKind::Response, Function);
		ForgedResponse.Request = RemoteRequestId(999);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, ForgedResponse)) && Manager.Pump() == 1,
			"response to nonexistent RequestId is safely rejected at semantic dispatch"
		);
		Check(
			Manager.GetMetrics().ProtocolRejections != 0,
			"forged/duplicate request traffic is observable in Remote diagnostics"
		);
		Check(HeldReply({}, std::nullopt), "valid held request still completes after hostile replay attempts");

		auto EventMessage = Message(RemoteMessageKind::ReliableEvent, Event);
		Check(
			Manager.HandleTransportEvent(Incoming(Peer, EventMessage)), "first message enters per-peer dispatch queue"
		);
		Check(
			!Manager.HandleTransportEvent(Incoming(Peer, EventMessage)),
			"per-peer decoded-message queue rejects count amplification before allocation grows"
		);
		Manager.Pump();
		for (std::uint32_t Index = 0; Index < MaximumRemoteCallsPerRemotePerSecond + 4; ++Index) {
			Check(
				Manager.HandleTransportEvent(Incoming(Peer, EventMessage)),
				"bounded hostile rate frame remains structurally decodable"
			);
			Manager.Pump();
		}
		Check(
			EventCalls == static_cast<int>(MaximumRemoteCallsPerRemotePerSecond),
			"incoming per-Remote rate ceiling prevents unbounded gameplay handler work"
		);
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Error) {
		std::cerr << "Remote schema bootstrap failed: " << Error.what() << '\n';
		return 1;
	}
	TestCodec();
	TestReliableAndRequests();
	TestUnreliableSequencedVisibilityAndLimits();
	TestLifecycleAndEpochIsolation();
	TestBroadcastIsolation();
	TestTerminalDeferralAndHandlerAdmission();
	TestDispatchFairnessAndBroadcastBudget();
	TestRejectionBudgetAndReentrantLifecycle();
	TestHostileDispatch();
	if (Failures != 0) {
		std::cerr << Failures << " Remote test(s) failed\n";
		return 1;
	}
	std::cout << "Bounded Remote codec, manager, simulator, request, visibility, and epoch tests passed\n";
	return 0;
}

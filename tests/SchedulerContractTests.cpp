#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/SimulatedTransport.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <iostream>
#include <map>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan::network;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (!Condition) {
			std::cerr << "FAIL: " << Message << '\n';
			++Failures;
		}
	}

	NetworkLimits TestLimits(
		std::size_t ReliableBytes = 64,
		std::size_t UnreliableBytes = 32,
		std::size_t ReliableQueueBytes = 256,
		std::size_t TickBytes = 128,
		std::uint32_t TickMessages = 4
	) {
		return {
			.MaximumReliableMessageBytes = ReliableBytes,
			.MaximumUnreliableMessageBytes = UnreliableBytes,
			.MaximumQueuedReliableBytes = ReliableQueueBytes,
			.MaximumInFlightRemoteRequests = 16,
			.MaximumDecodedMessageBytes = std::max(ReliableBytes, UnreliableBytes),
			.MaximumSendBytesPerTick = TickBytes,
			.MaximumReceiveBytesPerTick = TickBytes,
			.MaximumMessagesPerTick = TickMessages,
		};
	}

	std::optional<NetworkMessageIntent> Intent(
		ConnectionId Connection,
		DeliveryMode Delivery,
		TrafficClass Traffic,
		unsigned int Value,
		const NetworkLimits &Limits,
		std::size_t Bytes = 1,
		MessageOrder Order = std::monostate{}
	) {
		std::vector<std::byte> Payload(Bytes, static_cast<std::byte>(Value & 0xff));
		return MakeNetworkMessageIntent(Connection, Delivery, Traffic, std::move(Order), std::move(Payload), Limits);
	}

	class RecordingTransport final : public IGameTransport {
	  public:
		explicit RecordingTransport(ConnectionId Connection) : ActiveConnection(Connection) {
			Statistics.BytesSent = 0;
			Statistics.MessagesSent = 0;
			Statistics.DroppedUnreliableMessages = 0;
		}

		TransportOperationResult Start(const TransportStartConfiguration &) override {
			return {.Status = TransportOperationStatus::Succeeded};
		}

		TransportOperationResult Stop(DisconnectInfo Information) override {
			return {.Status = TransportOperationStatus::Succeeded, .TerminalDisconnect = std::move(Information)};
		}

		TransportOperationResult Disconnect(ConnectionId Connection, DisconnectInfo Information) override {
			if (Connection != ActiveConnection) return {.Status = TransportOperationStatus::InvalidConnection};
			return {.Status = TransportOperationStatus::Succeeded, .TerminalDisconnect = std::move(Information)};
		}

		TransportOperationResult Send(const NetworkMessageIntent &Message) override {
			if (Message.Destination() != ActiveConnection)
				return {.Status = TransportOperationStatus::InvalidConnection};
			if (NextSendStatus != TransportOperationStatus::Succeeded) {
				auto Status = NextSendStatus;
				auto Disconnect = std::move(NextTerminalDisconnect);
				NextSendStatus = TransportOperationStatus::Succeeded;
				return {.Status = Status, .TerminalDisconnect = std::move(Disconnect)};
			}
			SubmittedTraffic.push_back(Message.Traffic());
			SubmittedValues.push_back(std::to_integer<unsigned int>(Message.Payload().front()));
			++*Statistics.MessagesSent;
			*Statistics.BytesSent += Message.Payload().size();
			return {.Status = TransportOperationStatus::Succeeded};
		}

		std::size_t PollEvents(std::span<TransportEvent>) override { return 0; }

		std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Connection) const override {
			return Connection == ActiveConnection ? std::optional<std::size_t>(64) : std::nullopt;
		}

		std::optional<NetworkStatistics> GetStatistics(ConnectionId Connection) const override {
			return Connection == ActiveConnection ? std::optional(Statistics) : std::nullopt;
		}

		ConnectionId ActiveConnection;
		TransportOperationStatus NextSendStatus = TransportOperationStatus::Succeeded;
		std::optional<DisconnectInfo> NextTerminalDisconnect;
		std::vector<TrafficClass> SubmittedTraffic;
		std::vector<unsigned int> SubmittedValues;
		NetworkStatistics Statistics;
	};

	struct SequenceKey {
		std::uint8_t Domain = 0;
		std::uint64_t Channel = 0;
		auto operator<=>(const SequenceKey &) const = default;
	};

	std::optional<std::pair<SequenceKey, std::uint64_t>> SequencedKey(const MessageOrder &Order) {
		if (const auto *Realtime = std::get_if<RealtimeStateOrder>(&Order))
			return std::pair(SequenceKey{1, Realtime->Channel.Value()}, Realtime->Sequence.Value());
		if (const auto *Remote = std::get_if<RemoteEventOrder>(&Order))
			return std::pair(SequenceKey{2, Remote->Channel.Value()}, Remote->Sequence.Value());
		return std::nullopt;
	}

	class SchedulerPolicyHarness final : public INetworkScheduler {
	  public:
		explicit SchedulerPolicyHarness(IGameTransport &Value) : Transport(Value) {}

		bool RegisterConnection(ConnectionId Connection, const NetworkLimits &Limits) override {
			if (!Connection.IsValid() || !Limits.IsValid()) return false;
			for (const auto &[Existing, Value] : Connections)
				if (Existing.Slot == Connection.Slot && Value.Active) return false;
			return Connections.emplace(Connection, ConnectionQueue{.Limits = Limits}).second;
		}

		SchedulerSubmitResult Submit(NetworkMessageIntent Message) override {
			auto Iterator = Connections.find(Message.Destination());
			if (Iterator == Connections.end() || !Iterator->second.Active)
				return {SchedulerSubmitStatus::InvalidConnection};
			auto &Connection = Iterator->second;
			if (!MakeNetworkMessageIntent(Message.Destination(), Message.Delivery(), Message.Traffic(),
				Message.Order(), Message.Payload(), Connection.Limits)) {
				++Connection.Statistics.IntentsRejected;
				return {SchedulerSubmitStatus::IntentRejected};
			}

			const auto Bytes = Message.Payload().size();
			if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
				if (Bytes > Connection.Limits.MaximumQueuedReliableBytes -
					Connection.Statistics.QueuedReliableBytes) {
					++Connection.Statistics.IntentsRejected;
					++Connection.Statistics.ReliableBacklogExhaustions;
					Clear(Connection);
					Connection.Active = false;
					return {
						SchedulerSubmitStatus::ReliableBacklogExhausted,
						DisconnectInfo{DisconnectReason::ResourceExhaustion, "Reliable scheduler backlog exhausted"},
					};
				}
				Queue(Connection, std::move(Message));
				Connection.Statistics.QueuedReliableBytes += Bytes;
				++Connection.Statistics.IntentsAccepted;
				return {SchedulerSubmitStatus::Accepted};
			}

			if (Message.Delivery() == DeliveryMode::UnreliableSequenced) {
				auto Incoming = SequencedKey(Message.Order());
				if (!Incoming) return RejectUnreliable(Connection);
				for (auto &QueueValue : Connection.Queues) {
					for (auto Existing = QueueValue.begin(); Existing != QueueValue.end(); ++Existing) {
						auto Key = SequencedKey(Existing->Order());
						if (!Key || Key->first != Incoming->first) continue;
						if (Incoming->second <= Key->second) return RejectUnreliable(Connection);
						const auto ExistingBytes = Existing->Payload().size();
						if (Bytes > Connection.Limits.MaximumSendBytesPerTick -
							(Connection.Statistics.QueuedUnreliableBytes - ExistingBytes))
							return RejectUnreliable(Connection);
						QueueValue.erase(Existing);
						Connection.Statistics.QueuedUnreliableBytes -= ExistingBytes;
						--Connection.Statistics.QueuedUnreliableMessages;
						--Connection.Statistics.QueuedMessages;
						Queue(Connection, std::move(Message));
						Connection.Statistics.QueuedUnreliableBytes += Bytes;
						++Connection.Statistics.IntentsAccepted;
						++Connection.Statistics.SequencedStatesSuperseded;
						return {SchedulerSubmitStatus::AcceptedWithSupersession};
					}
				}
			}

			if (Connection.Statistics.QueuedUnreliableMessages >= Connection.Limits.MaximumMessagesPerTick ||
				Bytes > Connection.Limits.MaximumSendBytesPerTick - Connection.Statistics.QueuedUnreliableBytes)
				return RejectUnreliable(Connection);
			Queue(Connection, std::move(Message));
			Connection.Statistics.QueuedUnreliableBytes += Bytes;
			++Connection.Statistics.IntentsAccepted;
			return {SchedulerSubmitStatus::Accepted};
		}

		SchedulerFlushResult Flush(ConnectionId ConnectionIdValue, SchedulerTickBudget Budget) override {
			auto Iterator = Connections.find(ConnectionIdValue);
			if (Iterator == Connections.end() || !Iterator->second.Active)
				return {.Status = SchedulerFlushStatus::InvalidConnection};
			auto &Connection = Iterator->second;
			if (!Budget.IsValidFor(Connection.Limits))
				return {.Status = SchedulerFlushStatus::InvalidBudget};

			SchedulerFlushResult Result{.Status = SchedulerFlushStatus::Drained};
			for (auto &QueueValue : Connection.Queues) {
				while (!QueueValue.empty()) {
					const auto &Message = QueueValue.front();
					const auto Bytes = Message.Payload().size();
					if (Result.MessagesSubmitted == Budget.MaximumMessages ||
						Bytes > Budget.MaximumBytes - Result.BytesSubmitted) {
						Result.Status = SchedulerFlushStatus::BudgetLimited;
						++Connection.Statistics.BudgetLimitedFlushes;
						return Result;
					}

					auto Submission = Transport.Send(Message);
					if (Submission.Status == TransportOperationStatus::WouldBlock) {
						Result.Status = SchedulerFlushStatus::TransportBackpressured;
						++Connection.Statistics.TransportBackpressureEvents;
						return Result;
					}
					if (!Submission.Succeeded()) {
						Result.Status = SchedulerFlushStatus::TerminalFailure;
						Result.TerminalDisconnect = Submission.TerminalDisconnect.value_or(DisconnectInfo{
							DisconnectReason::TransportFailure, "Transport rejected scheduler submission"});
						Clear(Connection);
						Connection.Active = false;
						return Result;
					}

					if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
						Connection.Statistics.QueuedReliableBytes -= Bytes;
						--Connection.Statistics.QueuedReliableMessages;
					} else {
						Connection.Statistics.QueuedUnreliableBytes -= Bytes;
						--Connection.Statistics.QueuedUnreliableMessages;
					}
					QueueValue.pop_front();
					--Connection.Statistics.QueuedMessages;
					++Connection.Statistics.MessagesSubmittedToTransport;
					++Result.MessagesSubmitted;
					Result.BytesSubmitted += Bytes;
				}
			}
			return Result;
		}

		bool CancelConnection(ConnectionId Connection) override {
			return Connections.erase(Connection) != 0;
		}

		std::optional<SchedulerStatistics> GetStatistics(ConnectionId Connection) const override {
			auto Iterator = Connections.find(Connection);
			if (Iterator == Connections.end()) return std::nullopt;
			return Iterator->second.Statistics;
		}

	  private:
		struct ConnectionQueue {
			NetworkLimits Limits;
			std::array<std::deque<NetworkMessageIntent>, 6> Queues;
			SchedulerStatistics Statistics;
			bool Active = true;
		};

		static void Queue(ConnectionQueue &Connection, NetworkMessageIntent Message) {
			const auto Precedence = SchedulerTrafficPrecedence(Message.Traffic());
			if (Message.Delivery() == DeliveryMode::ReliableOrdered)
				++Connection.Statistics.QueuedReliableMessages;
			else ++Connection.Statistics.QueuedUnreliableMessages;
			Connection.Queues[*Precedence].push_back(std::move(Message));
			++Connection.Statistics.QueuedMessages;
		}

		static SchedulerSubmitResult RejectUnreliable(ConnectionQueue &Connection) {
			++Connection.Statistics.IntentsRejected;
			++Connection.Statistics.UnreliableMessagesDroppedBeforeTransport;
			return {SchedulerSubmitStatus::DroppedUnreliable};
		}

		static void Clear(ConnectionQueue &Connection) {
			for (auto &QueueValue : Connection.Queues) QueueValue.clear();
			Connection.Statistics.QueuedReliableBytes = 0;
			Connection.Statistics.QueuedReliableMessages = 0;
			Connection.Statistics.QueuedUnreliableBytes = 0;
			Connection.Statistics.QueuedUnreliableMessages = 0;
			Connection.Statistics.QueuedMessages = 0;
		}

		IGameTransport &Transport;
		std::map<ConnectionId, ConnectionQueue> Connections;
	};

	std::vector<TransportEvent> Drain(const std::shared_ptr<SimulatedTransport> &Transport) {
		std::array<TransportEvent, 64> Buffer;
		std::vector<TransportEvent> Result;
		for (;;) {
			const auto Count = Transport->PollEvents(Buffer);
			Result.insert(Result.end(), std::make_move_iterator(Buffer.begin()),
				std::make_move_iterator(Buffer.begin() + static_cast<std::ptrdiff_t>(Count)));
			if (Count < Buffer.size()) return Result;
		}
	}

	ConnectionId ConnectedId(const std::vector<TransportEvent> &Events) {
		for (const auto &Event : Events)
			if (const auto *State = std::get_if<ConnectionStateEvent>(&Event);
				State && State->Current == ConnectionState::Connected) return State->Connection;
		return {};
	}

	std::vector<unsigned int> RunPolicyTrace() {
		const ConnectionId Connection{9, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto Limits = TestLimits(8, 8, 64, 8, 2);
		Scheduler.RegisterConnection(Connection, Limits);
		for (unsigned int Value = 1; Value <= 6; ++Value) {
			auto Message = Intent(Connection, DeliveryMode::ReliableOrdered,
				Value == 6 ? TrafficClass::Control : TrafficClass::Background, Value, Limits);
			Scheduler.Submit(std::move(*Message));
		}
		while (Scheduler.Flush(Connection, SchedulerTickBudget{8, 2}).Status ==
			SchedulerFlushStatus::BudgetLimited) {}
		return Transport.SubmittedValues;
	}
}

int main() {
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	static_assert(std::is_abstract_v<INetworkScheduler>);
	static_assert(!std::is_pointer_v<SchedulerTickBudget>);

	auto Limits = TestLimits();
	auto Budget = SchedulerTickBudget::FromNetworkLimits(Limits);
	Check(Budget.IsValidFor(Limits) && !SchedulerTickBudget{63, 1}.IsValidFor(Limits),
		"scheduler tick budgets are explicit, bounded, and large enough for one valid message");
	Check(SchedulerTrafficPrecedence(TrafficClass::Control) == 0 &&
		SchedulerTrafficPrecedence(TrafficClass::StructuralReplication) == 1 &&
		SchedulerTrafficPrecedence(TrafficClass::Background) == 5 &&
		!SchedulerTrafficPrecedence(static_cast<TrafficClass>(255)),
		"traffic precedence is semantic and unknown classes fail closed");
	Check(SchedulerSubmitResult{SchedulerSubmitStatus::Accepted}.IsValid() &&
		!SchedulerSubmitResult{SchedulerSubmitStatus::ReliableBacklogExhausted}.IsValid(),
		"terminal reliable exhaustion requires a structured resource disconnect");
	Check(!SchedulerFlushResult{
		.Status = SchedulerFlushStatus::InvalidConnection,
		.MessagesSubmitted = 1,
		.BytesSubmitted = 1,
	}.IsValidFor(Budget), "failed flush results cannot claim that work was submitted");

	{
		const ConnectionId Connection{1, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		Check(!Scheduler.RegisterConnection({}, Limits) && Scheduler.RegisterConnection(Connection, Limits) &&
			!Scheduler.RegisterConnection(Connection, Limits) && !Scheduler.RegisterConnection({1, 2}, Limits),
			"connection registration rejects invalid, duplicate, and live same-slot generations");
		auto Stale = Intent({1, 2}, DeliveryMode::ReliableOrdered, TrafficClass::Control, 1, Limits);
		Check(Stale && Scheduler.Submit(std::move(*Stale)).Status == SchedulerSubmitStatus::InvalidConnection,
			"invalid or stale connection identity fails closed at submission");

		auto WiderLimits = TestLimits(128, 64, 256, 128, 4);
		auto Oversized = Intent(Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control,
			1, WiderLimits, 128);
		Check(Oversized && Scheduler.Submit(std::move(*Oversized)).Status == SchedulerSubmitStatus::IntentRejected,
			"an intent constructed under wider limits is revalidated against its registered session");
	}

	{
		const ConnectionId Connection{2, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto PolicyLimits = TestLimits(8, 8, 64, 8, 1);
		Scheduler.RegisterConnection(Connection, PolicyLimits);
		for (auto [Traffic, Value] : std::array{
			std::pair{TrafficClass::Background, 1u},
			std::pair{TrafficClass::ReliableApplication, 2u},
			std::pair{TrafficClass::StructuralReplication, 3u},
			std::pair{TrafficClass::Control, 4u},
		}) {
			auto Message = Intent(Connection, DeliveryMode::ReliableOrdered, Traffic, Value, PolicyLimits);
			Check(Message && Scheduler.Submit(std::move(*Message)).Accepted(), "reliable policy work is admitted");
		}
		for (unsigned int Tick = 0; Tick < 4; ++Tick)
			Scheduler.Flush(Connection, SchedulerTickBudget{8, 1});
		Check(Transport.SubmittedValues == std::vector<unsigned int>({4, 3, 2, 1}),
			"control and structural traffic have deterministic precedence over background work");
		auto Statistics = Scheduler.GetStatistics(Connection);
		Check(Statistics && Statistics->BudgetLimitedFlushes == 3 && Statistics->QueuedMessages == 0,
			"low-priority work remains bounded across budget-limited ticks and drains predictably");
	}

	{
		const ConnectionId Connection{3, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto QueueLimits = TestLimits(4, 4, 8, 4, 1);
		Scheduler.RegisterConnection(Connection, QueueLimits);
		for (unsigned int Value = 1; Value <= 2; ++Value) {
			auto Message = Intent(Connection, DeliveryMode::ReliableOrdered,
				TrafficClass::ReliableApplication, Value, QueueLimits, 4);
			Check(Message && Scheduler.Submit(std::move(*Message)).Accepted(),
				"reliable scheduler queue accepts work within its byte ceiling");
		}
		auto Overflow = Intent(Connection, DeliveryMode::ReliableOrdered,
			TrafficClass::ReliableApplication, 3, QueueLimits, 4);
		auto Result = Scheduler.Submit(std::move(*Overflow));
		Check(Result.Status == SchedulerSubmitStatus::ReliableBacklogExhausted && Result.IsTerminal() &&
			Result.TerminalDisconnect->Reason == DisconnectReason::ResourceExhaustion,
			"hard reliable backlog exhaustion is structured and terminal rather than a silent drop");
		auto Statistics = Scheduler.GetStatistics(Connection);
		Check(Statistics && Statistics->QueuedMessages == 0 && Statistics->QueuedReliableBytes == 0 &&
			Statistics->ReliableBacklogExhaustions == 1,
			"terminal reliable exhaustion clears queued scheduler state safely");
	}

	{
		const ConnectionId Connection{4, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto QueueLimits = TestLimits(8, 8, 64, 8, 2);
		Scheduler.RegisterConnection(Connection, QueueLimits);
		auto Message = Intent(Connection, DeliveryMode::ReliableOrdered,
			TrafficClass::ReliableApplication, 1, QueueLimits);
		Scheduler.Submit(std::move(*Message));
		Transport.NextSendStatus = TransportOperationStatus::WouldBlock;
		auto Backpressure = Scheduler.Flush(Connection, SchedulerTickBudget{8, 2});
		auto Before = Scheduler.GetStatistics(Connection);
		Check(Backpressure.Status == SchedulerFlushStatus::TransportBackpressured && Before &&
			Before->QueuedMessages == 1 && Before->TransportBackpressureEvents == 1,
			"temporary transport backpressure retains accepted reliable work for a later flush");
		auto Drained = Scheduler.Flush(Connection, SchedulerTickBudget{8, 2});
		Check(Drained.Status == SchedulerFlushStatus::Drained && Transport.SubmittedValues ==
			std::vector<unsigned int>({1}), "a later flush drains temporarily backpressured reliable work exactly once");
	}

	{
		const ConnectionId Connection{41, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto QueueLimits = TestLimits(8, 8, 64, 8, 2);
		Scheduler.RegisterConnection(Connection, QueueLimits);
		for (unsigned int Value = 1; Value <= 2; ++Value) {
			auto Message = Intent(Connection, DeliveryMode::ReliableOrdered,
				TrafficClass::ReliableApplication, Value, QueueLimits);
			Scheduler.Submit(std::move(*Message));
		}
		Transport.NextSendStatus = TransportOperationStatus::ResourceExhausted;
		Transport.NextTerminalDisconnect = DisconnectInfo{
			DisconnectReason::ResourceExhaustion, "transport queue failed"};
		auto Failure = Scheduler.Flush(Connection, SchedulerTickBudget{8, 2});
		auto Statistics = Scheduler.GetStatistics(Connection);
		auto Later = Intent(Connection, DeliveryMode::ReliableOrdered,
			TrafficClass::Control, 3, QueueLimits);
		Check(Failure.Status == SchedulerFlushStatus::TerminalFailure && Failure.IsTerminal() && Statistics &&
			Statistics->QueuedMessages == 0 && Statistics->QueuedReliableBytes == 0 &&
			Scheduler.Submit(std::move(*Later)).Status == SchedulerSubmitStatus::InvalidConnection,
			"terminal transport failure clears scheduler state and rejects later work for that generation");
		const ConnectionId Replacement{41, 2};
		Check(Scheduler.RegisterConnection(Replacement, QueueLimits) && Scheduler.CancelConnection(Connection) &&
			Scheduler.GetStatistics(Replacement).has_value(),
			"cancelling stale terminal state cannot remove a newer registered generation in the same slot");
	}

	{
		const ConnectionId Connection{5, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto QueueLimits = TestLimits(8, 8, 64, 8, 2);
		Scheduler.RegisterConnection(Connection, QueueLimits);
		for (unsigned int Value = 1; Value <= 3; ++Value) {
			auto Message = Intent(Connection, DeliveryMode::UnreliableUnordered,
				TrafficClass::EphemeralApplication, Value, QueueLimits);
			auto Result = Scheduler.Submit(std::move(*Message));
			Check((Value <= 2 && Result.Accepted()) ||
				(Value == 3 && Result.Status == SchedulerSubmitStatus::DroppedUnreliable),
				"ordinary unreliable admission drops excess congestion before transport");
		}
		auto Statistics = Scheduler.GetStatistics(Connection);
		auto TransportStatistics = Transport.GetStatistics(Connection);
		Check(Statistics && Statistics->QueuedMessages == 2 && Statistics->QueuedUnreliableMessages == 2 &&
			Statistics->IsValidFor(QueueLimits) &&
			Statistics->UnreliableMessagesDroppedBeforeTransport == 1 && TransportStatistics &&
			TransportStatistics->DroppedUnreliableMessages == 0,
			"scheduler drops remain separate from transport drop statistics and do not accumulate");
	}

	{
		const ConnectionId Connection{6, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto QueueLimits = TestLimits(8, 8, 64, 8, 4);
		Scheduler.RegisterConnection(Connection, QueueLimits);
		auto Older = Intent(Connection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
			1, QueueLimits, 1, RealtimeStateOrder{StateChannelId(7), RealtimeStateSequence(1)});
		auto Newer = Intent(Connection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
			2, QueueLimits, 1, RealtimeStateOrder{StateChannelId(7), RealtimeStateSequence(2)});
		auto Stale = Intent(Connection, DeliveryMode::UnreliableSequenced, TrafficClass::RealtimeState,
			3, QueueLimits, 1, RealtimeStateOrder{StateChannelId(7), RealtimeStateSequence(1)});
		Check(Scheduler.Submit(std::move(*Older)).Accepted() &&
			Scheduler.Submit(std::move(*Newer)).Status == SchedulerSubmitStatus::AcceptedWithSupersession &&
			Scheduler.Submit(std::move(*Stale)).Status == SchedulerSubmitStatus::DroppedUnreliable,
			"newer unsent sequenced state deterministically supersedes older state in the same channel");
		Scheduler.Flush(Connection, SchedulerTickBudget{8, 4});
		auto Statistics = Scheduler.GetStatistics(Connection);
		Check(Transport.SubmittedValues == std::vector<unsigned int>({2}) && Statistics &&
			Statistics->SequencedStatesSuperseded == 1 &&
			Statistics->UnreliableMessagesDroppedBeforeTransport == 1 && Statistics->QueuedMessages == 0 &&
			Statistics->QueuedUnreliableMessages == 0 && Statistics->IsValidFor(QueueLimits),
			"only the newest retained sequence reaches transport and supersession remains observable");
	}

	{
		const ConnectionId Connection{7, 1};
		RecordingTransport Transport(Connection);
		SchedulerPolicyHarness Scheduler(Transport);
		auto BurstLimits = TestLimits(1, 1, 6000, 4096, 4096);
		Scheduler.RegisterConnection(Connection, BurstLimits);
		bool Accepted = true;
		for (unsigned int Value = 0; Value < 5000; ++Value) {
			auto Message = Intent(Connection, DeliveryMode::ReliableOrdered,
				TrafficClass::Background, Value, BurstLimits);
			Accepted = Accepted && Message && Scheduler.Submit(std::move(*Message)).Accepted();
		}
		auto First = Scheduler.Flush(Connection, SchedulerTickBudget{4096, 4096});
		auto Second = Scheduler.Flush(Connection, SchedulerTickBudget{4096, 4096});
		auto Statistics = Scheduler.GetStatistics(Connection);
		Check(Accepted && First.Status == SchedulerFlushStatus::BudgetLimited &&
			First.MessagesSubmitted == 4096 && First.BytesSubmitted == 4096 &&
			Second.Status == SchedulerFlushStatus::Drained && Second.MessagesSubmitted == 904 && Statistics &&
			Statistics->QueuedMessages == 0 && Statistics->QueuedReliableBytes == 0 &&
			Statistics->QueuedReliableMessages == 0 && Statistics->IsValidFor(BurstLimits),
			"a bounded 5000-message producer burst never exceeds per-tick budgets and drains predictably");
	}

	{
		SimulatedTransportConfiguration Configuration;
		Configuration.BaseLatency = 10ms;
		Configuration.BandwidthBytesPerSecond = MaximumSimulatedBandwidthBytesPerSecond;
		auto Network = SimulatedNetwork::Create(Configuration);
		auto Server = Network->CreateTransport();
		auto Client = Network->CreateTransport();
		auto SessionLimits = TestLimits(8, 8, 64, 8, 4);
		Check(Server->Start({TransportRole::Server, {"scheduler-proof", 1}, SessionLimits, {}}).Succeeded() &&
			Client->Start({TransportRole::Client, {"scheduler-proof", 1}, SessionLimits, {}}).Succeeded(),
			"scheduler proof establishes an in-memory transport pair");
		Network->Pump();
		auto ServerConnection = ConnectedId(Drain(Server));
		auto ClientConnection = ConnectedId(Drain(Client));
		SchedulerPolicyHarness Scheduler(*Client);
		Scheduler.RegisterConnection(ClientConnection, SessionLimits);
		auto Message = Intent(ClientConnection, DeliveryMode::ReliableOrdered,
			TrafficClass::Control, 9, SessionLimits);
		Check(Message && Scheduler.Submit(std::move(*Message)).Accepted() && Drain(Server).empty() &&
			Client->GetStatistics(ClientConnection)->MessagesSent == 0,
			"Submit queues semantic intent and does not imply immediate transport submission or delivery");
		auto Flush = Scheduler.Flush(ClientConnection, SchedulerTickBudget{8, 4});
		Check(Flush.Status == SchedulerFlushStatus::Drained && Flush.MessagesSubmitted == 1 &&
			Drain(Server).empty() && Client->GetStatistics(ClientConnection)->MessagesSent == 1,
			"Flush makes eligible work available to transport but is not a remote acknowledgement");
		(void)Network->Advance(11ms);
		Network->Pump();
		bool Received = false;
		for (const auto &Event : Drain(Server))
			if (const auto *MessageEvent = std::get_if<ReceivedMessageEvent>(&Event))
				Received = MessageEvent->Connection == ServerConnection &&
					std::to_integer<unsigned int>(MessageEvent->Payload.front()) == 9;
		Check(Received, "the simulator delivers flushed work only after explicit deterministic time and pump");
	}

	Check(RunPolicyTrace() == RunPolicyTrace(),
		"identical scheduler state, intent order, limits, and ticks produce identical submission traces");

	if (Failures == 0) std::cout << "All scheduler contract tests passed\n";
	return Failures == 0 ? 0 : 1;
}

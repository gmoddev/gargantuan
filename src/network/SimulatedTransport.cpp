#include "gargantuan/network/SimulatedTransport.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace gargantuan::network {
	namespace {
		using SimulatedTime = std::chrono::microseconds;

		struct EndpointKey {
			std::string Host;
			std::uint16_t Port = 0;
			auto operator<=>(const EndpointKey &) const = default;
		};

		struct SequenceKey {
			std::uint8_t Domain = 0;
			std::uint64_t Channel = 0;
			auto operator<=>(const SequenceKey &) const = default;
		};

		struct SimulatedConnection {
			std::uint64_t LinkId = 0;
			ConnectionId RemoteConnection;
			std::uint64_t RemoteTransportId = 0;
			ConnectionState CurrentState = ConnectionState::Connecting;
			NetworkLimits Limits;
			std::size_t ReliableQueueBytes = 0;
			std::uint32_t QueuedUnreliableMessages = 0;
			std::uint64_t UnreliableAttempts = 0;
			SimulatedTime NextSendAvailable{0};
			SimulatedTime LastReliableDelivery{0};
			NetworkStatistics Statistics;
			std::map<SequenceKey, std::uint64_t> LatestReceivedSequences;
		};

		struct SimulatedLink {
			std::uint64_t Id = 0;
			std::uint64_t FirstTransportId = 0;
			ConnectionId FirstConnection;
			std::uint64_t SecondTransportId = 0;
			ConnectionId SecondConnection;
		};

		enum class ScheduledKind : std::uint8_t { Activate, Message, Disconnect };

		struct ScheduledItem {
			ScheduledKind Kind = ScheduledKind::Activate;
			SimulatedTime At{0};
			std::uint64_t LinkId = 0;
			std::uint64_t SourceTransportId = 0;
			ConnectionId SourceConnection;
			std::uint64_t DestinationTransportId = 0;
			ConnectionId DestinationConnection;
			DeliveryMode Delivery = DeliveryMode::ReliableOrdered;
			TrafficClass Traffic = TrafficClass::Control;
			MessageOrder Order;
			std::vector<std::byte> Payload;
			bool Duplicate = false;
			DisconnectInfo Disconnect;
		};

		using ScheduledKey = std::pair<std::int64_t, std::uint64_t>;

		TransportOperationResult Operation(TransportOperationStatus Status) {
			return {.Status = Status};
		}

		TransportOperationResult TerminalOperation(TransportOperationStatus Status, DisconnectInfo Information) {
			return {.Status = Status, .TerminalDisconnect = std::move(Information)};
		}

		bool IsValidProbability(double Probability) {
			return std::isfinite(Probability) && Probability >= 0.0 && Probability <= 1.0;
		}

		bool IsValidDuration(SimulatedTime Duration, bool PermitZero = true) {
			return Duration.count() >= (PermitZero ? 0 : 1) && Duration <= MaximumSimulatedDuration;
		}

		std::optional<SimulatedTime> AddTime(SimulatedTime First, SimulatedTime Second) {
			if (Second.count() < 0 || First.count() > std::numeric_limits<std::int64_t>::max() - Second.count())
				return std::nullopt;
			return SimulatedTime(First.count() + Second.count());
		}

		std::optional<SequenceKey> GetSequenceKey(const MessageOrder &Order, std::uint64_t &Sequence) {
			if (const auto *Realtime = std::get_if<RealtimeStateOrder>(&Order)) {
				Sequence = Realtime->Sequence.Value();
				return SequenceKey{1, Realtime->Channel.Value()};
			}
			if (const auto *Event = std::get_if<RemoteEventOrder>(&Order)) {
				Sequence = Event->Sequence.Value();
				return SequenceKey{2, Event->Channel.Value()};
			}
			return std::nullopt;
		}

		NetworkStatistics NewStatistics(SimulatedTime BaseLatency) {
			NetworkStatistics Statistics;
			Statistics.BytesSent = 0;
			Statistics.BytesReceived = 0;
			Statistics.MessagesSent = 0;
			Statistics.MessagesDelivered = 0;
			Statistics.MessagesReceived = 0;
			Statistics.DroppedUnreliableMessages = 0;
			Statistics.DuplicatedUnreliableMessages = 0;
			Statistics.QueuedReliableBytes = 0;
			if (auto RoundTrip = AddTime(BaseLatency, BaseLatency))
				Statistics.EstimatedRoundTripTime = *RoundTrip;
			Statistics.MessageLossRatio = 0.0;
			return Statistics;
		}
	}

	struct SimulatedTransportState {
		std::uint64_t Id = 0;
		bool Started = false;
		TransportRole Role = TransportRole::Client;
		TransportEndpoint Endpoint;
		NetworkLimits AdvertisedLimits;
		std::map<ConnectionId, SimulatedConnection> Connections;
		std::vector<std::uint32_t> Generations{0};
		std::deque<std::uint32_t> FreeSlots;
		std::deque<TransportEvent> Events;
	};

	struct SimulatedNetworkState {
		explicit SimulatedNetworkState(SimulatedTransportConfiguration Value)
			: Configuration(std::move(Value)), RandomState(Configuration.Seed) {}

		SimulatedTransportConfiguration Configuration;
		SimulatedTime CurrentTime{0};
		std::uint64_t RandomState = 0;
		std::uint64_t NextTransportId = 1;
		std::uint64_t NextLinkId = 1;
		std::uint64_t NextTieBreaker = 1;
		std::map<std::uint64_t, std::weak_ptr<SimulatedTransportState>> Transports;
		std::map<EndpointKey, std::uint64_t> Servers;
		std::map<std::uint64_t, SimulatedLink> Links;
		std::map<ScheduledKey, ScheduledItem> Scheduled;
	};

	namespace {
		std::shared_ptr<SimulatedTransportState> FindTransport(SimulatedNetworkState &Network, std::uint64_t Id) {
			auto Iterator = Network.Transports.find(Id);
			if (Iterator == Network.Transports.end()) return nullptr;
			auto Result = Iterator->second.lock();
			if (!Result) Network.Transports.erase(Iterator);
			return Result;
		}

		std::uint64_t NextRandom(SimulatedNetworkState &Network) {
			Network.RandomState += 0x9e3779b97f4a7c15ull;
			auto Value = Network.RandomState;
			Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
			Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
			return Value ^ (Value >> 31);
		}

		bool ProbabilityHit(SimulatedNetworkState &Network, double Probability) {
			if (Probability <= 0.0) return false;
			if (Probability >= 1.0) return true;
			constexpr double Denominator = 9007199254740992.0;
			const auto Sample = static_cast<double>(NextRandom(Network) >> 11) / Denominator;
			return Sample < Probability;
		}

		SimulatedTime RandomDuration(SimulatedNetworkState &Network, SimulatedTime Maximum) {
			if (Maximum.count() == 0) return SimulatedTime(0);
			const auto Range = static_cast<std::uint64_t>(Maximum.count()) + 1;
			return SimulatedTime(static_cast<std::int64_t>(NextRandom(Network) % Range));
		}

		std::optional<SimulatedTime> TransmissionDuration(
			std::size_t Bytes,
			std::uint64_t BytesPerSecond
		) {
			if (Bytes == 0 || BytesPerSecond == 0) return std::nullopt;
			const auto ByteCount = static_cast<std::uint64_t>(Bytes);
			if (ByteCount > std::numeric_limits<std::uint64_t>::max() / 1'000'000ull) return std::nullopt;
			const auto Scaled = ByteCount * 1'000'000ull;
			const auto Duration = Scaled / BytesPerSecond + (Scaled % BytesPerSecond != 0 ? 1 : 0);
			if (Duration == 0 || Duration > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return std::nullopt;
			return SimulatedTime(static_cast<std::int64_t>(Duration));
		}

		bool HasEventCapacity(
			const SimulatedNetworkState &Network,
			const SimulatedTransportState &Transport,
			std::size_t Required
		) {
			return Required <= Network.Configuration.MaximumPendingEventsPerTransport &&
				Transport.Events.size() <= Network.Configuration.MaximumPendingEventsPerTransport - Required;
		}

		bool QueueEvent(
			const SimulatedNetworkState &Network,
			SimulatedTransportState &Transport,
			TransportEvent Event,
			bool IsMessage
		) {
			if (!HasEventCapacity(Network, Transport, 1)) return false;
			if (IsMessage) {
				const auto ReservedTerminalEvents = Transport.Connections.size() * 2;
				if (ReservedTerminalEvents >= Network.Configuration.MaximumPendingEventsPerTransport ||
					Transport.Events.size() >= Network.Configuration.MaximumPendingEventsPerTransport - ReservedTerminalEvents)
					return false;
			}
			Transport.Events.push_back(std::move(Event));
			return true;
		}

		void RemoveConnectionEvents(SimulatedTransportState &Transport, ConnectionId Connection) {
			std::erase_if(Transport.Events, [&](const TransportEvent &Event) {
				return std::visit([&](const auto &Value) {
					using Type = std::decay_t<decltype(Value)>;
					if constexpr (std::is_same_v<Type, TransportFailureEvent>) return false;
					else return Value.Connection == Connection;
				}, Event);
			});
		}

		std::optional<ConnectionId> AllocateConnection(SimulatedTransportState &Transport) {
			while (!Transport.FreeSlots.empty()) {
				const auto Slot = Transport.FreeSlots.front();
				Transport.FreeSlots.pop_front();
				if (Transport.Generations[Slot] == std::numeric_limits<std::uint32_t>::max()) continue;
				return ConnectionId{Slot, ++Transport.Generations[Slot]};
			}
			if (Transport.Generations.size() > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
			const auto Slot = static_cast<std::uint32_t>(Transport.Generations.size());
			Transport.Generations.push_back(1);
			return ConnectionId{Slot, 1};
		}

		void ReleaseConnection(SimulatedTransportState &Transport, ConnectionId Connection) {
			if (Connection.Slot < Transport.Generations.size() &&
				Transport.Generations[Connection.Slot] == Connection.Generation &&
				Connection.Generation != std::numeric_limits<std::uint32_t>::max())
				Transport.FreeSlots.push_back(Connection.Slot);
		}

		bool Schedule(SimulatedNetworkState &Network, ScheduledItem Item) {
			if (Network.Scheduled.size() >= Network.Configuration.MaximumScheduledEvents ||
				Network.NextTieBreaker == std::numeric_limits<std::uint64_t>::max()) return false;
			const ScheduledKey Key{Item.At.count(), Network.NextTieBreaker++};
			return Network.Scheduled.emplace(Key, std::move(Item)).second;
		}

		bool CanSchedule(const SimulatedNetworkState &Network, std::size_t Count) {
			return Count <= Network.Configuration.MaximumScheduledEvents &&
				Network.Scheduled.size() <= Network.Configuration.MaximumScheduledEvents - Count &&
				Count < std::numeric_limits<std::uint64_t>::max() &&
				Network.NextTieBreaker <= std::numeric_limits<std::uint64_t>::max() - Count;
		}

		bool CanScheduleMessages(const SimulatedNetworkState &Network, std::size_t Count) {
			const auto ReservedControlEvents = static_cast<std::size_t>(Network.Configuration.MaximumConnections) * 2;
			if (ReservedControlEvents >= Network.Configuration.MaximumScheduledEvents) return false;
			const auto MessageCapacity = Network.Configuration.MaximumScheduledEvents - ReservedControlEvents;
			return Count <= MessageCapacity && Network.Scheduled.size() <= MessageCapacity - Count &&
				CanSchedule(Network, Count);
		}

		void CancelScheduledLink(SimulatedNetworkState &Network, std::uint64_t LinkId) {
			std::erase_if(Network.Scheduled, [&](const auto &Entry) { return Entry.second.LinkId == LinkId; });
		}

		DisconnectInfo RemoteDisconnectInformation(const DisconnectInfo &Information) {
			if (Information.Reason == DisconnectReason::LocalShutdown)
				return {DisconnectReason::RemoteShutdown, Information.Diagnostic};
			return Information;
		}

		void CloseOneSide(
			SimulatedNetworkState &Network,
			const std::shared_ptr<SimulatedTransportState> &Transport,
			ConnectionId Connection,
			const DisconnectInfo &Information
		) {
			if (!Transport) return;
			auto Iterator = Transport->Connections.find(Connection);
			if (Iterator == Transport->Connections.end()) return;
			const auto Previous = Iterator->second.CurrentState;
			RemoveConnectionEvents(*Transport, Connection);
			Transport->Connections.erase(Iterator);
			ReleaseConnection(*Transport, Connection);
			if (HasEventCapacity(Network, *Transport, 2)) {
				Transport->Events.push_back(ConnectionStateEvent{Connection, Previous, ConnectionState::Closed});
				Transport->Events.push_back(DisconnectedEvent{Connection, Information});
			}
		}

		void CloseLink(
			SimulatedNetworkState &Network,
			std::uint64_t LinkId,
			std::uint64_t InitiatorTransportId,
			const DisconnectInfo &Information
		) {
			auto LinkIterator = Network.Links.find(LinkId);
			if (LinkIterator == Network.Links.end()) return;
			const auto Link = LinkIterator->second;
			Network.Links.erase(LinkIterator);
			CancelScheduledLink(Network, LinkId);

			auto First = FindTransport(Network, Link.FirstTransportId);
			auto Second = FindTransport(Network, Link.SecondTransportId);
			const bool FirstInitiated = InitiatorTransportId == Link.FirstTransportId;
			const bool SecondInitiated = InitiatorTransportId == Link.SecondTransportId;
			CloseOneSide(Network, First, Link.FirstConnection,
				FirstInitiated || !InitiatorTransportId ? Information : RemoteDisconnectInformation(Information));
			CloseOneSide(Network, Second, Link.SecondConnection,
				SecondInitiated || !InitiatorTransportId ? Information : RemoteDisconnectInformation(Information));
		}

		void UpdateLossRatio(SimulatedConnection &Connection) {
			if (Connection.UnreliableAttempts == 0) {
				Connection.Statistics.MessageLossRatio = 0.0;
				return;
			}
			Connection.Statistics.MessageLossRatio = std::min(1.0,
				static_cast<double>(*Connection.Statistics.DroppedUnreliableMessages) /
				static_cast<double>(Connection.UnreliableAttempts));
		}

		void RecordUnreliableDrop(SimulatedConnection &Connection) {
			++*Connection.Statistics.DroppedUnreliableMessages;
			UpdateLossRatio(Connection);
		}

		void ActivateLink(SimulatedNetworkState &Network, const ScheduledItem &Item) {
			auto LinkIterator = Network.Links.find(Item.LinkId);
			if (LinkIterator == Network.Links.end()) return;
			const auto &Link = LinkIterator->second;
			auto First = FindTransport(Network, Link.FirstTransportId);
			auto Second = FindTransport(Network, Link.SecondTransportId);
			if (!First || !Second) {
				CloseLink(Network, Link.Id, 0, {DisconnectReason::TransportFailure, "Simulated endpoint disappeared"});
				return;
			}
			auto FirstConnection = First->Connections.find(Link.FirstConnection);
			auto SecondConnection = Second->Connections.find(Link.SecondConnection);
			if (FirstConnection == First->Connections.end() || SecondConnection == Second->Connections.end()) return;
			if (FirstConnection->second.CurrentState != ConnectionState::Connecting ||
				SecondConnection->second.CurrentState != ConnectionState::Connecting) return;

			for (auto &[Transport, Connection, State] : {
				std::tuple{First, Link.FirstConnection, &FirstConnection->second},
				std::tuple{Second, Link.SecondConnection, &SecondConnection->second},
			}) {
				Transport->Events.push_back(ConnectionStateEvent{
					Connection, ConnectionState::Connecting, ConnectionState::Authenticating});
				Transport->Events.push_back(ConnectionStateEvent{
					Connection, ConnectionState::Authenticating, ConnectionState::Connected});
				State->CurrentState = ConnectionState::Connected;
			}
		}

		void DeliverMessage(SimulatedNetworkState &Network, const ScheduledItem &Item) {
			auto LinkIterator = Network.Links.find(Item.LinkId);
			if (LinkIterator == Network.Links.end()) return;
			auto Source = FindTransport(Network, Item.SourceTransportId);
			auto Destination = FindTransport(Network, Item.DestinationTransportId);
			if (!Source || !Destination) return;
			auto SourceIterator = Source->Connections.find(Item.SourceConnection);
			auto DestinationIterator = Destination->Connections.find(Item.DestinationConnection);
			if (SourceIterator == Source->Connections.end() || DestinationIterator == Destination->Connections.end() ||
				SourceIterator->second.LinkId != Item.LinkId || DestinationIterator->second.LinkId != Item.LinkId ||
				SourceIterator->second.CurrentState != ConnectionState::Connected ||
				DestinationIterator->second.CurrentState != ConnectionState::Connected) return;

			auto &SourceConnection = SourceIterator->second;
			auto &DestinationConnection = DestinationIterator->second;
			if (Item.Delivery == DeliveryMode::ReliableOrdered) {
				SourceConnection.ReliableQueueBytes -= Item.Payload.size();
				SourceConnection.Statistics.QueuedReliableBytes = SourceConnection.ReliableQueueBytes;
			} else if (SourceConnection.QueuedUnreliableMessages > 0) {
				--SourceConnection.QueuedUnreliableMessages;
			}

			if (Item.Delivery == DeliveryMode::UnreliableSequenced) {
				std::uint64_t Sequence = 0;
				auto Key = GetSequenceKey(Item.Order, Sequence);
				if (!Key) {
					RecordUnreliableDrop(SourceConnection);
					return;
				}
				auto Existing = DestinationConnection.LatestReceivedSequences.find(*Key);
				if (Existing != DestinationConnection.LatestReceivedSequences.end() && Existing->second >= Sequence) {
					RecordUnreliableDrop(SourceConnection);
					return;
				}
				DestinationConnection.LatestReceivedSequences[*Key] = Sequence;
			}

			TransportEvent Event = ReceivedMessageEvent{
				.Connection = Item.DestinationConnection,
				.Delivery = Item.Delivery,
				.Traffic = Item.Traffic,
				.Order = Item.Order,
				.Payload = Item.Payload,
			};
			if (!QueueEvent(Network, *Destination, std::move(Event), true)) {
				if (Item.Delivery == DeliveryMode::ReliableOrdered) {
					CloseLink(Network, Item.LinkId, Item.DestinationTransportId,
						{DisconnectReason::ResourceExhaustion, "Simulated receive queue exhausted"});
				} else {
					RecordUnreliableDrop(SourceConnection);
				}
				return;
			}

			++*SourceConnection.Statistics.MessagesDelivered;
			if (Item.Duplicate) ++*SourceConnection.Statistics.DuplicatedUnreliableMessages;
			++*DestinationConnection.Statistics.MessagesReceived;
			*DestinationConnection.Statistics.BytesReceived += Item.Payload.size();
		}

		void ProcessScheduledItem(SimulatedNetworkState &Network, const ScheduledItem &Item) {
			switch (Item.Kind) {
			case ScheduledKind::Activate:
				ActivateLink(Network, Item);
				break;
			case ScheduledKind::Message:
				DeliverMessage(Network, Item);
				break;
			case ScheduledKind::Disconnect:
				CloseLink(Network, Item.LinkId, Item.SourceTransportId, Item.Disconnect);
				break;
			}
		}

		std::optional<SimulatedTime> ComputeDeliveryTime(
			SimulatedNetworkState &Network,
			SimulatedConnection &Connection,
			std::size_t PayloadBytes,
			bool Reorder
		) {
			auto Transmission = TransmissionDuration(PayloadBytes, Network.Configuration.BandwidthBytesPerSecond);
			if (!Transmission) return std::nullopt;
			const auto Start = std::max(Network.CurrentTime, Connection.NextSendAvailable);
			auto Finish = AddTime(Start, *Transmission);
			if (!Finish) return std::nullopt;
			Connection.NextSendAvailable = *Finish;
			auto At = AddTime(*Finish, Network.Configuration.BaseLatency);
			if (!At) return std::nullopt;
			At = AddTime(*At, RandomDuration(Network, Network.Configuration.MaximumJitter));
			if (!At) return std::nullopt;
			if (Reorder) At = AddTime(*At, RandomDuration(Network, Network.Configuration.MaximumReorderDelay));
			return At;
		}
	}

	bool SimulatedTransportConfiguration::IsValid() const {
		return IsValidDuration(BaseLatency) && IsValidDuration(MaximumJitter) &&
			IsValidDuration(MaximumReorderDelay) &&
			(!ConnectionLifetime || IsValidDuration(*ConnectionLifetime, false)) &&
			IsValidProbability(UnreliableLossProbability) &&
			IsValidProbability(UnreliableDuplicationProbability) &&
			IsValidProbability(UnreliableReorderProbability) &&
			BandwidthBytesPerSecond > 0 && BandwidthBytesPerSecond <= MaximumSimulatedBandwidthBytesPerSecond &&
			MaximumReliableQueueBytes > 0 && MaximumReliableQueueBytes <= NativeMaximumQueuedReliableBytes &&
			MaximumQueuedUnreliableMessages > 0 &&
			MaximumQueuedUnreliableMessages <= NativeMaximumNetworkMessagesPerTick &&
			MaximumUnreliableDatagramBytes > 0 &&
			MaximumUnreliableDatagramBytes <= NativeMaximumUnreliableMessageBytes &&
			MaximumTransports > 0 && MaximumTransports <= MaximumSimulatedTransports &&
			MaximumConnections > 0 && MaximumConnections <= MaximumSimulatedConnections &&
			MaximumScheduledEvents > MaximumConnections * 2 &&
			MaximumScheduledEvents <= MaximumSimulatedScheduledEvents &&
			MaximumPendingEventsPerTransport >= MaximumConnections * 4 &&
			MaximumPendingEventsPerTransport <= MaximumSimulatedPendingEvents;
	}

	std::shared_ptr<SimulatedNetwork> SimulatedNetwork::Create(SimulatedTransportConfiguration Configuration) {
		if (!Configuration.IsValid()) return nullptr;
		return std::shared_ptr<SimulatedNetwork>(
			new SimulatedNetwork(std::make_shared<SimulatedNetworkState>(std::move(Configuration))));
	}

	SimulatedNetwork::SimulatedNetwork(std::shared_ptr<SimulatedNetworkState> Value) : State(std::move(Value)) {}

	std::shared_ptr<SimulatedTransport> SimulatedNetwork::CreateTransport() {
		for (auto Iterator = State->Transports.begin(); Iterator != State->Transports.end();) {
			if (Iterator->second.expired()) Iterator = State->Transports.erase(Iterator);
			else ++Iterator;
		}
		if (State->Transports.size() >= State->Configuration.MaximumTransports ||
			State->NextTransportId == std::numeric_limits<std::uint64_t>::max()) return nullptr;
		auto TransportState = std::make_shared<SimulatedTransportState>();
		TransportState->Id = State->NextTransportId++;
		State->Transports.emplace(TransportState->Id, TransportState);
		return std::shared_ptr<SimulatedTransport>(new SimulatedTransport(State, std::move(TransportState)));
	}

	bool SimulatedNetwork::Advance(std::chrono::microseconds Delta) {
		if (Delta.count() < 0) return false;
		auto Next = AddTime(State->CurrentTime, Delta);
		if (!Next) return false;
		State->CurrentTime = *Next;
		return true;
	}

	std::size_t SimulatedNetwork::Pump() {
		std::size_t Processed = 0;
		while (!State->Scheduled.empty() && State->Scheduled.begin()->first.first <= State->CurrentTime.count()) {
			auto Node = State->Scheduled.extract(State->Scheduled.begin());
			ProcessScheduledItem(*State, Node.mapped());
			++Processed;
		}
		return Processed;
	}

	std::chrono::microseconds SimulatedNetwork::Now() const { return State->CurrentTime; }
	const SimulatedTransportConfiguration &SimulatedNetwork::Configuration() const { return State->Configuration; }

	SimulatedTransport::SimulatedTransport(
		std::shared_ptr<SimulatedNetworkState> NetworkValue,
		std::shared_ptr<SimulatedTransportState> StateValue
	) : Network(std::move(NetworkValue)), State(std::move(StateValue)) {}

	SimulatedTransport::~SimulatedTransport() {
		if (!Network || !State) return;
		std::vector<std::uint64_t> Links;
		for (const auto &[Connection, Value] : State->Connections) Links.push_back(Value.LinkId);
		for (const auto Link : Links)
			CloseLink(*Network, Link, State->Id, {DisconnectReason::TransportFailure, "Simulated transport destroyed"});
		if (State->Started && State->Role == TransportRole::Server)
			Network->Servers.erase(EndpointKey{State->Endpoint.Host, State->Endpoint.Port});
		Network->Transports.erase(State->Id);
	}

	TransportOperationResult SimulatedTransport::Start(const TransportStartConfiguration &Configuration) {
		if (!Configuration.IsValid()) return Operation(TransportOperationStatus::MessageRejected);
		if (State->Started || !State->Connections.empty()) return Operation(TransportOperationStatus::InvalidState);
		if (Network->Configuration.MaximumReliableQueueBytes < Configuration.AdvertisedLimits.MaximumReliableMessageBytes)
			return Operation(TransportOperationStatus::ResourceExhausted);

		const EndpointKey Endpoint{Configuration.Endpoint.Host, Configuration.Endpoint.Port};
		if (Configuration.Role == TransportRole::Server) {
			if (Network->Servers.contains(Endpoint)) return Operation(TransportOperationStatus::InvalidState);
			State->Started = true;
			State->Role = Configuration.Role;
			State->Endpoint = Configuration.Endpoint;
			State->AdvertisedLimits = Configuration.AdvertisedLimits;
			Network->Servers.emplace(Endpoint, State->Id);
			return Operation(TransportOperationStatus::Succeeded);
		}

		auto ServerIterator = Network->Servers.find(Endpoint);
		if (ServerIterator == Network->Servers.end()) return Operation(TransportOperationStatus::TransportFailure);
		auto Server = FindTransport(*Network, ServerIterator->second);
		if (!Server || !Server->Started || Server->Role != TransportRole::Server)
			return Operation(TransportOperationStatus::TransportFailure);
		auto Limits = NegotiateNetworkLimits(Configuration.AdvertisedLimits, Server->AdvertisedLimits);
		if (!Limits || Network->Configuration.MaximumReliableQueueBytes < Limits->MaximumReliableMessageBytes)
			return Operation(TransportOperationStatus::ResourceExhausted);
		const std::size_t RequiredSchedules = Network->Configuration.ConnectionLifetime ? 2 : 1;
		auto TimeoutAt = Network->Configuration.ConnectionLifetime
			? AddTime(Network->CurrentTime, *Network->Configuration.ConnectionLifetime)
			: std::optional<SimulatedTime>(Network->CurrentTime);
		if (!TimeoutAt || !CanSchedule(*Network, RequiredSchedules) ||
			Network->Links.size() >= Network->Configuration.MaximumConnections ||
			!HasEventCapacity(*Network, *State, 4) || !HasEventCapacity(*Network, *Server, 4))
			return Operation(TransportOperationStatus::ResourceExhausted);

		auto ClientConnection = AllocateConnection(*State);
		auto ServerConnection = AllocateConnection(*Server);
		if (!ClientConnection || !ServerConnection ||
			Network->NextLinkId == std::numeric_limits<std::uint64_t>::max()) {
			if (ClientConnection) ReleaseConnection(*State, *ClientConnection);
			if (ServerConnection) ReleaseConnection(*Server, *ServerConnection);
			return Operation(TransportOperationStatus::ResourceExhausted);
		}

		const auto LinkId = Network->NextLinkId++;
		SimulatedConnection ClientValue{
			.LinkId = LinkId,
			.RemoteConnection = *ServerConnection,
			.RemoteTransportId = Server->Id,
			.Limits = *Limits,
			.Statistics = NewStatistics(Network->Configuration.BaseLatency),
		};
		SimulatedConnection ServerValue{
			.LinkId = LinkId,
			.RemoteConnection = *ClientConnection,
			.RemoteTransportId = State->Id,
			.Limits = *Limits,
			.Statistics = NewStatistics(Network->Configuration.BaseLatency),
		};
		State->Connections.emplace(*ClientConnection, std::move(ClientValue));
		Server->Connections.emplace(*ServerConnection, std::move(ServerValue));
		Network->Links.emplace(LinkId, SimulatedLink{
			.Id = LinkId,
			.FirstTransportId = State->Id,
			.FirstConnection = *ClientConnection,
			.SecondTransportId = Server->Id,
			.SecondConnection = *ServerConnection,
		});

		ScheduledItem Activation{.Kind = ScheduledKind::Activate, .At = Network->CurrentTime, .LinkId = LinkId};
		if (!Schedule(*Network, std::move(Activation))) {
			CloseLink(*Network, LinkId, 0, {DisconnectReason::ResourceExhaustion, "Simulated schedule exhausted"});
			return Operation(TransportOperationStatus::ResourceExhausted);
		}
		if (Network->Configuration.ConnectionLifetime) {
			if (!Schedule(*Network, ScheduledItem{
				.Kind = ScheduledKind::Disconnect,
				.At = *TimeoutAt,
				.LinkId = LinkId,
				.Disconnect = {DisconnectReason::Timeout, "Simulated connection lifetime expired"},
			})) {
				CloseLink(*Network, LinkId, 0, {DisconnectReason::ResourceExhaustion, "Simulated schedule exhausted"});
				return Operation(TransportOperationStatus::ResourceExhausted);
			}
		}

		State->Started = true;
		State->Role = Configuration.Role;
		State->Endpoint = Configuration.Endpoint;
		State->AdvertisedLimits = Configuration.AdvertisedLimits;
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult SimulatedTransport::Stop(DisconnectInfo Information) {
		if (!Information.IsValid()) return Operation(TransportOperationStatus::MessageRejected);
		if (!State->Started) return Operation(TransportOperationStatus::Succeeded);
		if (!CanSchedule(*Network, State->Connections.size()))
			return Operation(TransportOperationStatus::ResourceExhausted);
		if (State->Role == TransportRole::Server)
			Network->Servers.erase(EndpointKey{State->Endpoint.Host, State->Endpoint.Port});
		State->Started = false;
		std::vector<std::pair<std::uint64_t, ConnectionId>> Links;
		for (auto &[Connection, Value] : State->Connections) {
			Value.CurrentState = ConnectionState::Closing;
			Links.emplace_back(Value.LinkId, Connection);
		}
		for (const auto &[LinkId, Connection] : Links) {
			(void)Schedule(*Network, ScheduledItem{
				.Kind = ScheduledKind::Disconnect,
				.At = Network->CurrentTime,
				.LinkId = LinkId,
				.SourceTransportId = State->Id,
				.SourceConnection = Connection,
				.Disconnect = Information,
			});
		}
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult SimulatedTransport::Disconnect(ConnectionId Connection, DisconnectInfo Information) {
		if (!Information.IsValid()) return Operation(TransportOperationStatus::MessageRejected);
		auto Iterator = State->Connections.find(Connection);
		if (Iterator == State->Connections.end()) return Operation(TransportOperationStatus::InvalidConnection);
		if (Iterator->second.CurrentState == ConnectionState::Closing)
			return Operation(TransportOperationStatus::Succeeded);
		if (!CanSchedule(*Network, 1)) return Operation(TransportOperationStatus::ResourceExhausted);
		Iterator->second.CurrentState = ConnectionState::Closing;
		(void)Schedule(*Network, ScheduledItem{
			.Kind = ScheduledKind::Disconnect,
			.At = Network->CurrentTime,
			.LinkId = Iterator->second.LinkId,
			.SourceTransportId = State->Id,
			.SourceConnection = Connection,
			.Disconnect = Information,
		});
		return Operation(TransportOperationStatus::Succeeded);
	}

	TransportOperationResult SimulatedTransport::Send(const NetworkMessageIntent &Message) {
		auto Iterator = State->Connections.find(Message.Destination());
		if (Iterator == State->Connections.end()) return Operation(TransportOperationStatus::InvalidConnection);
		auto &Connection = Iterator->second;
		if (Connection.CurrentState != ConnectionState::Connected)
			return Operation(TransportOperationStatus::InvalidState);
		const auto &Payload = Message.Payload();
		const auto MessageLimit = Message.Delivery() == DeliveryMode::ReliableOrdered
			? Connection.Limits.MaximumReliableMessageBytes : Connection.Limits.MaximumUnreliableMessageBytes;
		if (Payload.empty() || Payload.size() > MessageLimit ||
			Payload.size() > Connection.Limits.MaximumDecodedMessageBytes)
			return Operation(TransportOperationStatus::MessageRejected);

		if (Message.Delivery() != DeliveryMode::ReliableOrdered &&
			Payload.size() > std::min(Network->Configuration.MaximumUnreliableDatagramBytes,
				Connection.Limits.MaximumUnreliableMessageBytes))
			return Operation(TransportOperationStatus::MessageRejected);

		if (Message.Delivery() != DeliveryMode::ReliableOrdered) {
			++*Connection.Statistics.MessagesSent;
			*Connection.Statistics.BytesSent += Payload.size();
			++Connection.UnreliableAttempts;
			if (ProbabilityHit(*Network, Network->Configuration.UnreliableLossProbability)) {
				RecordUnreliableDrop(Connection);
				return Operation(TransportOperationStatus::Succeeded);
			}
		}

		const bool Duplicate = Message.Delivery() != DeliveryMode::ReliableOrdered &&
			ProbabilityHit(*Network, Network->Configuration.UnreliableDuplicationProbability);
		const std::uint32_t Copies = Duplicate ? 2 : 1;
		if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
			const auto QueueLimit = std::min(Network->Configuration.MaximumReliableQueueBytes,
				Connection.Limits.MaximumQueuedReliableBytes);
			if (Payload.size() > QueueLimit || Connection.ReliableQueueBytes > QueueLimit - Payload.size() ||
				!CanScheduleMessages(*Network, 1)) {
				DisconnectInfo Information{DisconnectReason::ResourceExhaustion, "Simulated reliable queue exhausted"};
				const auto LinkId = Connection.LinkId;
				Connection.CurrentState = ConnectionState::Closing;
				if (!Schedule(*Network, ScheduledItem{
					.Kind = ScheduledKind::Disconnect,
					.At = Network->CurrentTime,
					.LinkId = LinkId,
					.SourceTransportId = State->Id,
					.SourceConnection = Message.Destination(),
					.Disconnect = Information,
				})) CloseLink(*Network, LinkId, State->Id, Information);
				return TerminalOperation(TransportOperationStatus::ResourceExhausted, std::move(Information));
			}
			++*Connection.Statistics.MessagesSent;
			*Connection.Statistics.BytesSent += Payload.size();
		} else if (Copies > Network->Configuration.MaximumQueuedUnreliableMessages -
			Connection.QueuedUnreliableMessages || !CanScheduleMessages(*Network, Copies)) {
			RecordUnreliableDrop(Connection);
			return Operation(TransportOperationStatus::Succeeded);
		}

		std::vector<ScheduledItem> Items;
		Items.reserve(Copies);
		for (std::uint32_t Index = 0; Index < Copies; ++Index) {
			const bool Reorder = Message.Delivery() != DeliveryMode::ReliableOrdered &&
				ProbabilityHit(*Network, Network->Configuration.UnreliableReorderProbability);
			auto DeliveryTime = ComputeDeliveryTime(*Network, Connection, Payload.size(), Reorder);
			if (!DeliveryTime) return Operation(TransportOperationStatus::ResourceExhausted);
			if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
				*DeliveryTime = std::max(*DeliveryTime, Connection.LastReliableDelivery);
				Connection.LastReliableDelivery = *DeliveryTime;
			}
			Items.push_back(ScheduledItem{
				.Kind = ScheduledKind::Message,
				.At = *DeliveryTime,
				.LinkId = Connection.LinkId,
				.SourceTransportId = State->Id,
				.SourceConnection = Message.Destination(),
				.DestinationTransportId = Connection.RemoteTransportId,
				.DestinationConnection = Connection.RemoteConnection,
				.Delivery = Message.Delivery(),
				.Traffic = Message.Traffic(),
				.Order = Message.Order(),
				.Payload = Payload,
				.Duplicate = Index != 0,
			});
		}
		for (auto &Item : Items) {
			if (!Schedule(*Network, std::move(Item))) {
				if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
					DisconnectInfo Information{DisconnectReason::ResourceExhaustion, "Simulated schedule exhausted"};
					CloseLink(*Network, Connection.LinkId, State->Id, Information);
					return TerminalOperation(TransportOperationStatus::ResourceExhausted, std::move(Information));
				}
				RecordUnreliableDrop(Connection);
				return Operation(TransportOperationStatus::Succeeded);
			}
		}
		if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
			Connection.ReliableQueueBytes += Payload.size();
			Connection.Statistics.QueuedReliableBytes = Connection.ReliableQueueBytes;
		} else {
			Connection.QueuedUnreliableMessages += Copies;
		}
		return Operation(TransportOperationStatus::Succeeded);
	}

	std::size_t SimulatedTransport::PollEvents(std::span<TransportEvent> Output) {
		const auto Count = std::min(Output.size(), State->Events.size());
		for (std::size_t Index = 0; Index < Count; ++Index) {
			Output[Index] = std::move(State->Events.front());
			State->Events.pop_front();
		}
		return Count;
	}

	std::optional<std::size_t> SimulatedTransport::GetAvailableDatagramBytes(ConnectionId Connection) const {
		auto Iterator = State->Connections.find(Connection);
		if (Iterator == State->Connections.end()) return std::nullopt;
		return std::min(Network->Configuration.MaximumUnreliableDatagramBytes,
			Iterator->second.Limits.MaximumUnreliableMessageBytes);
	}

	std::optional<NetworkStatistics> SimulatedTransport::GetStatistics(ConnectionId Connection) const {
		auto Iterator = State->Connections.find(Connection);
		if (Iterator == State->Connections.end()) return std::nullopt;
		return Iterator->second.Statistics;
	}

	TransportOperationResult SimulatedTransport::ScheduleDisconnect(
		ConnectionId Connection,
		std::chrono::microseconds Delay,
		DisconnectInfo Information
	) {
		if (!IsValidDuration(Delay) || !Information.IsValid())
			return Operation(TransportOperationStatus::MessageRejected);
		auto Iterator = State->Connections.find(Connection);
		if (Iterator == State->Connections.end()) return Operation(TransportOperationStatus::InvalidConnection);
		auto At = AddTime(Network->CurrentTime, Delay);
		if (!At || !CanSchedule(*Network, 1) || !Schedule(*Network, ScheduledItem{
			.Kind = ScheduledKind::Disconnect,
			.At = At.value_or(Network->CurrentTime),
			.LinkId = Iterator->second.LinkId,
			.SourceTransportId = State->Id,
			.SourceConnection = Connection,
			.Disconnect = Information,
		})) return Operation(TransportOperationStatus::ResourceExhausted);
		return Operation(TransportOperationStatus::Succeeded);
	}

	std::uint64_t SimulatedTransport::TransportId() const { return State->Id; }
}

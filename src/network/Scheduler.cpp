#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/Transport.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		constexpr std::size_t MinimumQueuedMessageBytes = 1;

		bool MessageCountFitsQueuedBytes(std::size_t Messages, std::size_t Bytes) {
			return Messages <= Bytes / MinimumQueuedMessageBytes;
		}
	}

	bool SchedulerSubmitResult::Accepted() const {
		return Status == SchedulerSubmitStatus::Accepted ||
			Status == SchedulerSubmitStatus::AcceptedWithSupersession;
	}

	bool SchedulerSubmitResult::IsValid() const {
		switch (Status) {
		case SchedulerSubmitStatus::Accepted:
		case SchedulerSubmitStatus::AcceptedWithSupersession:
		case SchedulerSubmitStatus::DroppedUnreliable:
		case SchedulerSubmitStatus::InvalidConnection:
		case SchedulerSubmitStatus::IntentRejected:
			return !TerminalDisconnect.has_value();
		case SchedulerSubmitStatus::ReliableBacklogExhausted:
			return TerminalDisconnect && TerminalDisconnect->IsValid() &&
				TerminalDisconnect->Reason == DisconnectReason::ResourceExhaustion;
		}
		return false;
	}

	bool SchedulerTickBudget::IsValidFor(const NetworkLimits &Limits) const {
		if (!Limits.IsValid()) return false;
		const auto LargestMessage = std::max(Limits.MaximumReliableMessageBytes,
			Limits.MaximumUnreliableMessageBytes);
		return MaximumBytes >= LargestMessage && MaximumBytes <= Limits.MaximumSendBytesPerTick &&
			MaximumMessages > 0 && MaximumMessages <= Limits.MaximumMessagesPerTick;
	}

	SchedulerTickBudget SchedulerTickBudget::FromNetworkLimits(const NetworkLimits &Limits) {
		if (!Limits.IsValid()) return {};
		return {Limits.MaximumSendBytesPerTick, Limits.MaximumMessagesPerTick};
	}

	bool SchedulerFlushResult::IsValidFor(const SchedulerTickBudget &Budget) const {
		if (MessagesSubmitted > Budget.MaximumMessages || BytesSubmitted > Budget.MaximumBytes) return false;
		switch (Status) {
		case SchedulerFlushStatus::Drained:
		case SchedulerFlushStatus::BudgetLimited:
		case SchedulerFlushStatus::TransportBackpressured:
			return !TerminalDisconnect.has_value();
		case SchedulerFlushStatus::InvalidConnection:
		case SchedulerFlushStatus::InvalidBudget:
			return MessagesSubmitted == 0 && BytesSubmitted == 0 && !TerminalDisconnect.has_value();
		case SchedulerFlushStatus::TerminalFailure:
			return TerminalDisconnect && TerminalDisconnect->IsValid();
		}
		return false;
	}

	bool SchedulerStatistics::IsValidFor(const NetworkLimits &Limits) const {
		if (!Limits.IsValid()) return false;
		if (QueuedReliableBytes > Limits.MaximumQueuedReliableBytes ||
			QueuedUnreliableBytes > Limits.MaximumSendBytesPerTick ||
			QueuedUnreliableMessages > Limits.MaximumMessagesPerTick ||
			!MessageCountFitsQueuedBytes(QueuedReliableMessages, QueuedReliableBytes) ||
			!MessageCountFitsQueuedBytes(QueuedUnreliableMessages, QueuedUnreliableBytes) ||
			QueuedReliableMessages > std::numeric_limits<std::size_t>::max() - QueuedUnreliableMessages ||
			QueuedMessages != QueuedReliableMessages + QueuedUnreliableMessages)
			return false;
		return IntentsAccepted >= MessagesSubmittedToTransport &&
			IntentsRejected >= ReliableBacklogExhaustions;
	}

	std::optional<std::uint8_t> SchedulerTrafficPrecedence(TrafficClass Traffic) {
		switch (Traffic) {
		case TrafficClass::Control:
			return 0;
		case TrafficClass::StructuralReplication:
			return 1;
		case TrafficClass::ReliableApplication:
			return 2;
		case TrafficClass::RealtimeState:
			return 3;
		case TrafficClass::EphemeralApplication:
			return 4;
		case TrafficClass::Background:
			return 5;
		}
		return std::nullopt;
	}

	namespace {
		struct SequencedQueueKey {
			std::uint8_t Domain = 0;
			std::uint64_t Channel = 0;
			std::uint64_t Publication = 0;
			auto operator<=>(const SequencedQueueKey &) const = default;
		};

		std::optional<std::pair<SequencedQueueKey, std::uint64_t>> GetSequencedQueueKey(const MessageOrder &Order) {
			if (const auto *Realtime = std::get_if<RealtimeStateOrder>(&Order))
				return std::pair(SequencedQueueKey{1, Realtime->Channel.Value(), 0}, Realtime->Sequence.Value());
			if (const auto *Remote = std::get_if<RemoteEventOrder>(&Order))
				return std::pair(
					SequencedQueueKey{2, Remote->Channel.Value(), Remote->Publication.Value()},
					Remote->Sequence.Value()
				);
			return std::nullopt;
		}
	}

	struct NetworkScheduler::Implementation {
		struct ConnectionQueue {
			NetworkLimits Limits;
			std::array<std::deque<NetworkMessageIntent>, 6> Queues;
			SchedulerStatistics Statistics;
			std::uint32_t ConsecutiveStructuralReplicationMessages = 0;
			bool Active = true;
		};

		explicit Implementation(IGameTransport &Transport, std::size_t MaximumTotalQueuedReliableBytes)
			: Transport(Transport), MaximumTotalQueuedReliableBytes(MaximumTotalQueuedReliableBytes) {}

		static void Queue(ConnectionQueue &Connection, NetworkMessageIntent Message) {
			const auto Precedence = SchedulerTrafficPrecedence(Message.Traffic());
			if (Message.Delivery() == DeliveryMode::ReliableOrdered) ++Connection.Statistics.QueuedReliableMessages;
			else ++Connection.Statistics.QueuedUnreliableMessages;
			Connection.Queues[*Precedence].push_back(std::move(Message));
			++Connection.Statistics.QueuedMessages;
		}

		void Clear(ConnectionQueue &Connection) {
			TotalQueuedReliableBytes -= Connection.Statistics.QueuedReliableBytes;
			for (auto &QueueValue : Connection.Queues) QueueValue.clear();
			Connection.Statistics.QueuedReliableBytes = 0;
			Connection.Statistics.QueuedReliableMessages = 0;
			Connection.Statistics.QueuedUnreliableBytes = 0;
			Connection.Statistics.QueuedUnreliableMessages = 0;
			Connection.Statistics.QueuedMessages = 0;
			Connection.ConsecutiveStructuralReplicationMessages = 0;
		}

		static SchedulerSubmitResult RejectUnreliable(ConnectionQueue &Connection) {
			++Connection.Statistics.IntentsRejected;
			++Connection.Statistics.UnreliableMessagesDroppedBeforeTransport;
			return {SchedulerSubmitStatus::DroppedUnreliable};
		}

		IGameTransport &Transport;
		std::size_t MaximumTotalQueuedReliableBytes = 0;
		std::size_t TotalQueuedReliableBytes = 0;
		std::map<ConnectionId, ConnectionQueue> Connections;
	};

	NetworkScheduler::NetworkScheduler(IGameTransport &Transport, std::size_t MaximumTotalQueuedReliableBytes)
		: State(std::make_unique<Implementation>(Transport, MaximumTotalQueuedReliableBytes)) {
		if (MaximumTotalQueuedReliableBytes == 0)
			throw std::invalid_argument("NetworkScheduler aggregate reliable queue limit must be positive");
	}
	NetworkScheduler::~NetworkScheduler() = default;

	bool NetworkScheduler::RegisterConnection(ConnectionId Connection, const NetworkLimits &Limits) {
		if (!Connection.IsValid() || !Limits.IsValid()) return false;
		for (const auto &[Existing, Queue] : State->Connections)
			if (Existing.Slot == Connection.Slot && Queue.Active) return false;
		return State->Connections.emplace(Connection, Implementation::ConnectionQueue{.Limits = Limits}).second;
	}

	SchedulerSubmitResult NetworkScheduler::Submit(NetworkMessageIntent Message) {
		auto Iterator = State->Connections.find(Message.Destination());
		if (Iterator == State->Connections.end() || !Iterator->second.Active)
			return {SchedulerSubmitStatus::InvalidConnection};
		auto &Connection = Iterator->second;
		if (!IsNetworkMessageIntentValid(Message, Connection.Limits)) {
			++Connection.Statistics.IntentsRejected;
			return {SchedulerSubmitStatus::IntentRejected};
		}
		const auto Bytes = Message.Payload().size();
		if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
			if (Bytes > Connection.Limits.MaximumQueuedReliableBytes - Connection.Statistics.QueuedReliableBytes ||
				Bytes > State->MaximumTotalQueuedReliableBytes - State->TotalQueuedReliableBytes) {
				++Connection.Statistics.IntentsRejected;
				++Connection.Statistics.ReliableBacklogExhaustions;
				State->Clear(Connection);
				Connection.Active = false;
				return {SchedulerSubmitStatus::ReliableBacklogExhausted,
					DisconnectInfo{DisconnectReason::ResourceExhaustion, "Reliable scheduler backlog exhausted"}};
			}
			Implementation::Queue(Connection, std::move(Message));
			Connection.Statistics.QueuedReliableBytes += Bytes;
			State->TotalQueuedReliableBytes += Bytes;
			++Connection.Statistics.IntentsAccepted;
			return {SchedulerSubmitStatus::Accepted};
		}
		if (Message.Delivery() == DeliveryMode::UnreliableSequenced) {
			auto Incoming = GetSequencedQueueKey(Message.Order());
			if (!Incoming) return Implementation::RejectUnreliable(Connection);
			for (auto &QueueValue : Connection.Queues) for (auto Existing = QueueValue.begin(); Existing != QueueValue.end(); ++Existing) {
				auto Key = GetSequencedQueueKey(Existing->Order());
				if (!Key || Key->first != Incoming->first) continue;
				if (Incoming->second <= Key->second) return Implementation::RejectUnreliable(Connection);
				const auto ExistingBytes = Existing->Payload().size();
				if (Bytes > Connection.Limits.MaximumSendBytesPerTick -
					(Connection.Statistics.QueuedUnreliableBytes - ExistingBytes)) return Implementation::RejectUnreliable(Connection);
				QueueValue.erase(Existing);
				Connection.Statistics.QueuedUnreliableBytes -= ExistingBytes;
				--Connection.Statistics.QueuedUnreliableMessages;
				--Connection.Statistics.QueuedMessages;
				Implementation::Queue(Connection, std::move(Message));
				Connection.Statistics.QueuedUnreliableBytes += Bytes;
				++Connection.Statistics.IntentsAccepted;
				++Connection.Statistics.SequencedStatesSuperseded;
				return {SchedulerSubmitStatus::AcceptedWithSupersession};
			}
		}
		if (Connection.Statistics.QueuedUnreliableMessages >= Connection.Limits.MaximumMessagesPerTick ||
			Bytes > Connection.Limits.MaximumSendBytesPerTick - Connection.Statistics.QueuedUnreliableBytes)
			return Implementation::RejectUnreliable(Connection);
		Implementation::Queue(Connection, std::move(Message));
		Connection.Statistics.QueuedUnreliableBytes += Bytes;
		++Connection.Statistics.IntentsAccepted;
		return {SchedulerSubmitStatus::Accepted};
	}

	SchedulerFlushResult NetworkScheduler::Flush(ConnectionId ConnectionIdValue, SchedulerTickBudget Budget) {
		auto Iterator = State->Connections.find(ConnectionIdValue);
		if (Iterator == State->Connections.end() || !Iterator->second.Active)
			return {.Status = SchedulerFlushStatus::InvalidConnection};
		auto &Connection = Iterator->second;
		if (!Budget.IsValidFor(Connection.Limits)) return {.Status = SchedulerFlushStatus::InvalidBudget};
		SchedulerFlushResult Result{.Status = SchedulerFlushStatus::Drained};
		while (Connection.Statistics.QueuedMessages != 0) {
			std::size_t QueueIndex = 0;
			if (!Connection.Queues[0].empty()) QueueIndex = 0;
			else if (!Connection.Queues[1].empty() && !Connection.Queues[2].empty())
				QueueIndex = Connection.ConsecutiveStructuralReplicationMessages >=
					MaximumConsecutiveStructuralReplicationMessages ? 2 : 1;
			else {
				while (QueueIndex < Connection.Queues.size() && Connection.Queues[QueueIndex].empty()) ++QueueIndex;
			}
			auto &QueueValue = Connection.Queues[QueueIndex];
			const auto &Message = QueueValue.front();
			const auto Bytes = Message.Payload().size();
			if (Result.MessagesSubmitted == Budget.MaximumMessages || Bytes > Budget.MaximumBytes - Result.BytesSubmitted) {
				Result.Status = SchedulerFlushStatus::BudgetLimited;
				++Connection.Statistics.BudgetLimitedFlushes;
				return Result;
			}
			auto Submission = State->Transport.Send(Message);
			if (Submission.Status == TransportOperationStatus::WouldBlock) {
				Result.Status = SchedulerFlushStatus::TransportBackpressured;
				++Connection.Statistics.TransportBackpressureEvents;
				return Result;
			}
			if (!Submission.Succeeded()) {
				Result.Status = SchedulerFlushStatus::TerminalFailure;
				Result.TerminalDisconnect = Submission.TerminalDisconnect.value_or(DisconnectInfo{
					DisconnectReason::TransportFailure, "Transport rejected scheduler submission"});
				State->Clear(Connection);
				Connection.Active = false;
				return Result;
			}
			if (Message.Delivery() == DeliveryMode::ReliableOrdered) {
				Connection.Statistics.QueuedReliableBytes -= Bytes;
				State->TotalQueuedReliableBytes -= Bytes;
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
			if (QueueIndex == 1) {
				if (Connection.ConsecutiveStructuralReplicationMessages != std::numeric_limits<std::uint32_t>::max())
					++Connection.ConsecutiveStructuralReplicationMessages;
			}
			else if (QueueIndex == 2)
				Connection.ConsecutiveStructuralReplicationMessages = 0;
		}
		return Result;
	}

	bool NetworkScheduler::CancelConnection(ConnectionId Connection) {
		auto Existing = State->Connections.find(Connection);
		if (Existing == State->Connections.end()) return false;
		State->Clear(Existing->second);
		State->Connections.erase(Existing);
		return true;
	}

	std::optional<SchedulerStatistics> NetworkScheduler::GetStatistics(ConnectionId Connection) const {
		auto Iterator = State->Connections.find(Connection);
		return Iterator == State->Connections.end() ? std::nullopt : std::optional(Iterator->second.Statistics);
	}
}

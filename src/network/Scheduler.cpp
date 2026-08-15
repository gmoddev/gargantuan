#include "gargantuan/network/Scheduler.hpp"

#include <algorithm>
#include <limits>

namespace gargantuan::network {
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
			QueuedReliableMessages > Limits.MaximumQueuedReliableBytes ||
			QueuedUnreliableMessages > Limits.MaximumMessagesPerTick ||
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
}

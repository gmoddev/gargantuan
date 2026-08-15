#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/network/Outcome.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gargantuan::network {
	enum class SchedulerSubmitStatus : std::uint8_t {
		Accepted,
		AcceptedWithSupersession,
		DroppedUnreliable,
		InvalidConnection,
		IntentRejected,
		ReliableBacklogExhausted,
	};

	struct SchedulerSubmitResult {
		SchedulerSubmitStatus Status = SchedulerSubmitStatus::IntentRejected;
		std::optional<DisconnectInfo> TerminalDisconnect;

		[[nodiscard]] bool Accepted() const;
		[[nodiscard]] bool IsTerminal() const { return TerminalDisconnect.has_value(); }
		[[nodiscard]] bool IsValid() const;
	};

	struct SchedulerTickBudget {
		std::size_t MaximumBytes = 0;
		std::uint32_t MaximumMessages = 0;

		[[nodiscard]] bool IsValidFor(const NetworkLimits &Limits) const;
		[[nodiscard]] static SchedulerTickBudget FromNetworkLimits(const NetworkLimits &Limits);
	};

	enum class SchedulerFlushStatus : std::uint8_t {
		Drained,
		BudgetLimited,
		TransportBackpressured,
		InvalidConnection,
		InvalidBudget,
		TerminalFailure,
	};

	struct SchedulerFlushResult {
		SchedulerFlushStatus Status = SchedulerFlushStatus::InvalidConnection;
		std::uint32_t MessagesSubmitted = 0;
		std::size_t BytesSubmitted = 0;
		std::optional<DisconnectInfo> TerminalDisconnect;

		[[nodiscard]] bool IsTerminal() const { return TerminalDisconnect.has_value(); }
		[[nodiscard]] bool IsValidFor(const SchedulerTickBudget &Budget) const;
	};

	struct SchedulerStatistics {
		std::uint64_t IntentsAccepted = 0;
		std::uint64_t IntentsRejected = 0;
		std::uint64_t ReliableBacklogExhaustions = 0;
		std::uint64_t UnreliableMessagesDroppedBeforeTransport = 0;
		std::uint64_t SequencedStatesSuperseded = 0;
		std::uint64_t MessagesSubmittedToTransport = 0;
		std::uint64_t BatchesProduced = 0;
		std::uint64_t BudgetLimitedFlushes = 0;
		std::uint64_t TransportBackpressureEvents = 0;
		std::size_t QueuedReliableBytes = 0;
		std::size_t QueuedReliableMessages = 0;
		std::size_t QueuedUnreliableBytes = 0;
		std::size_t QueuedUnreliableMessages = 0;
		std::size_t QueuedMessages = 0;

		[[nodiscard]] bool IsValidFor(const NetworkLimits &Limits) const;
	};

	[[nodiscard]] std::optional<std::uint8_t> SchedulerTrafficPrecedence(TrafficClass Traffic);

	class INetworkScheduler {
	  public:
		virtual ~INetworkScheduler() = default;

		virtual bool RegisterConnection(ConnectionId Connection, const NetworkLimits &Limits) = 0;
		virtual SchedulerSubmitResult Submit(NetworkMessageIntent Intent) = 0;
		virtual SchedulerFlushResult Flush(ConnectionId Connection, SchedulerTickBudget Budget) = 0;
		virtual bool CancelConnection(ConnectionId Connection) = 0;
		[[nodiscard]] virtual std::optional<SchedulerStatistics> GetStatistics(ConnectionId Connection) const = 0;
	};
}

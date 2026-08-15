#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gargantuan::network {
	struct NetworkStatistics {
		std::optional<std::uint64_t> BytesSent;
		std::optional<std::uint64_t> BytesReceived;
		std::optional<std::uint64_t> MessagesSent;
		std::optional<std::uint64_t> MessagesReceived;
		std::optional<std::uint64_t> DroppedUnreliableMessages;
		std::optional<std::size_t> QueuedReliableBytes;
		std::optional<std::chrono::microseconds> EstimatedRoundTripTime;
		std::optional<double> MessageLossRatio;

		[[nodiscard]] bool IsValid() const;
	};
}

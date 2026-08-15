#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gargantuan::network {
	inline constexpr std::size_t NativeMaximumNetworkMessageBytes = 8 * 1024 * 1024;
	inline constexpr std::size_t NativeMaximumUnreliableMessageBytes = 64 * 1024;
	inline constexpr std::size_t NativeMaximumQueuedReliableBytes = 64 * 1024 * 1024;
	inline constexpr std::uint32_t NativeMaximumInFlightRemoteRequests = 4096;
	inline constexpr std::size_t NativeMaximumNetworkBytesPerTick = 16 * 1024 * 1024;
	inline constexpr std::uint32_t NativeMaximumNetworkMessagesPerTick = 4096;

	struct NetworkLimits {
		std::size_t MaximumReliableMessageBytes = 0;
		std::size_t MaximumUnreliableMessageBytes = 0;
		std::size_t MaximumQueuedReliableBytes = 0;
		std::uint32_t MaximumInFlightRemoteRequests = 0;
		std::size_t MaximumDecodedMessageBytes = 0;
		std::size_t MaximumSendBytesPerTick = 0;
		std::size_t MaximumReceiveBytesPerTick = 0;
		std::uint32_t MaximumMessagesPerTick = 0;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] static NetworkLimits NativeCeilings();
	};

	[[nodiscard]] std::optional<NetworkLimits> NegotiateNetworkLimits(
		const NetworkLimits &First,
		const NetworkLimits &Second
	);
}

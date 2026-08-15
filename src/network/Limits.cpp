#include "gargantuan/network/Limits.hpp"

#include <algorithm>

namespace gargantuan::network {
	bool NetworkLimits::IsValid() const {
		return MaximumReliableMessageBytes > 0 &&
			MaximumReliableMessageBytes <= NativeMaximumNetworkMessageBytes &&
			MaximumUnreliableMessageBytes > 0 &&
			MaximumUnreliableMessageBytes <= NativeMaximumUnreliableMessageBytes &&
			MaximumQueuedReliableBytes >= MaximumReliableMessageBytes &&
			MaximumQueuedReliableBytes <= NativeMaximumQueuedReliableBytes &&
			MaximumInFlightRemoteRequests > 0 &&
			MaximumInFlightRemoteRequests <= NativeMaximumInFlightRemoteRequests &&
			MaximumDecodedMessageBytes >= MaximumReliableMessageBytes &&
			MaximumDecodedMessageBytes >= MaximumUnreliableMessageBytes &&
			MaximumDecodedMessageBytes <= NativeMaximumNetworkMessageBytes &&
			MaximumSendBytesPerTick >= MaximumReliableMessageBytes &&
			MaximumSendBytesPerTick >= MaximumUnreliableMessageBytes &&
			MaximumSendBytesPerTick <= NativeMaximumNetworkBytesPerTick &&
			MaximumReceiveBytesPerTick >= MaximumDecodedMessageBytes &&
			MaximumReceiveBytesPerTick <= NativeMaximumNetworkBytesPerTick &&
			MaximumMessagesPerTick > 0 && MaximumMessagesPerTick <= NativeMaximumNetworkMessagesPerTick;
	}

	NetworkLimits NetworkLimits::NativeCeilings() {
		return {
			.MaximumReliableMessageBytes = NativeMaximumNetworkMessageBytes,
			.MaximumUnreliableMessageBytes = NativeMaximumUnreliableMessageBytes,
			.MaximumQueuedReliableBytes = NativeMaximumQueuedReliableBytes,
			.MaximumInFlightRemoteRequests = NativeMaximumInFlightRemoteRequests,
			.MaximumDecodedMessageBytes = NativeMaximumNetworkMessageBytes,
			.MaximumSendBytesPerTick = NativeMaximumNetworkBytesPerTick,
			.MaximumReceiveBytesPerTick = NativeMaximumNetworkBytesPerTick,
			.MaximumMessagesPerTick = NativeMaximumNetworkMessagesPerTick,
		};
	}

	std::optional<NetworkLimits> NegotiateNetworkLimits(const NetworkLimits &First, const NetworkLimits &Second) {
		if (!First.IsValid() || !Second.IsValid()) return std::nullopt;
		NetworkLimits Result{
			.MaximumReliableMessageBytes = std::min(First.MaximumReliableMessageBytes, Second.MaximumReliableMessageBytes),
			.MaximumUnreliableMessageBytes = std::min(First.MaximumUnreliableMessageBytes, Second.MaximumUnreliableMessageBytes),
			.MaximumQueuedReliableBytes = std::min(First.MaximumQueuedReliableBytes, Second.MaximumQueuedReliableBytes),
			.MaximumInFlightRemoteRequests = std::min(First.MaximumInFlightRemoteRequests, Second.MaximumInFlightRemoteRequests),
			.MaximumDecodedMessageBytes = std::min(First.MaximumDecodedMessageBytes, Second.MaximumDecodedMessageBytes),
			.MaximumSendBytesPerTick = std::min(First.MaximumSendBytesPerTick, Second.MaximumSendBytesPerTick),
			.MaximumReceiveBytesPerTick = std::min(First.MaximumReceiveBytesPerTick, Second.MaximumReceiveBytesPerTick),
			.MaximumMessagesPerTick = std::min(First.MaximumMessagesPerTick, Second.MaximumMessagesPerTick),
		};
		return Result.IsValid() ? std::optional(Result) : std::nullopt;
	}
}

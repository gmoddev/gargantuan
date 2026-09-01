#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Delivery.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Sequence.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace gargantuan::network {
	struct RealtimeStateOrder {
		StateChannelId Channel;
		RealtimeStateSequence Sequence;
	};

	struct RemoteEventOrder {
		StateChannelId Channel;
		RemotePublicationId Publication;
		RemoteEventSequence Sequence;
	};

	struct ReliableReplicationOrder {
		ReliableReplicationSequence Sequence;
	};

	using MessageOrder = std::variant<
		std::monostate,
		RealtimeStateOrder,
		RemoteEventOrder,
		ReliableReplicationOrder
	>;

	[[nodiscard]] bool IsValidMessageOrder(const MessageOrder &Order);

	class NetworkMessageIntent {
	  public:
		[[nodiscard]] ConnectionId Destination() const { return DestinationConnection; }
		[[nodiscard]] DeliveryMode Delivery() const { return RequiredDelivery; }
		[[nodiscard]] TrafficClass Traffic() const { return IntendedTraffic; }
		[[nodiscard]] const MessageOrder &Order() const { return Ordering; }
		[[nodiscard]] const std::vector<std::byte> &Payload() const { return MessagePayload; }

	  private:
		friend std::optional<NetworkMessageIntent> MakeNetworkMessageIntent(
			ConnectionId,
			DeliveryMode,
			TrafficClass,
			MessageOrder,
			std::vector<std::byte>,
			const NetworkLimits &
		);

		ConnectionId DestinationConnection;
		DeliveryMode RequiredDelivery = DeliveryMode::ReliableOrdered;
		TrafficClass IntendedTraffic = TrafficClass::Control;
		MessageOrder Ordering;
		std::vector<std::byte> MessagePayload;
	};

	[[nodiscard]] std::optional<NetworkMessageIntent> MakeNetworkMessageIntent(
		ConnectionId Destination,
		DeliveryMode Delivery,
		TrafficClass Traffic,
		MessageOrder Order,
		std::vector<std::byte> Payload,
		const NetworkLimits &Limits
	);
	[[nodiscard]] bool IsNetworkMessageIntentValid(
		const NetworkMessageIntent &Intent,
		const NetworkLimits &Limits
	);
}

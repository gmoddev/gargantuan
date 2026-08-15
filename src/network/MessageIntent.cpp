#include "gargantuan/network/MessageIntent.hpp"

#include <type_traits>

namespace gargantuan::network {
	bool IsValidMessageOrder(const MessageOrder &Order) {
		return std::visit([](const auto &Value) {
			using Type = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<Type, std::monostate>) return true;
			else if constexpr (std::is_same_v<Type, RealtimeStateOrder> || std::is_same_v<Type, RemoteEventOrder>)
				return Value.Channel.IsValid() && Value.Sequence.IsValid();
			else return Value.Sequence.IsValid();
		}, Order);
	}

	std::optional<NetworkMessageIntent> MakeNetworkMessageIntent(
		ConnectionId Destination,
		DeliveryMode Delivery,
		TrafficClass Traffic,
		MessageOrder Order,
		std::vector<std::byte> Payload,
		const NetworkLimits &Limits
	) {
		if (!Destination.IsValid() || !Limits.IsValid() || !IsValidDeliveryMode(Delivery) ||
			!IsValidTrafficClass(Traffic) || Payload.empty() || !IsValidMessageOrder(Order)) return std::nullopt;
		const bool HasOrder = !std::holds_alternative<std::monostate>(Order);
		if (Delivery == DeliveryMode::UnreliableSequenced) {
			if (!HasOrder || std::holds_alternative<ReliableReplicationOrder>(Order)) return std::nullopt;
		} else if (Delivery == DeliveryMode::UnreliableUnordered && HasOrder) {
			return std::nullopt;
		}
		const auto MessageLimit = Delivery == DeliveryMode::ReliableOrdered
			? Limits.MaximumReliableMessageBytes : Limits.MaximumUnreliableMessageBytes;
		if (Payload.size() > MessageLimit || Payload.size() > Limits.MaximumDecodedMessageBytes ||
			Payload.size() > Limits.MaximumSendBytesPerTick) return std::nullopt;

		NetworkMessageIntent Result;
		Result.DestinationConnection = Destination;
		Result.RequiredDelivery = Delivery;
		Result.IntendedTraffic = Traffic;
		Result.Ordering = std::move(Order);
		Result.MessagePayload = std::move(Payload);
		return Result;
	}
}

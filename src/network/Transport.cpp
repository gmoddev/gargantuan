#include "gargantuan/network/Transport.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <type_traits>

namespace gargantuan::network {
	bool TransportEndpoint::IsValid() const {
		try {
			ValidateProtocolString(Host, MaximumTransportEndpointBytes, "Transport endpoint");
			return !Host.empty() && Port != 0;
		} catch (...) {
			return false;
		}
	}

	bool TransportStartConfiguration::IsValid() const {
		const bool ValidRole = Role == TransportRole::Client || Role == TransportRole::Server;
		return ValidRole && Endpoint.IsValid() && AdvertisedLimits.IsValid() &&
			OpaqueHandshakeMaterial.size() <= MaximumOpaqueHandshakeMaterialBytes;
	}

	bool IsValidTransportEvent(const TransportEvent &Event, const NetworkLimits &Limits) {
		if (!Limits.IsValid()) return false;
		return std::visit([&](const auto &Value) {
			using Type = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<Type, ConnectionStateEvent>) {
				return Value.Connection.IsValid() && IsLegalConnectionTransition(Value.Previous, Value.Current);
			} else if constexpr (std::is_same_v<Type, ReceivedMessageEvent>) {
				if (!Value.Connection.IsValid() || !IsValidDeliveryMode(Value.Delivery) ||
					!IsValidTrafficClass(Value.Traffic) || Value.Payload.empty() || !IsValidMessageOrder(Value.Order) ||
					Value.Payload.size() > Limits.MaximumDecodedMessageBytes ||
					Value.Payload.size() > Limits.MaximumReceiveBytesPerTick) return false;
				const auto MessageLimit = Value.Delivery == DeliveryMode::ReliableOrdered
					? Limits.MaximumReliableMessageBytes : Limits.MaximumUnreliableMessageBytes;
				if (Value.Payload.size() > MessageLimit) return false;
				const bool HasOrder = !std::holds_alternative<std::monostate>(Value.Order);
				if (Value.Delivery == DeliveryMode::UnreliableSequenced)
					return HasOrder && !std::holds_alternative<ReliableReplicationOrder>(Value.Order);
				return Value.Delivery != DeliveryMode::UnreliableUnordered || !HasOrder;
			} else if constexpr (std::is_same_v<Type, DisconnectedEvent>) {
				return Value.Connection.IsValid() && Value.Information.IsValid();
			} else {
				return Value.Information.IsValid() &&
					Value.Information.Reason == DisconnectReason::TransportFailure;
			}
		}, Event);
	}
}

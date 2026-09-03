#include "gargantuan/network/Transport.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <array>
#include <cctype>
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

	bool IsLoopbackTransportEndpoint(const TransportEndpoint &Endpoint) {
		if (!Endpoint.IsValid()) return false;
		if (Endpoint.Host == "::1") return true;
		std::array<std::uint32_t, 4> Octets{};
		std::size_t Octet = 0;
		std::size_t Digits = 0;
		for (const auto Character : Endpoint.Host) {
			if (Character == '.') {
				if (Digits == 0 || Octet >= 3) return false;
				++Octet;
				Digits = 0;
				continue;
			}
			if (!std::isdigit(static_cast<unsigned char>(Character)) || Digits >= 3) return false;
			Octets[Octet] = Octets[Octet] * 10 + static_cast<std::uint32_t>(Character - '0');
			if (Octets[Octet] > 255) return false;
			++Digits;
		}
		return Octet == 3 && Digits != 0 && Octets[0] == 127;
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

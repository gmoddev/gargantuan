#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Delivery.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/network/Outcome.hpp"
#include "gargantuan/network/Statistics.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace gargantuan::network {
	inline constexpr std::size_t MaximumTransportEndpointBytes = 1024;
	inline constexpr std::size_t MaximumOpaqueHandshakeMaterialBytes = 64 * 1024;

	enum class TransportRole : std::uint8_t { Client, Server };

	struct TransportEndpoint {
		std::string Host;
		std::uint16_t Port = 0;
		[[nodiscard]] bool IsValid() const;
	};

	struct TransportStartConfiguration {
		TransportRole Role = TransportRole::Client;
		TransportEndpoint Endpoint;
		NetworkLimits AdvertisedLimits;
		std::vector<std::byte> OpaqueHandshakeMaterial;

		[[nodiscard]] bool IsValid() const;
	};

	struct ConnectionStateEvent {
		ConnectionId Connection;
		ConnectionState Previous = ConnectionState::Connecting;
		ConnectionState Current = ConnectionState::Connecting;
	};

	struct ReceivedMessageEvent {
		ConnectionId Connection;
		DeliveryMode Delivery = DeliveryMode::ReliableOrdered;
		TrafficClass Traffic = TrafficClass::Control;
		MessageOrder Order;
		std::vector<std::byte> Payload;
	};

	struct DisconnectedEvent {
		ConnectionId Connection;
		DisconnectInfo Information;
	};

	struct TransportFailureEvent { DisconnectInfo Information; };

	using TransportEvent = std::variant<
		ConnectionStateEvent,
		ReceivedMessageEvent,
		DisconnectedEvent,
		TransportFailureEvent
	>;

	[[nodiscard]] bool IsValidTransportEvent(const TransportEvent &Event, const NetworkLimits &Limits);

	class IGameTransport {
	  public:
		virtual ~IGameTransport() = default;
		virtual TransportOperationResult Start(const TransportStartConfiguration &Configuration) = 0;
		virtual TransportOperationResult Stop(DisconnectInfo Information) = 0;
		virtual TransportOperationResult Send(const NetworkMessageIntent &Message) = 0;
		virtual std::size_t PollEvents(std::span<TransportEvent> Output) = 0;
		[[nodiscard]] virtual std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Connection) const = 0;
		[[nodiscard]] virtual std::optional<NetworkStatistics> GetStatistics(ConnectionId Connection) const = 0;
	};
}

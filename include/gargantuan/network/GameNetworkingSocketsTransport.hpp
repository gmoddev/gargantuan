#pragma once

#include "gargantuan/network/Transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gargantuan::network {
	inline constexpr std::uint32_t NativeMaximumGnsConnections = 4096;
	inline constexpr std::uint32_t NativeMaximumGnsPendingEvents = 1'048'576;
	inline constexpr std::size_t NativeMaximumGnsPendingReceiveBytes = 64 * 1024 * 1024;

	struct GameNetworkingSocketsTransportConfiguration {
		std::uint32_t MaximumConnections = 256;
		std::uint32_t MaximumPendingEvents = 16'384;
		std::size_t MaximumPendingReceiveBytes = 16 * 1024 * 1024;

		[[nodiscard]] bool IsValid() const;
	};

	class GameNetworkingSocketsTransport final : public IGameTransport {
	  public:
		explicit GameNetworkingSocketsTransport(
			GameNetworkingSocketsTransportConfiguration Configuration = {}
		);
		~GameNetworkingSocketsTransport() override;

		GameNetworkingSocketsTransport(const GameNetworkingSocketsTransport &) = delete;
		GameNetworkingSocketsTransport &operator=(const GameNetworkingSocketsTransport &) = delete;
		GameNetworkingSocketsTransport(GameNetworkingSocketsTransport &&) = delete;
		GameNetworkingSocketsTransport &operator=(GameNetworkingSocketsTransport &&) = delete;

		TransportOperationResult Start(const TransportStartConfiguration &Configuration) override;
		TransportOperationResult Stop(DisconnectInfo Information) override;
		TransportOperationResult Disconnect(ConnectionId Connection, DisconnectInfo Information) override;
		TransportOperationResult Send(const NetworkMessageIntent &Message) override;
		std::size_t PollEvents(std::span<TransportEvent> Output) override;
		[[nodiscard]] std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Connection) const override;
		[[nodiscard]] std::optional<NetworkStatistics> GetStatistics(ConnectionId Connection) const override;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}

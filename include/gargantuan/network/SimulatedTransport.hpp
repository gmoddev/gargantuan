#pragma once

#include "gargantuan/network/Transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace gargantuan::network {
	inline constexpr auto MaximumSimulatedDuration = std::chrono::hours(24 * 365);
	inline constexpr std::uint64_t MaximumSimulatedBandwidthBytesPerSecond = 1024ull * 1024 * 1024;
	inline constexpr std::uint32_t MaximumSimulatedTransports = 1024;
	inline constexpr std::uint32_t MaximumSimulatedConnections = 16 * 1024;
	inline constexpr std::uint32_t MaximumSimulatedScheduledEvents = 1024 * 1024;
	inline constexpr std::uint32_t MaximumSimulatedPendingEvents = 1024 * 1024;

	struct SimulatedTransportConfiguration {
		std::uint64_t Seed = 1;
		std::chrono::microseconds BaseLatency{0};
		std::chrono::microseconds MaximumJitter{0};
		std::chrono::microseconds MaximumReorderDelay{0};
		double UnreliableLossProbability = 0.0;
		double UnreliableDuplicationProbability = 0.0;
		double UnreliableReorderProbability = 0.0;
		std::uint64_t BandwidthBytesPerSecond = 1024 * 1024;
		std::size_t MaximumReliableQueueBytes = NativeMaximumQueuedReliableBytes;
		std::uint32_t MaximumQueuedUnreliableMessages = 1024;
		std::size_t MaximumUnreliableDatagramBytes = 1200;
		std::uint32_t MaximumTransports = 64;
		std::uint32_t MaximumConnections = 1024;
		std::uint32_t MaximumScheduledEvents = 16 * 1024;
		std::uint32_t MaximumPendingEventsPerTransport = 4096;
		std::optional<std::chrono::microseconds> ConnectionLifetime;

		[[nodiscard]] bool IsValid() const;
	};

	struct SimulatedNetworkState;
	struct SimulatedTransportState;
	class SimulatedTransport;

	class SimulatedNetwork final {
	  public:
		[[nodiscard]] static std::shared_ptr<SimulatedNetwork> Create(SimulatedTransportConfiguration Configuration);
		[[nodiscard]] std::shared_ptr<SimulatedTransport> CreateTransport();
		[[nodiscard]] bool Advance(std::chrono::microseconds Delta);
		std::size_t Pump();
		[[nodiscard]] std::chrono::microseconds Now() const;
		[[nodiscard]] const SimulatedTransportConfiguration &Configuration() const;

	  private:
		explicit SimulatedNetwork(std::shared_ptr<SimulatedNetworkState> State);
		std::shared_ptr<SimulatedNetworkState> State;
	};

	class SimulatedTransport final : public IGameTransport {
	  public:
		~SimulatedTransport() override;

		TransportOperationResult Start(const TransportStartConfiguration &Configuration) override;
		TransportOperationResult Stop(DisconnectInfo Information) override;
		TransportOperationResult Disconnect(ConnectionId Connection, DisconnectInfo Information) override;
		TransportOperationResult Send(const NetworkMessageIntent &Message) override;
		std::size_t PollEvents(std::span<TransportEvent> Output) override;
		[[nodiscard]] std::optional<std::size_t> GetAvailableDatagramBytes(ConnectionId Connection) const override;
		[[nodiscard]] std::optional<NetworkStatistics> GetStatistics(ConnectionId Connection) const override;

		[[nodiscard]] TransportOperationResult ScheduleDisconnect(
			ConnectionId Connection,
			std::chrono::microseconds Delay,
			DisconnectInfo Information
		);
		[[nodiscard]] std::uint64_t TransportId() const;

	  private:
		friend class SimulatedNetwork;
		SimulatedTransport(std::shared_ptr<SimulatedNetworkState> Network, std::shared_ptr<SimulatedTransportState> State);

		std::shared_ptr<SimulatedNetworkState> Network;
		std::shared_ptr<SimulatedTransportState> State;
	};
}

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace gargantuan::network {
	struct ConnectionId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] constexpr bool IsValid() const { return Slot != 0 && Generation != 0; }
		auto operator<=>(const ConnectionId &) const = default;
	};

	enum class ConnectionState : std::uint8_t {
		Connecting,
		Authenticating,
		Connected,
		Closing,
		Closed,
	};

	[[nodiscard]] bool IsLegalConnectionTransition(ConnectionState From, ConnectionState To);
	[[nodiscard]] bool IsTerminalConnectionState(ConnectionState State);
}

template <> struct std::hash<gargantuan::network::ConnectionId> {
	std::size_t operator()(const gargantuan::network::ConnectionId &Id) const noexcept {
		const auto Combined = (static_cast<std::uint64_t>(Id.Generation) << 32) | Id.Slot;
		return std::hash<std::uint64_t>{}(Combined);
	}
};

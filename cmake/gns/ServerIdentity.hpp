#pragma once

#include <array>
#include <cstdint>

class ISteamNetworkingSockets;

namespace SteamNetworkingSocketsLib {
	using GargantuanServerPublicKey = std::array<std::uint8_t, 32>;
	bool GargantuanEnableServerIdentity(ISteamNetworkingSockets *Interface,
		GargantuanServerPublicKey &PublicKey);
	bool GargantuanReadAuthenticatedServerKey(ISteamNetworkingSockets *Interface,
		std::uint32_t Connection, GargantuanServerPublicKey &PublicKey);
}

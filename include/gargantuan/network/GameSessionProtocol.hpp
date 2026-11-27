#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gargantuan::network {
	// Version 2 requires GRMT v3. Versions 3 and 4 separately identify the
	// managed package and loopback terrain handshake formats.
	inline constexpr std::uint16_t GameSessionProtocolVersion = 2;
	inline constexpr std::uint16_t GameSessionDistributionProtocolVersion = 3;
	inline constexpr std::uint16_t GameSessionTerrainProtocolVersion = 4;
	inline constexpr std::uint16_t GameSessionSpatialProtocolVersion = 5;
	inline constexpr std::size_t MaximumGameSessionFrameBytes = 256;

	enum class GameSessionMessageKind : std::uint8_t {
		ClientHello = 1,
		ServerAccepted = 2,
		ClientReady = 3,
	};

	enum class SessionIdentityKind : std::uint8_t {
		DevelopmentLocal = 1,
	};

	struct GameSessionClientHello {
		std::uint64_t Nonce = 0;
		NetworkLimits AdvertisedLimits;
		AssetContentId TerrainBinding;
		std::string RuntimeDistributionIdentity;
		bool SpatialContent{};
	};

	struct GameSessionServerAccepted {
		std::uint64_t Nonce = 0;
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
		std::uint32_t PlayerId = 0;
		SessionIdentityKind Identity = SessionIdentityKind::DevelopmentLocal;
		NetworkLimits NegotiatedLimits;
		AssetContentId TerrainBinding;
		std::string RuntimeDistributionIdentity;
		bool SpatialContent{};
		ObjectId AuthorizedSpatialRegion;
	};

	struct GameSessionClientReady {
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
		AssetContentId TerrainBinding;
		bool SpatialContent{};
	};

	using GameSessionMessage = std::variant<GameSessionClientHello, GameSessionServerAccepted, GameSessionClientReady>;

	[[nodiscard]] bool IsGameSessionFrame(std::span<const std::byte> Bytes);
	[[nodiscard]] bool IsGameSessionDistributionIdentity(std::string_view Value);
	[[nodiscard]] SerializationResult<std::vector<std::byte>>
	EncodeGameSessionMessage(const GameSessionMessage &Message);
	[[nodiscard]] SerializationResult<GameSessionMessage> DecodeGameSessionMessage(std::span<const std::byte> Bytes);
}

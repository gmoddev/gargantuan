#pragma once

#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace gargantuan::network {
	inline constexpr std::uint16_t GameSessionProtocolVersion = 1;
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
	};

	struct GameSessionServerAccepted {
		std::uint64_t Nonce = 0;
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
		std::uint32_t PlayerId = 0;
		SessionIdentityKind Identity = SessionIdentityKind::DevelopmentLocal;
		NetworkLimits NegotiatedLimits;
	};

	struct GameSessionClientReady {
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
	};

	using GameSessionMessage = std::variant<GameSessionClientHello, GameSessionServerAccepted, GameSessionClientReady>;

	[[nodiscard]] bool IsGameSessionFrame(std::span<const std::byte> Bytes);
	[[nodiscard]] SerializationResult<std::vector<std::byte>>
	EncodeGameSessionMessage(const GameSessionMessage &Message);
	[[nodiscard]] SerializationResult<GameSessionMessage> DecodeGameSessionMessage(std::span<const std::byte> Bytes);
}

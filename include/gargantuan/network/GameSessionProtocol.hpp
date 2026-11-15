#pragma once

#include "gargantuan/network/GameAdmission.hpp"
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
	inline constexpr std::uint16_t AuthenticatedGameSessionProtocolVersion = 2;
	inline constexpr std::size_t MaximumGameSessionFrameBytes = 256;

	enum class GameSessionMessageKind : std::uint8_t {
		ClientHello = 1,
		ServerAccepted = 2,
		ClientReady = 3,
	};

	enum class SessionIdentityKind : std::uint8_t {
		DevelopmentLocal = 1,
		Authenticated = 2,
	};

	struct GameSessionClientHello {
		std::uint64_t Nonce = 0;
		NetworkLimits AdvertisedLimits;
	};

	struct GameSessionAuthenticatedClientHello {
		std::uint64_t Nonce = 0;
		NetworkLimits AdvertisedLimits;
		std::array<std::byte, GameAdmissionProofBytes> Proof{};
	};

	struct GameSessionServerAccepted {
		std::uint64_t Nonce = 0;
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
		std::uint32_t PlayerId = 0;
		SessionIdentityKind Identity = SessionIdentityKind::DevelopmentLocal;
		NetworkLimits NegotiatedLimits;
		std::uint16_t Version = GameSessionProtocolVersion;
	};

	struct GameSessionClientReady {
		std::uint64_t SessionEpoch = 0;
		ReplicationEpoch Replication;
		ObjectId Player;
		std::uint16_t Version = GameSessionProtocolVersion;
	};

	using GameSessionMessage = std::variant<GameSessionClientHello, GameSessionAuthenticatedClientHello,
		GameSessionServerAccepted, GameSessionClientReady>;

	[[nodiscard]] bool IsGameSessionFrame(std::span<const std::byte> Bytes);
	[[nodiscard]] SerializationResult<std::vector<std::byte>>
	EncodeGameSessionMessage(const GameSessionMessage &Message);
	[[nodiscard]] SerializationResult<GameSessionMessage> DecodeGameSessionMessage(std::span<const std::byte> Bytes);
}

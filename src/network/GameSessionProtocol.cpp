#include "gargantuan/network/GameSessionProtocol.hpp"

#include "gargantuan/network/BinaryCodec.hpp"

#include <limits>
#include <string>
#include <type_traits>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t GameSessionMagic = 0x53455347; // "GSES" in little endian.

		bool WriteLimits(GameBinaryWriter &Output, const NetworkLimits &Limits) {
			if (!Limits.IsValid() || Limits.MaximumReliableMessageBytes > std::numeric_limits<std::uint32_t>::max() ||
				Limits.MaximumUnreliableMessageBytes > std::numeric_limits<std::uint32_t>::max() ||
				Limits.MaximumQueuedReliableBytes > std::numeric_limits<std::uint32_t>::max() ||
				Limits.MaximumDecodedMessageBytes > std::numeric_limits<std::uint32_t>::max() ||
				Limits.MaximumSendBytesPerTick > std::numeric_limits<std::uint32_t>::max() ||
				Limits.MaximumReceiveBytesPerTick > std::numeric_limits<std::uint32_t>::max())
				return false;
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumReliableMessageBytes));
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumUnreliableMessageBytes));
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumQueuedReliableBytes));
			Output.Integer(Limits.MaximumInFlightRemoteRequests);
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumDecodedMessageBytes));
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumSendBytesPerTick));
			Output.Integer(static_cast<std::uint32_t>(Limits.MaximumReceiveBytesPerTick));
			Output.Integer(Limits.MaximumMessagesPerTick);
			return true;
		}

		bool ReadLimits(GameBinaryReader &Input, NetworkLimits &Limits) {
			std::uint32_t Reliable;
			std::uint32_t Unreliable;
			std::uint32_t Queued;
			std::uint32_t InFlight;
			std::uint32_t Decoded;
			std::uint32_t Send;
			std::uint32_t Receive;
			std::uint32_t Messages;
			if (!Input.Integer(Reliable) || !Input.Integer(Unreliable) || !Input.Integer(Queued) ||
				!Input.Integer(InFlight) || !Input.Integer(Decoded) || !Input.Integer(Send) ||
				!Input.Integer(Receive) || !Input.Integer(Messages))
				return false;
			Limits = NetworkLimits{Reliable, Unreliable, Queued, InFlight, Decoded, Send, Receive, Messages};
			return Limits.IsValid() || Input.Fail("Game session network limits are invalid");
		}

		SerializationErrorCode ErrorCode(const GameBinaryReader &Input) {
			return Input.Error.find("truncated") != std::string::npos ? SerializationErrorCode::TruncatedInput
																	  : SerializationErrorCode::InvalidValue;
		}
	}

	bool IsGameSessionFrame(std::span<const std::byte> Bytes) {
		if (Bytes.size() < sizeof(GameSessionMagic)) return false;
		GameBinaryReader Input(Bytes, "Game session frame");
		std::uint32_t Magic;
		return Input.Integer(Magic) && Magic == GameSessionMagic;
	}

	SerializationResult<std::vector<std::byte>> EncodeGameSessionMessage(const GameSessionMessage &Message) {
		GameBinaryWriter Output(MaximumGameSessionFrameBytes);
		Output.Integer(GameSessionMagic);
		Output.Integer(GameSessionProtocolVersion);
		const auto Kind = std::visit(
			[](const auto &Value) {
				using ValueType = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<ValueType, GameSessionClientHello>)
					return GameSessionMessageKind::ClientHello;
				if constexpr (std::is_same_v<ValueType, GameSessionServerAccepted>)
					return GameSessionMessageKind::ServerAccepted;
				return GameSessionMessageKind::ClientReady;
			},
			Message
		);
		Output.Integer(static_cast<std::uint8_t>(Kind));
		Output.Integer<std::uint8_t>(0);

		bool Valid = std::visit(
			[&](const auto &Value) {
				using ValueType = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<ValueType, GameSessionClientHello>) {
					Output.Integer(Value.Nonce);
					return Value.Nonce != 0 && WriteLimits(Output, Value.AdvertisedLimits);
				} else if constexpr (std::is_same_v<ValueType, GameSessionServerAccepted>) {
					Output.Integer(Value.Nonce);
					Output.Integer(Value.SessionEpoch);
					Output.Integer(Value.Replication.Value());
					WriteBinaryObjectId(Output, Value.Player);
					Output.Integer(Value.PlayerId);
					Output.Integer(static_cast<std::uint8_t>(Value.Identity));
					Output.Integer<std::uint8_t>(0);
					Output.Integer<std::uint16_t>(0);
					return Value.Nonce != 0 && Value.SessionEpoch != 0 && Value.Replication.IsValid() &&
						   Value.Player.IsValid() && Value.PlayerId != 0 &&
						   Value.Identity == SessionIdentityKind::DevelopmentLocal &&
						   WriteLimits(Output, Value.NegotiatedLimits);
				} else {
					Output.Integer(Value.SessionEpoch);
					Output.Integer(Value.Replication.Value());
					WriteBinaryObjectId(Output, Value.Player);
					return Value.SessionEpoch != 0 && Value.Replication.IsValid() && Value.Player.IsValid();
				}
			},
			Message
		);
		if (!Valid || !Output.Succeeded())
			return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session message is invalid");
		return std::move(Output.Bytes);
	}

	SerializationResult<GameSessionMessage> DecodeGameSessionMessage(std::span<const std::byte> Bytes) {
		if (Bytes.size() > MaximumGameSessionFrameBytes)
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Game session frame exceeds its bound");
		GameBinaryReader Input(Bytes, "Game session frame");
		std::uint32_t Magic;
		std::uint16_t Version;
		std::uint8_t KindValue;
		std::uint8_t Reserved;
		if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(KindValue) || !Input.Integer(Reserved))
			return SerializationFailure(ErrorCode(Input), Input.Error);
		if (Magic != GameSessionMagic)
			return SerializationFailure(SerializationErrorCode::InvalidSyntax, "Game session magic is invalid");
		if (Version != GameSessionProtocolVersion)
			return SerializationFailure(
				SerializationErrorCode::UnsupportedVersion, "Game session version is unsupported"
			);
		if (Reserved != 0)
			return SerializationFailure(
				SerializationErrorCode::InvalidValue, "Game session reserved header byte is non-zero"
			);

		GameSessionMessage Result;
		if (KindValue == static_cast<std::uint8_t>(GameSessionMessageKind::ClientHello)) {
			GameSessionClientHello Value;
			if (!Input.Integer(Value.Nonce) || !ReadLimits(Input, Value.AdvertisedLimits) || Value.Nonce == 0)
				return SerializationFailure(
					ErrorCode(Input), Input.Error.empty() ? "Game session client hello is invalid" : Input.Error
				);
			Result = Value;
		} else if (KindValue == static_cast<std::uint8_t>(GameSessionMessageKind::ServerAccepted)) {
			GameSessionServerAccepted Value;
			std::uint64_t Replication;
			std::uint8_t Identity;
			std::uint8_t Reserved8;
			std::uint16_t Reserved16;
			if (!Input.Integer(Value.Nonce) || !Input.Integer(Value.SessionEpoch) || !Input.Integer(Replication) ||
				!ReadBinaryObjectId(Input, Value.Player) || !Input.Integer(Value.PlayerId) ||
				!Input.Integer(Identity) || !Input.Integer(Reserved8) || !Input.Integer(Reserved16) ||
				!ReadLimits(Input, Value.NegotiatedLimits))
				return SerializationFailure(ErrorCode(Input), Input.Error);
			Value.Replication = ReplicationEpoch(Replication);
			Value.Identity = static_cast<SessionIdentityKind>(Identity);
			if (Value.Nonce == 0 || Value.SessionEpoch == 0 || !Value.Replication.IsValid() ||
				!Value.Player.IsValid() || Value.PlayerId == 0 ||
				Value.Identity != SessionIdentityKind::DevelopmentLocal || Reserved8 != 0 || Reserved16 != 0)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session acceptance is invalid");
			Result = Value;
		} else if (KindValue == static_cast<std::uint8_t>(GameSessionMessageKind::ClientReady)) {
			GameSessionClientReady Value;
			std::uint64_t Replication;
			if (!Input.Integer(Value.SessionEpoch) || !Input.Integer(Replication) ||
				!ReadBinaryObjectId(Input, Value.Player))
				return SerializationFailure(ErrorCode(Input), Input.Error);
			Value.Replication = ReplicationEpoch(Replication);
			if (Value.SessionEpoch == 0 || !Value.Replication.IsValid() || !Value.Player.IsValid())
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session readiness is invalid");
			Result = Value;
		} else {
			return SerializationFailure(SerializationErrorCode::InvalidType, "Game session message kind is unknown");
		}
		if (!Input.Complete())
			return SerializationFailure(
				SerializationErrorCode::InvalidValue, "Game session frame contains trailing bytes"
			);
		return Result;
	}
}

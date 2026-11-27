#include "gargantuan/network/GameSessionProtocol.hpp"

#include "gargantuan/network/BinaryCodec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t GameSessionMagic = 0x53455347; // "GSES" in little endian.

		bool ReadTerrainBinding(GameBinaryReader &Input, AssetContentId &Binding) {
			std::span<const std::byte> Bytes;
			if (!Input.Take(Binding.Bytes.size(), Bytes)) return false;
			std::memcpy(Binding.Bytes.data(), Bytes.data(), Bytes.size());
			return true;
		}

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

	bool IsGameSessionDistributionIdentity(std::string_view Value) {
		return Value.size() == 64 && std::ranges::all_of(Value, [](char C) {
			return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
		});
	}

	SerializationResult<std::vector<std::byte>> EncodeGameSessionMessage(const GameSessionMessage &Message) {
		GameBinaryWriter Output(MaximumGameSessionFrameBytes);
		Output.Integer(GameSessionMagic);
		const auto RuntimeDistribution = std::visit([](const auto &Value) -> std::string_view {
			using Type = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<Type, GameSessionClientReady>) return {};
			else return Value.RuntimeDistributionIdentity;
		}, Message);
		if (!RuntimeDistribution.empty() && !IsGameSessionDistributionIdentity(RuntimeDistribution))
			return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session distribution identity is invalid");
		const bool Terrain = std::visit([](const auto &Value) { return Value.TerrainBinding.IsValid(); }, Message);
		const bool Spatial = std::visit([](const auto &Value) { return Value.SpatialContent; }, Message);
		if (Terrain && !Spatial && !RuntimeDistribution.empty())
			return SerializationFailure(SerializationErrorCode::InvalidValue, "Terrain cannot use a managed game session");
		Output.Integer(Spatial && !RuntimeDistribution.empty() ? GameSessionManagedSpatialProtocolVersion :
			Spatial ? GameSessionSpatialProtocolVersion : Terrain ? GameSessionTerrainProtocolVersion : RuntimeDistribution.empty()
			? GameSessionProtocolVersion : GameSessionDistributionProtocolVersion);
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
					const auto ValidLimits = WriteLimits(Output, Value.AdvertisedLimits);
					if (Terrain || Spatial) Output.Append(std::as_bytes(std::span(Value.TerrainBinding.Bytes)));
					return Value.Nonce != 0 && ValidLimits;
				} else if constexpr (std::is_same_v<ValueType, GameSessionServerAccepted>) {
					Output.Integer(Value.Nonce);
					Output.Integer(Value.SessionEpoch);
					Output.Integer(Value.Replication.Value());
					WriteBinaryObjectId(Output, Value.Player);
					Output.Integer(Value.PlayerId);
					Output.Integer(static_cast<std::uint8_t>(Value.Identity));
					Output.Integer<std::uint8_t>(0);
					Output.Integer<std::uint16_t>(0);
					const auto ValidLimits = WriteLimits(Output, Value.NegotiatedLimits);
					if (Terrain || Spatial) Output.Append(std::as_bytes(std::span(Value.TerrainBinding.Bytes)));
					if (Spatial) {
						if (Value.AuthorizedSpatialRegion != ObjectId{} && !Value.AuthorizedSpatialRegion.IsValid()) return false;
						Output.Integer(Value.AuthorizedSpatialRegion.Slot);
						Output.Integer(Value.AuthorizedSpatialRegion.Generation);
					} else if (Value.AuthorizedSpatialRegion != ObjectId{}) return false;
					return Value.Nonce != 0 && Value.SessionEpoch != 0 && Value.Replication.IsValid() &&
						   Value.Player.IsValid() && Value.PlayerId != 0 &&
						   Value.Identity == SessionIdentityKind::DevelopmentLocal &&
						   ValidLimits;
				} else {
					Output.Integer(Value.SessionEpoch);
					Output.Integer(Value.Replication.Value());
					WriteBinaryObjectId(Output, Value.Player);
					if (Terrain || Spatial) Output.Append(std::as_bytes(std::span(Value.TerrainBinding.Bytes)));
					return Value.SessionEpoch != 0 && Value.Replication.IsValid() && Value.Player.IsValid();
				}
			},
			Message
		);
		if (!RuntimeDistribution.empty()) Output.String(RuntimeDistribution);
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
		const bool Distribution = Version == GameSessionDistributionProtocolVersion;
		const bool Terrain = Version == GameSessionTerrainProtocolVersion;
		const bool ManagedSpatial = Version == GameSessionManagedSpatialProtocolVersion;
		const bool Spatial = Version == GameSessionSpatialProtocolVersion || ManagedSpatial;
		if (Version != GameSessionProtocolVersion && !Distribution && !Terrain && !Spatial)
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
			if (Distribution && (!Input.String(Value.RuntimeDistributionIdentity, 64) ||
				!IsGameSessionDistributionIdentity(Value.RuntimeDistributionIdentity)))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session client distribution identity is invalid");
			Result = std::move(Value);
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
			if (Distribution && (!Input.String(Value.RuntimeDistributionIdentity, 64) ||
				!IsGameSessionDistributionIdentity(Value.RuntimeDistributionIdentity)))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session server distribution identity is invalid");
			Result = std::move(Value);
		} else if (KindValue == static_cast<std::uint8_t>(GameSessionMessageKind::ClientReady)) {
			if (Distribution || ManagedSpatial)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session readiness has an invalid version");
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
		if (Terrain && !std::visit([&](auto &Value) {
			return ReadTerrainBinding(Input, Value.TerrainBinding) && Value.TerrainBinding.IsValid();
		}, Result))
			return SerializationFailure(SerializationErrorCode::InvalidValue, "Game session terrain binding is invalid");
		if (Spatial && !std::visit([&](auto &Value) {
			Value.SpatialContent = true;
			if (!ReadTerrainBinding(Input, Value.TerrainBinding)) return false;
			if constexpr (std::is_same_v<std::decay_t<decltype(Value)>, GameSessionServerAccepted>) {
				if (!Input.Integer(Value.AuthorizedSpatialRegion.Slot) || !Input.Integer(Value.AuthorizedSpatialRegion.Generation))
					return false;
				return Value.AuthorizedSpatialRegion.IsValid() || Value.AuthorizedSpatialRegion == ObjectId{};
			}
			return true;
		}, Result)) return SerializationFailure(SerializationErrorCode::InvalidValue, "Invalid spatial audience binding");
		if (ManagedSpatial && !std::visit([&](auto &Value) {
			if constexpr (std::is_same_v<std::decay_t<decltype(Value)>, GameSessionClientReady>) return false;
			else return Input.String(Value.RuntimeDistributionIdentity,64) &&
				IsGameSessionDistributionIdentity(Value.RuntimeDistributionIdentity);
		},Result)) return SerializationFailure(SerializationErrorCode::InvalidValue,"Invalid spatial package identity");
		if (!Input.Complete())
			return SerializationFailure(
				SerializationErrorCode::InvalidValue, "Game session frame contains trailing bytes"
			);
		return Result;
	}
}

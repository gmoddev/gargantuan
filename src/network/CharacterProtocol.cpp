#include "gargantuan/network/CharacterProtocol.hpp"

#include "gargantuan/network/BinaryCodec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <new>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t CharacterMagic = 0x52484347; // "GCHR" in little endian.
		constexpr std::uint8_t ValidInputFlags = static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested);
		constexpr std::uint8_t ValidStateFlags = static_cast<std::uint8_t>(CharacterStateFlag::Grounded) |
												 static_cast<std::uint8_t>(CharacterStateFlag::Teleport);

		bool IsFinite(const glm::vec2 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const CFrame &Value) {
			if (!IsFinite(Value.Position)) return false;
			for (int Column = 0; Column < 3; ++Column)
				for (int Row = 0; Row < 3; ++Row)
					if (!std::isfinite(Value.Rotation[Column][Row])) return false;
			const auto Quaternion = Value.ToQuaternion();
			return std::isfinite(Quaternion.x) && std::isfinite(Quaternion.y) && std::isfinite(Quaternion.z) &&
				   std::isfinite(Quaternion.w) && std::abs(glm::length(Quaternion) - 1.0f) <= 1.0e-3f;
		}

		void WriteAssetId(GameBinaryWriter &Output, AssetId Id) {
			Output.Integer(Id.High);
			Output.Integer(Id.Low);
		}

		bool ReadAssetId(GameBinaryReader &Input, AssetId &Id) {
			return Input.Integer(Id.High) && Input.Integer(Id.Low) &&
				   (Id.IsValid() || Input.Fail("Character Animation AssetId is invalid"));
		}

		void WriteContentId(GameBinaryWriter &Output, const AssetContentId &Id) {
			for (const auto Byte : Id.Bytes)
				Output.Integer(Byte);
		}

		bool ReadContentId(GameBinaryReader &Input, AssetContentId &Id) {
			for (auto &Byte : Id.Bytes)
				if (!Input.Integer(Byte)) return false;
			return Id.IsValid() || Input.Fail("Character Animation content revision is invalid");
		}

		void WriteCFrame(GameBinaryWriter &Output, const CFrame &Value) {
			Output.Float(Value.Position.x);
			Output.Float(Value.Position.y);
			Output.Float(Value.Position.z);
			const auto Rotation = Value.ToQuaternion();
			Output.Float(Rotation.x);
			Output.Float(Rotation.y);
			Output.Float(Rotation.z);
			Output.Float(Rotation.w);
		}

		bool ReadCFrame(GameBinaryReader &Input, CFrame &Value) {
			glm::vec3 Position;
			glm::quat Rotation;
			if (!Input.Float(Position.x) || !Input.Float(Position.y) || !Input.Float(Position.z) ||
				!Input.Float(Rotation.x) || !Input.Float(Rotation.y) || !Input.Float(Rotation.z) ||
				!Input.Float(Rotation.w))
				return false;
			if (!IsFinite(Position) || !std::isfinite(Rotation.x) || !std::isfinite(Rotation.y) ||
				!std::isfinite(Rotation.z) || !std::isfinite(Rotation.w) ||
				std::abs(glm::length(Rotation) - 1.0f) > 1.0e-3f)
				return Input.Fail("Character CFrame is not finite and normalized");
			Value = CFrame(Position, glm::mat3_cast(glm::normalize(Rotation)));
			return true;
		}

		void WriteVector3(GameBinaryWriter &Output, const glm::vec3 &Value) {
			Output.Float(Value.x);
			Output.Float(Value.y);
			Output.Float(Value.z);
		}

		bool ReadVector3(GameBinaryReader &Input, glm::vec3 &Value) {
			return Input.Float(Value.x) && Input.Float(Value.y) && Input.Float(Value.z);
		}

		bool QuantizeSigned(float Value, float Scale, std::int16_t &Result) {
			if (!std::isfinite(Value)) return false;
			const auto Scaled = std::round(static_cast<double>(Value) * static_cast<double>(Scale));
			if (Scaled < -32767.0 || Scaled > 32767.0) return false;
			Result = static_cast<std::int16_t>(Scaled);
			return true;
		}

		bool WriteCompactState(GameBinaryWriter &Output, const CharacterAuthoritativeState &State) {
			std::array<std::int16_t, 4> Rotation{};
			std::array<std::int16_t, 3> Velocity{};
			std::array<std::int16_t, 3> FloorNormal{};
			const auto Quaternion = State.Transform.ToQuaternion();
			if (!QuantizeSigned(Quaternion.x, 32767.0f, Rotation[0]) ||
				!QuantizeSigned(Quaternion.y, 32767.0f, Rotation[1]) ||
				!QuantizeSigned(Quaternion.z, 32767.0f, Rotation[2]) ||
				!QuantizeSigned(Quaternion.w, 32767.0f, Rotation[3]))
				return false;
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				if (!QuantizeSigned(State.Velocity[Axis], 64.0f, Velocity[Axis]) ||
					!QuantizeSigned(State.FloorNormal[Axis], 32767.0f, FloorNormal[Axis]))
					return false;

			WriteBinaryObjectId(Output, State.Character);
			Output.Integer(State.ControlEpoch.Value());
			Output.Integer(State.StateSequence.Value());
			Output.Integer(State.AcknowledgedInput.Value());
			Output.Integer(State.ResolvedAction.Value());
			Output.Float(State.Transform.Position.x);
			Output.Float(State.Transform.Position.y);
			Output.Float(State.Transform.Position.z);
			for (const auto Value : Rotation)
				Output.Integer(Value);
			for (const auto Value : Velocity)
				Output.Integer(Value);
			for (const auto Value : FloorNormal)
				Output.Integer(Value);
			Output.Integer(State.Flags);
			Output.Integer<std::uint8_t>(State.ActiveAction ? 1 : 0);
			if (State.ActiveAction) {
				Output.Integer(State.ActiveAction->ActionSequence.Value());
				Output.Integer(State.ActiveAction->ActionToken);
				WriteAssetId(Output, State.ActiveAction->Animation);
				WriteContentId(Output, State.ActiveAction->ContentRevision);
				Output.Integer(State.ActiveAction->StartTick);
				Output.Integer(State.ActiveAction->DurationTicks);
			}
			return Output.Succeeded();
		}

		bool ReadCompactState(GameBinaryReader &Input, std::uint64_t ServerTick, CharacterAuthoritativeState &State) {
			std::uint64_t Epoch = 0, StateSequence = 0, Acknowledged = 0, Resolved = 0;
			std::array<std::int16_t, 4> Rotation{};
			std::array<std::int16_t, 3> Velocity{};
			std::array<std::int16_t, 3> FloorNormal{};
			std::uint8_t HasAction = 0;
			if (!ReadBinaryObjectId(Input, State.Character) || !Input.Integer(Epoch) || !Input.Integer(StateSequence) ||
				!Input.Integer(Acknowledged) || !Input.Integer(Resolved) || !Input.Float(State.Transform.Position.x) ||
				!Input.Float(State.Transform.Position.y) || !Input.Float(State.Transform.Position.z))
				return false;
			for (auto &Value : Rotation)
				if (!Input.Integer(Value)) return false;
			for (auto &Value : Velocity)
				if (!Input.Integer(Value)) return false;
			for (auto &Value : FloorNormal)
				if (!Input.Integer(Value)) return false;
			if (!Input.Integer(State.Flags) || !Input.Integer(HasAction)) return false;
			if (HasAction > 1 || std::ranges::contains(Rotation, std::numeric_limits<std::int16_t>::min()) ||
				std::ranges::contains(Velocity, std::numeric_limits<std::int16_t>::min()) ||
				std::ranges::contains(FloorNormal, std::numeric_limits<std::int16_t>::min()))
				return Input.Fail("Character compact state contains an invalid quantized value");

			glm::quat Quaternion;
			Quaternion.x = static_cast<float>(Rotation[0]) * CompactCharacterUnitResolution;
			Quaternion.y = static_cast<float>(Rotation[1]) * CompactCharacterUnitResolution;
			Quaternion.z = static_cast<float>(Rotation[2]) * CompactCharacterUnitResolution;
			Quaternion.w = static_cast<float>(Rotation[3]) * CompactCharacterUnitResolution;
			const auto QuaternionLength = glm::length(Quaternion);
			if (!IsFinite(State.Transform.Position) || !std::isfinite(QuaternionLength) ||
				std::abs(QuaternionLength - 1.0f) > 1.0e-3f)
				return Input.Fail("Character compact transform is invalid");
			State.Transform.Rotation = glm::mat3_cast(glm::normalize(Quaternion));
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				State.Velocity[Axis] = static_cast<float>(Velocity[Axis]) * CompactCharacterVelocityResolution;
				State.FloorNormal[Axis] = static_cast<float>(FloorNormal[Axis]) * CompactCharacterUnitResolution;
			}
			State.ControlEpoch = CharacterControlEpoch(Epoch);
			State.StateSequence = RealtimeStateSequence(StateSequence);
			State.AcknowledgedInput = CharacterInputSequence(Acknowledged);
			State.ResolvedAction = CharacterActionSequence(Resolved);
			State.AuthoritativeTick = ServerTick;
			if (HasAction) {
				CharacterActionState Action;
				std::uint64_t ActionSequence = 0;
				if (!Input.Integer(ActionSequence) || !Input.Integer(Action.ActionToken) ||
					!ReadAssetId(Input, Action.Animation) || !ReadContentId(Input, Action.ContentRevision) ||
					!Input.Integer(Action.StartTick) || !Input.Integer(Action.DurationTicks))
					return false;
				Action.ActionSequence = CharacterActionSequence(ActionSequence);
				State.ActiveAction = Action;
			}
			return State.IsValid();
		}
	}

	bool CharacterControlTransition::IsValid() const {
		return Character.IsValid() && ControlEpoch.IsValid() && AuthoritativeTick != 0 && (Channel.IsValid() == Bound);
	}

	bool CharacterInputCommand::IsValid() const {
		return Character.IsValid() && ControlEpoch.IsValid() && InputSequence.IsValid() && SimulationTick != 0 &&
			   std::isfinite(DeltaSeconds) && DeltaSeconds > 0.0f && DeltaSeconds <= MaximumCharacterCommandInterval &&
			   IsFinite(MoveIntent) && glm::length(MoveIntent) <= MaximumCharacterMoveIntentMagnitude &&
			   std::isfinite(FacingYawRadians) && std::abs(FacingYawRadians) <= 3.14159265358979323846f &&
			   (Flags & ~ValidInputFlags) == 0;
	}

	bool CharacterActionRequest::IsValid() const {
		return Character.IsValid() && ControlEpoch.IsValid() && ActionSequence.IsValid() && RequestedActionToken != 0;
	}

	bool CharacterActionState::IsValid() const {
		return ActionSequence.IsValid() && ActionToken != 0 && Animation.IsValid() && ContentRevision.IsValid() &&
			   StartTick != 0 && DurationTicks != 0;
	}

	bool CharacterAuthoritativeState::IsValid() const {
		return Character.IsValid() && ControlEpoch.IsValid() && StateSequence.IsValid() && AuthoritativeTick != 0 &&
			   IsFinite(Transform) && IsFinite(Velocity) && IsFinite(FloorNormal) &&
			   glm::length(FloorNormal) <= 1.001f && (Flags & ~ValidStateFlags) == 0 &&
			   (!ActiveAction || ActiveAction->IsValid()) &&
			   (!ActiveAction || !ResolvedAction.IsValid() ||
				ActiveAction->ActionSequence.Value() <= ResolvedAction.Value());
	}

	std::size_t GetCompactCharacterStateEncodedBytes(const CharacterAuthoritativeState &State) {
		if (!State.IsValid()) return 0;
		std::int16_t Quantized = 0;
		const auto Rotation = State.Transform.ToQuaternion();
		if (!QuantizeSigned(Rotation.x, 32767.0f, Quantized) || !QuantizeSigned(Rotation.y, 32767.0f, Quantized) ||
			!QuantizeSigned(Rotation.z, 32767.0f, Quantized) || !QuantizeSigned(Rotation.w, 32767.0f, Quantized))
			return 0;
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
			if (!QuantizeSigned(State.Velocity[Axis], 64.0f, Quantized) ||
				!QuantizeSigned(State.FloorNormal[Axis], 32767.0f, Quantized))
				return 0;
		return CompactCharacterStateBytes + (State.ActiveAction ? CompactCharacterActionStateBytes : 0);
	}

	bool CharacterStateFrame::IsValid() const {
		if (ServerTick == 0 || !FrameSequence.IsValid() || !MaterializationEpoch.IsValid() ||
			MaterializationEpoch.Value() > std::numeric_limits<std::uint16_t>::max() || StateCount == 0 ||
			StateCount > States.size())
			return false;
		ObjectId Previous;
		for (std::size_t Index = 0; Index < StateCount; ++Index) {
			const auto &State = States[Index];
			if (State.AuthoritativeTick != ServerTick || GetCompactCharacterStateEncodedBytes(State) == 0) return false;
			if (Index != 0 && !(Previous < State.Character)) return false;
			Previous = State.Character;
		}
		return true;
	}

	CharacterMessageKind GetCharacterMessageKind(const CharacterMessage &Message) {
		return std::visit(
			[](const auto &Value) {
				using Type = std::decay_t<decltype(Value)>;
				if constexpr (std::is_same_v<Type, CharacterControlTransition>)
					return Value.Bound ? CharacterMessageKind::ControlBind : CharacterMessageKind::ControlUnbind;
				if constexpr (std::is_same_v<Type, CharacterInputCommand>) return CharacterMessageKind::Input;
				if constexpr (std::is_same_v<Type, CharacterActionRequest>) return CharacterMessageKind::ActionRequest;
				if constexpr (std::is_same_v<Type, CharacterStateFrame>) return CharacterMessageKind::StateFrame;
				return CharacterMessageKind::State;
			},
			Message
		);
	}

	SerializationResult<std::vector<std::byte>> EncodeCharacterMessage(const CharacterMessage &Message) {
		try {
			const bool Valid = std::visit([](const auto &Value) { return Value.IsValid(); }, Message);
			if (!Valid)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Character message is invalid");
			const bool StateFrame = std::holds_alternative<CharacterStateFrame>(Message);
			GameBinaryWriter Output(StateFrame ? MaximumCharacterStateFrameBytes : MaximumCharacterFrameBytes);
			Output.Bytes.reserve(StateFrame ? MaximumCharacterStateFrameBytes : MaximumCharacterFrameBytes);
			Output.Integer(CharacterMagic);
			Output.Integer(StateFrame ? CharacterProtocolVersion : LegacyCharacterProtocolVersion);
			Output.Integer(static_cast<std::uint8_t>(GetCharacterMessageKind(Message)));
			Output.Integer<std::uint8_t>(0);
			std::visit(
				[&](const auto &Value) {
					using Type = std::decay_t<decltype(Value)>;
					if constexpr (std::is_same_v<Type, CharacterStateFrame>) {
						Output.Integer(Value.ServerTick);
						Output.Integer(Value.FrameSequence.Value());
						Output.Integer(Value.StateCount);
						Output.Integer(static_cast<std::uint16_t>(Value.MaterializationEpoch.Value()));
						for (const auto &State : Value.GetStates())
							if (!WriteCompactState(Output, State)) return;
					} else {
						WriteBinaryObjectId(Output, Value.Character);
						Output.Integer(Value.ControlEpoch.Value());
						if constexpr (std::is_same_v<Type, CharacterControlTransition>) {
							Output.Integer(Value.Channel.Value());
							Output.Integer(Value.AuthoritativeTick);
						} else if constexpr (std::is_same_v<Type, CharacterInputCommand>) {
							Output.Integer(Value.InputSequence.Value());
							Output.Integer(Value.SimulationTick);
							Output.Float(Value.DeltaSeconds);
							Output.Float(Value.MoveIntent.x);
							Output.Float(Value.MoveIntent.y);
							Output.Float(Value.FacingYawRadians);
							Output.Integer(Value.Flags);
							Output.Integer<std::uint8_t>(0);
							Output.Integer<std::uint16_t>(0);
						} else if constexpr (std::is_same_v<Type, CharacterActionRequest>) {
							Output.Integer(Value.ActionSequence.Value());
							Output.Integer(Value.BasedOnInput.Value());
							Output.Integer(Value.RequestedActionToken);
							Output.Integer<std::uint32_t>(0);
						} else {
							Output.Integer(Value.StateSequence.Value());
							Output.Integer(Value.AcknowledgedInput.Value());
							Output.Integer(Value.ResolvedAction.Value());
							Output.Integer(Value.AuthoritativeTick);
							WriteCFrame(Output, Value.Transform);
							WriteVector3(Output, Value.Velocity);
							WriteVector3(Output, Value.FloorNormal);
							Output.Integer(Value.Flags);
							Output.Integer<std::uint8_t>(Value.ActiveAction ? 1 : 0);
							Output.Integer<std::uint16_t>(0);
							if (Value.ActiveAction) {
								Output.Integer(Value.ActiveAction->ActionSequence.Value());
								Output.Integer(Value.ActiveAction->ActionToken);
								WriteAssetId(Output, Value.ActiveAction->Animation);
								WriteContentId(Output, Value.ActiveAction->ContentRevision);
								Output.Integer(Value.ActiveAction->StartTick);
								Output.Integer(Value.ActiveAction->DurationTicks);
								Output.Integer<std::uint32_t>(0);
							}
						}
					}
				},
				Message
			);
			if (!Output.Succeeded())
				return SerializationFailure(SerializationErrorCode::LimitExceeded, "Character frame exceeds its limit");
			return std::move(Output.Bytes);
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Character encode allocation failed");
		}
	}

	SerializationResult<CharacterMessage> DecodeCharacterMessage(std::span<const std::byte> Bytes) {
		try {
			if (Bytes.size() > MaximumCharacterStateFrameBytes)
				return SerializationFailure(SerializationErrorCode::LimitExceeded, "Character frame exceeds its limit");
			GameBinaryReader Input(Bytes, "Character frame");
			std::uint32_t Magic = 0;
			std::uint16_t Version = 0;
			std::uint8_t KindValue = 0, Reserved = 0;
			if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(KindValue) ||
				!Input.Integer(Reserved))
				return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
			if (Magic != CharacterMagic)
				return SerializationFailure(SerializationErrorCode::InvalidSyntax, "Character frame magic is invalid");
			if (Reserved != 0)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Character frame header is invalid");

			if (Version == CharacterProtocolVersion) {
				if (KindValue != static_cast<std::uint8_t>(CharacterMessageKind::StateFrame))
					return SerializationFailure(SerializationErrorCode::InvalidValue, "Character v3 opcode is invalid");
				CharacterStateFrame Frame;
				std::uint64_t FrameSequence = 0;
				std::uint16_t MaterializationEpoch = 0;
				if (!Input.Integer(Frame.ServerTick) || !Input.Integer(FrameSequence) ||
					!Input.Integer(Frame.StateCount) || !Input.Integer(MaterializationEpoch))
					return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
				if (MaterializationEpoch == 0 || Frame.StateCount == 0 || Frame.StateCount > Frame.States.size())
					return SerializationFailure(
						SerializationErrorCode::InvalidValue,
						"Character state-frame materialization epoch or count is invalid"
					);
				Frame.FrameSequence = CharacterStateFrameSequence(FrameSequence);
				Frame.MaterializationEpoch = CharacterMaterializationEpoch(MaterializationEpoch);
				for (std::size_t Index = 0; Index < Frame.StateCount; ++Index)
					if (!ReadCompactState(Input, Frame.ServerTick, Frame.States[Index]))
						return SerializationFailure(SerializationErrorCode::InvalidValue, Input.Error);
				if (!Input.Complete() || !Frame.IsValid())
					return SerializationFailure(
						SerializationErrorCode::InvalidValue,
						"Character state frame contains trailing, duplicate, unordered, or invalid data"
					);
				return CharacterMessage(Frame);
			}

			if (Version != LegacyCharacterProtocolVersion)
				return SerializationFailure(
					SerializationErrorCode::UnsupportedVersion, "Character protocol version is unsupported"
				);
			if (Bytes.size() > MaximumCharacterFrameBytes ||
				KindValue > static_cast<std::uint8_t>(CharacterMessageKind::State))
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Character v1 frame is invalid");
			ObjectId Character;
			std::uint64_t Epoch = 0;
			if (!ReadBinaryObjectId(Input, Character) || !Input.Integer(Epoch))
				return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
			if (Epoch == 0)
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Character frame header is invalid");
			const auto Kind = static_cast<CharacterMessageKind>(KindValue);
			CharacterMessage Message;
			if (Kind == CharacterMessageKind::ControlBind || Kind == CharacterMessageKind::ControlUnbind) {
				std::uint64_t Channel = 0, Tick = 0;
				if (!Input.Integer(Channel) || !Input.Integer(Tick))
					return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
				Message = CharacterControlTransition{
					Character,
					CharacterControlEpoch(Epoch),
					StateChannelId(Channel),
					Tick,
					Kind == CharacterMessageKind::ControlBind
				};
			} else if (Kind == CharacterMessageKind::Input) {
				std::uint64_t Sequence = 0, Tick = 0;
				std::uint8_t Flags = 0, Tail = 0;
				std::uint16_t ReservedTail = 0;
				CharacterInputCommand Value{.Character = Character, .ControlEpoch = CharacterControlEpoch(Epoch)};
				if (!Input.Integer(Sequence) || !Input.Integer(Tick) || !Input.Float(Value.DeltaSeconds) ||
					!Input.Float(Value.MoveIntent.x) || !Input.Float(Value.MoveIntent.y) ||
					!Input.Float(Value.FacingYawRadians) || !Input.Integer(Flags) || !Input.Integer(Tail) ||
					!Input.Integer(ReservedTail))
					return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
				if (Tail != 0 || ReservedTail != 0)
					return SerializationFailure(
						SerializationErrorCode::InvalidValue, "Character input reserved bytes are nonzero"
					);
				Value.InputSequence = CharacterInputSequence(Sequence);
				Value.SimulationTick = Tick;
				Value.Flags = Flags;
				Message = Value;
			} else if (Kind == CharacterMessageKind::ActionRequest) {
				std::uint64_t Sequence = 0, BasedOn = 0;
				std::uint32_t Token = 0, ReservedTail = 0;
				if (!Input.Integer(Sequence) || !Input.Integer(BasedOn) || !Input.Integer(Token) ||
					!Input.Integer(ReservedTail))
					return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
				if (ReservedTail != 0)
					return SerializationFailure(
						SerializationErrorCode::InvalidValue, "Character action reserved bytes are nonzero"
					);
				Message = CharacterActionRequest{
					Character,
					CharacterControlEpoch(Epoch),
					CharacterActionSequence(Sequence),
					CharacterInputSequence(BasedOn),
					Token
				};
			} else {
				CharacterAuthoritativeState Value{.Character = Character, .ControlEpoch = CharacterControlEpoch(Epoch)};
				std::uint64_t StateSequence = 0, Acknowledged = 0, Resolved = 0;
				std::uint8_t Flags = 0, HasAction = 0;
				std::uint16_t ReservedTail = 0;
				if (!Input.Integer(StateSequence) || !Input.Integer(Acknowledged) || !Input.Integer(Resolved) ||
					!Input.Integer(Value.AuthoritativeTick) || !ReadCFrame(Input, Value.Transform) ||
					!ReadVector3(Input, Value.Velocity) || !ReadVector3(Input, Value.FloorNormal) ||
					!Input.Integer(Flags) || !Input.Integer(HasAction) || !Input.Integer(ReservedTail))
					return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
				if (HasAction > 1 || ReservedTail != 0)
					return SerializationFailure(
						SerializationErrorCode::InvalidValue, "Character state flags are invalid"
					);
				Value.StateSequence = RealtimeStateSequence(StateSequence);
				Value.AcknowledgedInput = CharacterInputSequence(Acknowledged);
				Value.ResolvedAction = CharacterActionSequence(Resolved);
				Value.Flags = Flags;
				if (HasAction) {
					CharacterActionState Action;
					std::uint64_t ActionSequence = 0;
					std::uint32_t ActionReserved = 0;
					if (!Input.Integer(ActionSequence) || !Input.Integer(Action.ActionToken) ||
						!ReadAssetId(Input, Action.Animation) || !ReadContentId(Input, Action.ContentRevision) ||
						!Input.Integer(Action.StartTick) || !Input.Integer(Action.DurationTicks) ||
						!Input.Integer(ActionReserved))
						return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
					if (ActionReserved != 0)
						return SerializationFailure(
							SerializationErrorCode::InvalidValue, "Character action state reserved bytes are nonzero"
						);
					Action.ActionSequence = CharacterActionSequence(ActionSequence);
					Value.ActiveAction = Action;
				}
				Message = Value;
			}
			if (!Input.Complete() || !std::visit([](const auto &Value) { return Value.IsValid(); }, Message))
				return SerializationFailure(
					SerializationErrorCode::InvalidValue, "Character frame contains trailing or invalid data"
				);
			return Message;
		} catch (const std::bad_alloc &) {
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "Character decode allocation failed");
		}
	}
}

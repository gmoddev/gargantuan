#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/serialization/SerializationError.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace gargantuan::network {
	inline constexpr std::uint16_t LegacyCharacterProtocolVersion = 1;
	inline constexpr std::uint16_t CharacterProtocolVersion = 4;
	inline constexpr std::size_t MaximumCharacterFrameBytes = 256;
	inline constexpr std::size_t MaximumCharacterStateFrameBytes = 1200;
	inline constexpr std::size_t MaximumCharacterStatesPerFrame = 15;
	inline constexpr std::size_t CompactCharacterStateBytes = 74;
	inline constexpr std::size_t CompactCharacterActionStateBytes = 72;
	inline constexpr std::size_t CharacterStateFrameHeaderBytes = 34;
	inline constexpr float CompactCharacterVelocityResolution = 1.0f / 64.0f;
	inline constexpr float MaximumCompactCharacterVelocity = 32767.0f / 64.0f;
	inline constexpr float CompactCharacterUnitResolution = 1.0f / 32767.0f;
	inline constexpr float MaximumCharacterCommandInterval = 0.25f;
	inline constexpr float MaximumCharacterMoveIntentMagnitude = 1.001f;

	enum class CharacterMessageKind : std::uint8_t {
		ControlBind = 0,
		ControlUnbind = 1,
		Input = 2,
		ActionRequest = 3,
		State = 4,
		StateFrame = 5,
		ActionResult = 6,
	};

	enum class CharacterInputFlag : std::uint8_t {
		JumpRequested = 1 << 0,
	};

	enum class CharacterStateFlag : std::uint8_t {
		Grounded = 1 << 0,
		Teleport = 1 << 1,
	};

	struct CharacterControlTransition {
		ObjectId Character;
		CharacterControlEpoch ControlEpoch;
		StateChannelId Channel;
		std::uint64_t AuthoritativeTick = 0;
		bool Bound = false;

		[[nodiscard]] bool IsValid() const;
	};

	struct CharacterInputCommand {
		ObjectId Character;
		CharacterControlEpoch ControlEpoch;
		CharacterInputSequence InputSequence;
		std::uint64_t SimulationTick = 0;
		float DeltaSeconds = 0.0f;
		glm::vec2 MoveIntent{0.0f};
		float FacingYawRadians = 0.0f;
		std::uint8_t Flags = 0;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool JumpRequested() const {
			return (Flags & static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested)) != 0;
		}
	};

	struct CharacterActionRequest {
		ObjectId Character;
		CharacterControlEpoch ControlEpoch;
		CharacterActionSequence ActionSequence;
		CharacterInputSequence BasedOnInput;
		std::uint32_t RequestedActionToken = 0;

		[[nodiscard]] bool IsValid() const;
	};

	struct CharacterActionState {
		CharacterActionSequence ActionSequence;
		std::uint32_t ActionToken = 0;
		AssetId Animation;
		AssetContentId ContentRevision;
		std::uint64_t StartTick = 0;
		std::uint32_t DurationTicks = 0;

		[[nodiscard]] bool IsValid() const;
	};

	struct CharacterActionResult {
		ObjectId Character;
		CharacterControlEpoch ControlEpoch;
		CharacterActionSequence ActionSequence;
		std::uint32_t RequestedActionToken = 0;
		bool Accepted = false;
		std::optional<CharacterActionState> AuthoritativeAction;

		[[nodiscard]] bool IsValid() const;
	};

	struct CharacterAuthoritativeState {
		ObjectId Character;
		CharacterControlEpoch ControlEpoch;
		RealtimeStateSequence StateSequence;
		CharacterInputSequence AcknowledgedInput;
		CharacterActionSequence ResolvedAction;
		std::uint64_t AuthoritativeTick = 0;
		CFrame Transform;
		glm::vec3 Velocity{0.0f};
		glm::vec3 FloorNormal{0.0f};
		std::uint8_t Flags = 0;
		std::optional<CharacterActionState> ActiveAction;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] bool Grounded() const {
			return (Flags & static_cast<std::uint8_t>(CharacterStateFlag::Grounded)) != 0;
		}
		[[nodiscard]] bool Teleport() const {
			return (Flags & static_cast<std::uint8_t>(CharacterStateFlag::Teleport)) != 0;
		}
	};

	struct CharacterStateFrame {
		std::uint64_t ServerTick = 0;
		CharacterStateFrameSequence FrameSequence;
		CharacterMaterializationEpoch MaterializationEpoch{1};
		std::array<CharacterAuthoritativeState, MaximumCharacterStatesPerFrame> States{};
		std::uint16_t StateCount = 0;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] std::span<const CharacterAuthoritativeState> GetStates() const {
			return std::span<const CharacterAuthoritativeState>(States.data(), StateCount);
		}
	};

	using CharacterMessage = std::variant<
		CharacterControlTransition,
		CharacterInputCommand,
		CharacterActionRequest,
		CharacterActionResult,
		CharacterAuthoritativeState,
		CharacterStateFrame>;

	[[nodiscard]] CharacterMessageKind GetCharacterMessageKind(const CharacterMessage &Message);
	[[nodiscard]] std::size_t GetCompactCharacterStateEncodedBytes(const CharacterAuthoritativeState &State);
	[[nodiscard]] SerializationResult<std::vector<std::byte>> EncodeCharacterMessage(const CharacterMessage &Message);
	[[nodiscard]] SerializationResult<CharacterMessage> DecodeCharacterMessage(std::span<const std::byte> Bytes);
}

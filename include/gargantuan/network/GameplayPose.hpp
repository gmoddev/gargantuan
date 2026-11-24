#pragma once

#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/network/MessageIntent.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <variant>

namespace gargantuan::network {
	struct ReceivedMessageEvent;

	inline constexpr std::size_t MaximumGameplayPoseReplicas = 16;
	inline constexpr std::size_t MaximumGameplayPoseSamples = 4;
	inline constexpr std::uint64_t GameplayPoseInterpolationDelayTicks = 6;
	inline constexpr std::uint64_t MaximumGameplayPoseWindowTicks = 15;

	struct GameplayPoseControl {
		ObjectId Part;
		std::uint64_t MaterializationEpoch = 0;
		bool Published = false;
		std::uint64_t RequiredStructuralSequence = 0;
	};

	struct GameplayPoseSnapshot {
		ObjectId Part;
		std::uint64_t MaterializationEpoch = 0;
		std::uint64_t ServerTick = 0;
		CFrame Transform;
		bool Teleport = false;
	};

	using GameplayPoseMessage = std::variant<GameplayPoseControl, GameplayPoseSnapshot>;

	[[nodiscard]] bool IsValidGameplayPoseTransform(const CFrame &Transform);
	[[nodiscard]] bool IsGameplayPoseFrame(std::span<const std::byte> Bytes);
	[[nodiscard]] std::optional<NetworkMessageIntent> MakeGameplayPoseIntent(ConnectionId Connection,
		const GameplayPoseMessage &Message, const NetworkLimits &Limits);
	[[nodiscard]] std::optional<GameplayPoseMessage> DecodeGameplayPose(const ReceivedMessageEvent &Event);

	// Main-owned, client-only value data. GameSession gates application on the
	// matching source object and the Publish's required structural sequence.
	class GameplayPosePresentation final {
	  public:
		[[nodiscard]] bool ApplyControl(const GameplayPoseControl &Control);
		void Forget(ObjectId Part);
		void Clear();
		[[nodiscard]] bool Accept(const GameplayPoseSnapshot &Snapshot, std::uint64_t LocalTick);
		[[nodiscard]] std::optional<GameplayPoseSnapshot> GetLatest(ObjectId Part) const;
		[[nodiscard]] std::optional<CFrame> Sample(ObjectId Part, std::uint64_t LocalTick);

	  private:
		struct Replica {
			std::uint64_t MaterializationEpoch;
			std::uint64_t LastReceiptTick = 0;
			std::uint64_t LastPresentedServerTick = 0;
			std::array<GameplayPoseSnapshot, MaximumGameplayPoseSamples> History{};
			std::size_t Count = 0;
		};

		std::map<ObjectId, Replica> Replicas;
		std::uint64_t HighestPublishedEpoch = 0;
	};
}

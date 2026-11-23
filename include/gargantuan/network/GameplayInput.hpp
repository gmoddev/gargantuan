#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/Sequence.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>

#include <glm/vec2.hpp>

namespace gargantuan::network {
	class INetworkScheduler;
	struct NetworkLimits;
	struct ReceivedMessageEvent;

	inline constexpr std::size_t MaximumGameplayInputLeases = 1024;
	inline constexpr std::uint32_t MaximumGameplayInputsPerTick = 4;
	inline constexpr std::uint64_t GameplayInputFreshnessTicks = 6;

	struct GameplayInputLeaseEpochDomain;
	struct GameplayInputSequenceDomain;
	using GameplayInputLeaseEpoch = MonotonicSequence<GameplayInputLeaseEpochDomain>;
	using GameplayInputSequence = MonotonicSequence<GameplayInputSequenceDomain>;

	struct GameplayInputSample {
		glm::vec2 Movement{0.0f};
		float FacingYawRadians = 0.0f;
		bool JumpRequested = false;
		bool Fresh = false;
		std::uint64_t AuthoritativeTick = 0;
		GameplayInputSequence AcknowledgedInput;
		std::uint64_t AgeTicks = 0;
	};

	struct GameplayInputMetrics {
		std::uint64_t Bindings = 0;
		std::uint64_t Revocations = 0;
		std::uint64_t InputsAccepted = 0;
		std::uint64_t InputsConsumed = 0;
		std::uint64_t InputsCoalesced = 0;
		std::uint64_t MalformedInputs = 0;
		std::uint64_t UnauthorizedInputs = 0;
		std::uint64_t StaleInputs = 0;
		std::uint64_t RateLimitedInputs = 0;
		std::uint64_t InputsExpired = 0;
		std::uint64_t MaximumConsumedInputAgeTicks = 0;
		std::uint64_t InputBytesReceived = 0;
		std::uint64_t AdmissionNanoseconds = 0;
	};

	[[nodiscard]] bool IsGameplayInputFrame(std::span<const std::byte> Bytes);

	// Main-owned. GameSession admits only ready peers and supplies the live
	// authoritative possession and accepted materialization for each binding.
	class GameplayInputServer final {
	  public:
		explicit GameplayInputServer(INetworkScheduler &SchedulerValue) : Scheduler(SchedulerValue) {}

		// Invalid Entity clears the binding. False means reliable control could
		// not be queued or the requested binding is invalid. The caller retires
		// that peer on reliable failure.
		[[nodiscard]] bool SetPossession(ConnectionId Connection, ObjectId Entity, std::uint64_t ServerTick,
			const NetworkLimits &Limits, std::uint64_t PossessionRevision = 0);
		void RemovePeer(ConnectionId Connection);
		[[nodiscard]] bool HandleReceived(const ReceivedMessageEvent &Event, std::uint64_t ServerTick);
		[[nodiscard]] std::optional<GameplayInputSample> Consume(ConnectionId Connection, ObjectId CurrentEntity,
			std::uint64_t ServerTick, std::uint64_t PossessionRevision = 0);
		[[nodiscard]] const GameplayInputMetrics &GetMetrics() const { return Metrics; }

	  private:
		struct Lease {
			ObjectId Entity;
			GameplayInputLeaseEpoch Epoch;
			std::uint64_t PossessionRevision = 0;
			std::uint64_t BoundTick = 0;
			std::uint64_t RateTick = 0;
			std::uint32_t AttemptsAtRateTick = 0;
			std::uint64_t LastClientTick = 0;
			std::uint64_t LastConsumedTick = 0;
			std::uint64_t ArrivalTick = 0;
			std::uint64_t JumpArrivalTick = 0;
			GameplayInputSequence LastInput;
			GameplayInputSequence AcknowledgedInput;
			glm::vec2 Movement{0.0f};
			float FacingYawRadians = 0.0f;
		};

		INetworkScheduler &Scheduler;
		std::map<ConnectionId, Lease> Leases;
		GameplayInputLeaseEpoch NextEpoch{1};
		GameplayInputMetrics Metrics;
	};

	class GameplayInputClient final {
	  public:
		explicit GameplayInputClient(INetworkScheduler &SchedulerValue) : Scheduler(SchedulerValue) {}

		[[nodiscard]] bool HandleReceived(const ReceivedMessageEvent &Event);
		[[nodiscard]] bool Submit(ConnectionId Connection, ObjectId CurrentEntity, std::uint64_t ClientTick,
			glm::vec2 Movement, float FacingYawRadians, bool JumpRequested, const NetworkLimits &Limits);
		[[nodiscard]] ObjectId GetPossessedEntity() const { return Entity; }
		[[nodiscard]] std::uint64_t GetInputsProduced() const { return InputsProduced; }
		void Reset();

	  private:
		INetworkScheduler &Scheduler;
		ConnectionId ServerConnection;
		ObjectId Entity;
		GameplayInputLeaseEpoch Epoch;
		GameplayInputSequence NextInput{1};
		std::uint64_t LastControlTick = 0;
		std::uint64_t LastSubmittedTick = 0;
		std::uint64_t InputsProduced = 0;
	};
}

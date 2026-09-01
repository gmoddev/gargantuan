#pragma once

#include "gargantuan/animation/RootMotion.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/network/CharacterProtocol.hpp"
#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/Transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace gargantuan {
	class KinematicCharacter;
	class WorldRoot;
}

namespace gargantuan::network {
	inline constexpr std::size_t MaximumCharacterPredictionHistory = 64;
	inline constexpr std::size_t MaximumPendingCharacterActions = 8;
	inline constexpr std::size_t MaximumNetworkCharacters = 4096;
	inline constexpr std::size_t MaximumCharacterNetworkPeers = 1024;
	inline constexpr std::uint32_t MaximumCharacterCommandsPerTick = 4;
	inline constexpr std::uint64_t MaximumCharacterCommandTickLead = 8;
	inline constexpr float MaximumHardCorrectionDistance = 8.0f;
	inline constexpr float MaximumSmoothPresentationCorrectionDistance = 1.0f;
	inline constexpr float TinyPresentationCorrectionDistance = 0.01f;
	inline constexpr std::size_t MaximumRemoteCharacterSnapshots = 4;
	inline constexpr std::uint64_t MaximumRemoteCharacterSnapshotWindowTicks = 15;
	inline constexpr std::uint32_t DefaultCharacterSimulationTicksPerSecond = 60;
	inline constexpr std::uint32_t DefaultCharacterStateUpdatesPerSecond = 20;
	inline constexpr std::uint64_t DefaultCharacterAbsoluteRefreshTicks = 60;
	inline constexpr std::uint64_t DefaultRemoteInterpolationDelayTicks = 6;
	inline constexpr std::uint64_t TinyCorrectionSmoothingTicks = 3;
	inline constexpr std::uint64_t SmallCorrectionSmoothingTicks = 6;

	struct CharacterNetworkConfiguration {
		std::uint32_t SimulationTicksPerSecond = DefaultCharacterSimulationTicksPerSecond;
		std::uint32_t StateUpdatesPerSecond = DefaultCharacterStateUpdatesPerSecond;
		std::uint64_t AbsoluteRefreshTicks = DefaultCharacterAbsoluteRefreshTicks;
		std::uint64_t RemoteInterpolationDelayTicks = DefaultRemoteInterpolationDelayTicks;
		std::size_t MaximumStateFrameBytes = MaximumCharacterStateFrameBytes;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] std::uint64_t PublicationIntervalTicks() const;
	};

	struct CharacterActionDefinition {
		std::uint32_t Token = 0;
		AssetId Animation;
		AssetContentId ContentRevision;
		std::uint32_t DurationTicks = 0;
		std::function<std::optional<RootMotionDelta>(std::uint64_t FromTick, std::uint64_t ToTick)> EvaluateRootMotion;

		[[nodiscard]] bool IsValid() const;
	};

	using CharacterMovementPolicy = std::function<
		CharacterMotionRequest(const CharacterInputCommand &Command, const KinematicCharacter &Character)>;
	using CharacterActionPolicy =
		std::function<std::optional<std::uint32_t>(ConnectionId Connection, const CharacterActionRequest &Request)>;

	struct CharacterNetworkMetrics {
		std::uint64_t CommandsReceived = 0;
		std::uint64_t CommandsAccepted = 0;
		std::uint64_t StaleCommandsRejected = 0;
		std::uint64_t UnauthorizedCommandsRejected = 0;
		std::uint64_t RateLimitedCommands = 0;
		std::uint64_t ActionRequestsAccepted = 0;
		std::uint64_t ActionRequestsRejected = 0;
		std::uint64_t AuthoritativeStatesSent = 0;
		std::uint64_t StatesConsidered = 0;
		std::uint64_t StatesSuppressedUnchanged = 0;
		std::uint64_t AbsoluteStatesSent = 0;
		std::uint64_t DeltaStatesSent = 0;
		std::uint64_t SemanticStateBytes = 0;
		std::uint64_t CompactStateBytes = 0;
		std::uint64_t StateFramesEmitted = 0;
		std::uint64_t StatesInFrames = 0;
		std::uint64_t BatchSplits = 0;
		std::uint64_t SchedulerSubmissions = 0;
		std::uint64_t BaselineMisses = 0;
		std::uint64_t StaleStatesDropped = 0;
		std::uint64_t InterpolationBufferUnderruns = 0;
		std::uint64_t InterpolationResets = 0;
		std::uint64_t LocalSmoothCorrections = 0;
		std::uint64_t HardPresentationResets = 0;
		std::uint64_t MalformedFramesRejected = 0;
		std::uint64_t PredictionCorrections = 0;
		std::uint64_t HardResets = 0;
		std::uint64_t PredictedCommandsReplayed = 0;
		std::uint64_t HistoryOverflows = 0;
		std::uint64_t BytesIn = 0;
		std::uint64_t BytesOut = 0;
		std::uint64_t ProtocolRejects = 0;
		std::uint64_t ProtocolDecodeCpuNanoseconds = 0;
		std::uint64_t CommandAdmissionCpuNanoseconds = 0;
		std::uint64_t MovementCpuNanoseconds = 0;
		std::uint64_t RootMotionCpuNanoseconds = 0;
		std::uint64_t StateEncodeCpuNanoseconds = 0;
		std::uint64_t StateChangeDetectionCpuNanoseconds = 0;
		std::uint64_t StateFrameAssemblyCpuNanoseconds = 0;
		std::uint64_t SchedulerSubmitCpuNanoseconds = 0;
		std::uint64_t ReconciliationCpuNanoseconds = 0;
		std::uint64_t ReplayCpuNanoseconds = 0;
		std::uint64_t InterpolationCpuNanoseconds = 0;
	};

	class AuthoritativeCharacterNetwork final {
	  public:
		AuthoritativeCharacterNetwork(
			INetworkScheduler &Scheduler,
			NetworkLimits Limits,
			CharacterMovementPolicy MovementPolicy,
			CharacterActionPolicy ActionPolicy = {},
			CharacterNetworkConfiguration Configuration = {}
		);
		~AuthoritativeCharacterNetwork();

		bool AddPeer(ConnectionId Connection);
		bool RemovePeer(ConnectionId Connection, std::uint64_t AuthoritativeTick);
		bool RegisterCharacter(const std::shared_ptr<KinematicCharacter> &Character);
		bool UnregisterCharacter(ObjectId Character, std::uint64_t AuthoritativeTick);
		bool MarkMaterialized(ConnectionId Connection, ObjectId Character, StateChannelId Channel);
		bool MarkUnmaterialized(ConnectionId Connection, ObjectId Character);
		[[nodiscard]] std::optional<CharacterControlEpoch>
		BindControl(ConnectionId Connection, ObjectId Character, std::uint64_t AuthoritativeTick);
		bool RevokeControl(ObjectId Character, std::uint64_t AuthoritativeTick);
		bool RegisterAction(CharacterActionDefinition Definition);
		bool StartServerAction(ObjectId Character, std::uint32_t ActionToken, std::uint64_t AuthoritativeTick);
		bool HandleTransportEvent(const TransportEvent &Event);
		void Step(WorldRoot &World, std::uint64_t AuthoritativeTick);
		bool
		PublishState(ObjectId Character, std::uint64_t AuthoritativeTick, bool Teleport = false, bool Reliable = false);

		[[nodiscard]] CharacterNetworkMetrics GetMetrics() const {
			return Metrics;
		}

	  private:
		struct PeerState;
		struct CharacterState;
		INetworkScheduler &Scheduler;
		NetworkLimits Limits;
		CharacterMovementPolicy MovementPolicy;
		CharacterActionPolicy ActionPolicy;
		CharacterNetworkConfiguration Configuration;
		std::map<ConnectionId, PeerState> Peers;
		std::map<ObjectId, CharacterState> Characters;
		std::map<std::uint32_t, CharacterActionDefinition> Actions;
		CharacterControlEpoch NextControlEpoch{1};
		std::uint64_t LastAuthoritativeTick = 0;
		std::uint64_t LastStatePublicationTick = 0;
		CharacterNetworkMetrics Metrics;

		bool Queue(ConnectionId Connection, const CharacterMessage &Message, StateChannelId Channel, bool Reliable);
		bool QueueStateFrame(ConnectionId Connection, const CharacterStateFrame &Frame, StateChannelId Channel);
		bool HandleMessage(ConnectionId Connection, const ReceivedMessageEvent &Event, CharacterMessage Message);
		bool SendControl(ConnectionId Connection, const CharacterControlTransition &Transition);
		[[nodiscard]] std::optional<CharacterAuthoritativeState>
		BuildState(ObjectId Character, std::uint64_t AuthoritativeTick, bool Teleport = false);
		void PublishStateFrames(std::uint64_t AuthoritativeTick);
	};

	class PredictedCharacterNetwork final {
	  public:
		PredictedCharacterNetwork(
			INetworkScheduler &Scheduler,
			NetworkLimits Limits,
			CharacterMovementPolicy MovementPolicy,
			CharacterNetworkConfiguration Configuration = {}
		);
		~PredictedCharacterNetwork();

		bool AddPeer(ConnectionId Connection);
		bool RemovePeer(ConnectionId Connection);
		bool MarkMaterialized(ObjectId Character, const std::shared_ptr<KinematicCharacter> &Replica);
		bool MarkUnmaterialized(ObjectId Character);
		bool RegisterAction(CharacterActionDefinition Definition);
		bool HandleTransportEvent(const TransportEvent &Event);
		bool SubmitInput(
			ConnectionId Connection,
			WorldRoot &World,
			std::uint64_t SimulationTick,
			float DeltaSeconds,
			glm::vec2 MoveIntent,
			float FacingYawRadians,
			bool JumpRequested
		);
		bool RequestAction(ConnectionId Connection, std::uint32_t ActionToken, std::uint64_t SimulationTick);
		void Reconcile(WorldRoot &World);
		void UpdatePresentation(std::uint64_t PresentationTick);

		[[nodiscard]] std::optional<CharacterControlTransition> GetControl(ConnectionId Connection) const;
		[[nodiscard]] std::optional<CharacterActionState> GetAuthoritativeAction(ObjectId Character) const;
		[[nodiscard]] std::size_t GetPredictionHistorySize(ConnectionId Connection) const;
		[[nodiscard]] std::size_t GetPresentationSnapshotCount(ObjectId Character) const;
		[[nodiscard]] CharacterNetworkMetrics GetMetrics() const {
			return Metrics;
		}

	  private:
		struct PeerState;
		struct ReplicaState;
		INetworkScheduler &Scheduler;
		NetworkLimits Limits;
		CharacterMovementPolicy MovementPolicy;
		CharacterNetworkConfiguration Configuration;
		std::map<ConnectionId, PeerState> Peers;
		std::map<ObjectId, ReplicaState> Replicas;
		std::map<std::uint32_t, CharacterActionDefinition> Actions;
		CharacterNetworkMetrics Metrics;

		bool Queue(ConnectionId Connection, const CharacterMessage &Message, StateChannelId Channel, bool Reliable);
		bool HandleMessage(ConnectionId Connection, const ReceivedMessageEvent &Event, CharacterMessage Message);
		bool HandleState(PeerState &Peer, const ReceivedMessageEvent &Event, CharacterAuthoritativeState State,
			bool ReliableState, bool FramedState = false);
		bool Predict(PeerState &Peer, WorldRoot &World, const CharacterInputCommand &Command, bool Record);
		void HardReset(PeerState &Peer);
	};
}

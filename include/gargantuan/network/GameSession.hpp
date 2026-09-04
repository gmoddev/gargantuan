#pragma once

#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Outcome.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/network/Transport.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace gargantuan {
	class DataModel;
	class Engine;
	class Player;
}

namespace gargantuan::network {
	namespace detail {
		class GameSessionTestAccess;
	}

	inline constexpr std::uint64_t DefaultGameSessionHandshakeTimeoutTicks = 600;
	inline constexpr std::size_t MaximumGameSessionPeers = 512;

	enum class GameSessionRole : std::uint8_t { Server, Client };
	enum class GameSessionStatus : std::uint8_t {
		Created,
		Starting,
		Listening,
		Connecting,
		Accepted,
		Ready,
		Closing,
		Closed,
		Failed,
	};

	struct GameSessionConfiguration {
		GameSessionRole Role = GameSessionRole::Client;
		TransportEndpoint Endpoint;
		NetworkLimits Limits;
		std::uint64_t HandshakeTimeoutTicks = DefaultGameSessionHandshakeTimeoutTicks;
		std::uint64_t ClientNonce = 0;
		ReplicationRelevanceConfiguration Relevance;
		bool AllowInsecureDevelopmentNetwork = false;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] static NetworkLimits DefaultLimits();
	};

	struct GameSessionMetrics {
		std::uint64_t TransportConnections = 0;
		std::uint64_t AcceptedPeers = 0;
		std::uint64_t ReadyPeers = 0;
		std::uint64_t RejectedHandshakes = 0;
		std::uint64_t RejectedPreAcceptanceMessages = 0;
		std::uint64_t ProtocolRejects = 0;
		std::uint64_t HandshakeTimeouts = 0;
		std::uint64_t PlayersCreated = 0;
		std::uint64_t PlayersRemoved = 0;
		std::uint64_t CharacterControlBindings = 0;
		std::uint64_t CharacterControlRevocations = 0;
		std::uint64_t ActionsPresented = 0;
		std::uint64_t ActionPresentationStops = 0;
		std::uint64_t ActionPresentationDeferrals = 0;
		std::uint64_t SessionAcceptanceCpuNanoseconds = 0;
		std::uint64_t PlayerCreationCpuNanoseconds = 0;
		std::uint64_t ServerGraphSynchronizationCpuNanoseconds = 0;
		std::uint64_t BaselineSnapshotCpuNanoseconds = 0;
		std::uint64_t BaselineDiscoveryCpuNanoseconds = 0;
		std::uint64_t BaselineEncodeCpuNanoseconds = 0;
		std::uint64_t GameplayRegistrationCpuNanoseconds = 0;
		std::uint64_t RelevanceInitializationCpuNanoseconds = 0;
		std::uint64_t RelevantObjects = 0;
		std::uint64_t RelevanceEnters = 0;
		std::uint64_t RelevanceLeaves = 0;
		std::uint64_t RelevanceQueries = 0;
		std::uint64_t RelevanceCandidates = 0;
		std::uint64_t RelevanceCpuNanoseconds = 0;
		std::uint64_t MaterializedObjects = 0;
		std::uint64_t MaterializedCharacters = 0;
		std::uint64_t MaterializationBacklog = 0;
		std::uint64_t MaterializationTransitions = 0;
		std::uint64_t MaterializationCpuNanoseconds = 0;
		std::uint64_t CharacterImportanceEvaluations = 0;
		std::uint64_t CharacterImportanceTierTransitions = 0;
		std::uint64_t CharacterTemporaryPromotions = 0;
		std::uint64_t CharacterForcedSemanticPublications = 0;
		std::uint64_t CharacterFullRateStates = 0;
		std::uint64_t CharacterReducedRateStates = 0;
		std::uint64_t CharacterLowRateStates = 0;
		std::uint64_t CharacterStateBytes = 0;
		std::uint64_t CharacterMaximumStateAgeTicks = 0;
		std::uint64_t CharacterImportanceCpuNanoseconds = 0;
		std::uint64_t CharacterDueSetCpuNanoseconds = 0;
		std::uint64_t CharacterPublicationBudgetConsumed = 0;
		std::uint64_t CharacterPublicationStatesSelected = 0;
		std::uint64_t CharacterPublicationStatesAccepted = 0;
		std::uint64_t CharacterPublicationStatesOffered = 0;
		std::uint64_t CharacterPublicationStatesDeferred = 0;
		std::uint64_t CharacterPublicationOverdueRelationships = 0;
		std::uint64_t CharacterPublicationDeadlineMisses = 0;
		std::uint64_t CharacterPublicationLatencySamples = 0;
		std::uint64_t CharacterPublicationLatencyTicks = 0;
		std::uint64_t CharacterMaximumPublicationLatencyTicks = 0;
		std::uint64_t CharacterMaximumOwnerStateAgeTicks = 0;
		std::uint64_t CharacterPublicationOwnerDeferrals = 0;
		std::uint64_t CharacterPublicationBudgetExhaustions = 0;
		std::uint64_t CharacterPublicationSchedulerRejections = 0;
		std::uint64_t CharacterPublicationSelectionCpuNanoseconds = 0;
		std::uint64_t CharacterPublicationActiveRelationships = 0;
		std::uint64_t CharacterPublicationCurrentDueRelationships = 0;
		std::uint64_t CharacterPublicationCurrentOverdueRelationships = 0;
		std::uint64_t CharacterMaximumCurrentPublicationAgeTicks = 0;
	};

	class GameSession final {
	  public:
		GameSession(
			std::shared_ptr<IGameTransport> Transport,
			GameSessionConfiguration Configuration,
			Engine *ServerRuntime = nullptr
		);
		~GameSession();
		GameSession(const GameSession &) = delete;
		GameSession &operator=(const GameSession &) = delete;

		[[nodiscard]] TransportOperationResult Start();
		void Stop();
		std::size_t Poll();
		void Step(std::uint64_t SimulationTick);
		bool AttachClientRuntime(Engine &Runtime);

		[[nodiscard]] GameSessionRole GetRole() const;
		[[nodiscard]] GameSessionStatus GetStatus() const;
		[[nodiscard]] const std::string &GetFailure() const;
		[[nodiscard]] GameSessionMetrics GetMetrics() const;
		[[nodiscard]] std::shared_ptr<DataModel> GetClientDataModel() const;
		[[nodiscard]] std::optional<ConnectionId> GetPrimaryConnection() const;
		[[nodiscard]] std::shared_ptr<Player> GetAcceptedPlayer(ConnectionId Connection) const;
		bool SetTrustedReplicationFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints);

	  private:
		friend class detail::GameSessionTestAccess;
		struct Implementation;
		std::unique_ptr<Implementation> State;
	};
}

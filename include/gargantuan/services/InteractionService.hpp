#pragma once

#include "gargantuan/classes/Player.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/services/generated/InteractionService.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

namespace gargantuan {
	class ActionMap;
	class Attachment;
	class DataModel;
	class Players;
	class Script;
	struct InteractionServiceTestAccess;

	class InteractionService : public Instance {
		I_InteractionService;

	  public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t MaximumPrompts = 16'384;
		static constexpr std::size_t MaximumPlayers = 64;
		static constexpr std::size_t MaximumPromptsConsideredPerQuery = MaximumPrompts;
		static constexpr float SpatialCellSize = 16.0f;
		static constexpr float MaximumSpatialCoordinate = 10'000'000.0f;
		static constexpr auto EvaluationInterval = std::chrono::milliseconds(33);

		~InteractionService() override;

	  private:
		friend class Engine;
		friend struct InteractionServiceTestAccess;

		struct CellKey {
			std::int64_t X = 0;
			std::int64_t Y = 0;
			std::int64_t Z = 0;
			auto operator<=>(const CellKey &) const = default;
		};

		struct CellKeyHash {
			std::size_t operator()(const CellKey &Value) const noexcept;
		};

		struct PromptRecord {
			std::weak_ptr<ProximityPrompt> Prompt;
			glm::vec3 Position{};
			CellKey Cell{};
			bool Indexed = false;
			std::vector<SignalConnection::Pointer> PromptConnections;
			std::vector<SignalConnection::Pointer> AnchorConnections;
		};

		struct PlayerState {
			ObjectId ActivePrompt;
			ObjectId HoldingPrompt;
			Clock::time_point HoldStarted{};
			bool AwaitingRelease = false;
			float HoldProgress = 0.0f;
		};

		struct QueryResult {
			ObjectId Prompt;
			float DistanceSquared = 0.0f;
			std::size_t Considered = 0;
		};

		std::weak_ptr<DataModel> World;
		std::weak_ptr<Players> PlayerService;
		std::weak_ptr<ActionMap> Actions;
		std::unordered_map<ObjectId, PromptRecord> Prompts;
		std::unordered_map<CellKey, std::vector<ObjectId>, CellKeyHash> SpatialCells;
		std::unordered_set<ObjectId> DirtyPrompts;
		std::unordered_map<ObjectId, PlayerState> PlayerStates;
		std::function<void()> UnbindDescendants;
		SignalConnection::Pointer DescendantRemovedConnection;
		SignalConnection::Pointer ActionBeganConnection;
		SignalConnection::Pointer ActionEndedConnection;
		std::shared_ptr<Script> RuntimeScript;
		Clock::time_point LastEvaluation{};
		ObjectId PresentedPrompt;
		std::string PresentedActionText;
		std::string PresentedObjectText;
		float PresentedHoldDuration = 0.0f;
		float PresentedHoldProgress = 0.0f;
		bool PresentedAvailable = false;
		bool DefaultPresentationEnabledValue = false;
		bool SemanticInputDown = false;
		bool PresentationInputDown = false;
		bool ActivationPressed = false;
		bool ActivationReleased = false;
		bool RejectionLogged = false;
		bool RuntimeAttached = false;

		void AttachRuntime(
			const std::shared_ptr<DataModel> &WorldValue,
			const std::shared_ptr<Players> &PlayersValue,
			const std::shared_ptr<ActionMap> &ActionMapValue,
			bool EnableDefaultPresentation
		);
		void StartDefaultRuntime();
		void ShutdownRuntime();
		void Step(Clock::time_point Now);
		void CancelInput();
		void SetInputSource(bool &Source, bool Down);
		void RegisterPrompt(const std::shared_ptr<ProximityPrompt> &Prompt);
		void UnregisterPrompt(ObjectId PromptId);
		void MarkPromptDirty(ObjectId PromptId);
		void ProcessDirtyPrompts();
		void RefreshPrompt(ObjectId PromptId);
		void RemoveFromCell(PromptRecord &Record, ObjectId PromptId);
		void InvalidatePromptStates(ObjectId PromptId);
		[[nodiscard]] static std::optional<CellKey> PositionToCell(const glm::vec3 &Position);
		[[nodiscard]] static std::optional<glm::vec3> ResolveAnchor(
			const std::shared_ptr<ProximityPrompt> &Prompt, std::vector<std::shared_ptr<Instance>> *Observed = nullptr
		);
		[[nodiscard]] static std::optional<glm::vec3> ResolvePlayerOrigin(const std::shared_ptr<Player> &PlayerValue);
		[[nodiscard]] QueryResult QueryNearest(const glm::vec3 &Origin) const;
		[[nodiscard]] bool IsActivationValid(
			const std::shared_ptr<Player> &PlayerValue,
			ObjectId PromptId,
			std::shared_ptr<ProximityPrompt> *ResolvedPrompt = nullptr
		) const;
		[[nodiscard]] bool UpdatePlayer(
			const std::shared_ptr<Player> &PlayerValue,
			PlayerState &State,
			Clock::time_point Now,
			bool Pressed,
			bool Released,
			bool InputDown
		);
		[[nodiscard]] bool Trigger(
			const std::shared_ptr<Player> &PlayerValue,
			PlayerState &State,
			const std::shared_ptr<ProximityPrompt> &Prompt
		);
		void PublishLocalPresentation();
	};
}

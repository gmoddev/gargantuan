#pragma once

#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/runtime/RuntimeMode.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/services/generated/CharacterControlService.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <lua.h>

namespace gargantuan {
	class KinematicCharacter;
	class Character;
	class Player;
	class WorldRoot;

	class CharacterControlService : public Instance {
		I_CharacterControlService;

	  public:
		static constexpr std::size_t MaximumActions = 256;
		static constexpr std::size_t MaximumActionNameBytes = 64;

		using ClientSubmitHandler = std::function<bool(std::uint64_t, float, glm::vec2, float, bool)>;
		using ClientActionHandler = std::function<bool(std::uint32_t, std::uint64_t)>;
		using ActionRegistrationHandler = std::function<bool(const network::CharacterActionDefinition &, bool)>;
		using PredictionModeHandler = std::function<void(bool)>;

		class RuntimeAttachment final {
			friend class CharacterControlService;
			std::weak_ptr<CharacterControlService> Owner;
			std::uint64_t Generation = 0;

			RuntimeAttachment(std::weak_ptr<CharacterControlService> OwnerValue, std::uint64_t GenerationValue);

		  public:
			RuntimeAttachment() = default;
			~RuntimeAttachment();
			RuntimeAttachment(const RuntimeAttachment &) = delete;
			RuntimeAttachment &operator=(const RuntimeAttachment &) = delete;
			RuntimeAttachment(RuntimeAttachment &&Other) noexcept;
			RuntimeAttachment &operator=(RuntimeAttachment &&Other) noexcept;
			void Reset();
			[[nodiscard]] bool IsValid() const;
		};

		CharacterControlService();
		~CharacterControlService() override;

		void ConfigureRuntime(RuntimeMode Mode);
		[[nodiscard]] std::optional<RuntimeAttachment> AttachRuntime(
			ClientSubmitHandler Submit,
			ClientActionHandler Action,
			ActionRegistrationHandler Register,
			PredictionModeHandler Prediction
		);
		void BeginSimulationFrame(std::uint64_t Tick, float DeltaSeconds);
		void DetachRuntime();

		[[nodiscard]] CharacterMotionRequest
		EvaluateMovement(const network::CharacterInputCommand &Command, const KinematicCharacter &Character);
		[[nodiscard]] bool EvaluateAction(
			const std::shared_ptr<Player> &PlayerValue,
			const std::shared_ptr<KinematicCharacter> &Character,
			std::uint32_t Token
		);
		[[nodiscard]] std::optional<std::string> GetActionName(std::uint32_t Token) const;
		void
		PublishActionResolution(const std::shared_ptr<Character> &CharacterValue, std::uint32_t Token, bool Accepted);
		void PublishActionEnded(const std::shared_ptr<Character> &CharacterValue, std::uint32_t Token);

	  private:
		struct RegisteredAction {
			std::string Name;
			network::CharacterActionDefinition Definition;
			bool Predictable = false;
		};

		RuntimeMode Mode = RuntimeMode::Offline;
		ClientSubmitHandler ClientSubmit;
		ClientActionHandler ClientAction;
		ActionRegistrationHandler ActionRegistration;
		PredictionModeHandler PredictionMode;
		std::uint64_t RuntimeAttachmentGeneration = 0;
		std::uint64_t NextRuntimeAttachmentGeneration = 1;
		std::uint64_t SimulationTick = 0;
		float SimulationDeltaSeconds = 0.0f;
		lua_State *PolicyState = nullptr;
		int MovementPolicyReference = LUA_NOREF;
		int ActionPolicyReference = LUA_NOREF;
		ScriptSecurityContext MovementPolicyContext;
		ScriptSecurityContext ActionPolicyContext;
		std::map<std::string, RegisteredAction> ActionsByName;
		std::map<std::uint32_t, std::string> ActionNamesByToken;

		[[nodiscard]] static std::uint32_t ActionToken(std::string_view Name);
		void DetachRuntime(std::uint64_t Generation);
		void ClearPolicyReferences();
	};
}

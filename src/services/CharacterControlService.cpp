#include "gargantuan/services/CharacterControlService.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	namespace {
		CharacterControlService *CheckedService(Instance *Value) {
			auto *Service = dynamic_cast<CharacterControlService *>(Value);
			if (!Service || Service->GetDestroyed() || Service->IsDestroying())
				throw std::runtime_error("[Character:Control] CharacterControlService is unavailable");
			return Service;
		}

		void RequireDomain(RuntimeMode Mode, std::string_view Operation) {
			const auto Domain = GetCurrentScriptSecurityContext().Domain;
			const bool Allowed = (Mode == RuntimeMode::NetworkClient && Domain == ScriptExecutionDomain::Client) ||
								 (Mode == RuntimeMode::NetworkServer && Domain == ScriptExecutionDomain::Server);
			if (!Allowed)
				throw std::runtime_error(
					"[Character:Control] " + std::string(Operation) + " is unavailable in this runtime role"
				);
		}

		bool Finite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}
	}

	CharacterControlService::CharacterControlService() = default;
	CharacterControlService::~CharacterControlService() {
		ClearPolicyReferences();
	}

	void CharacterControlService::ConfigureRuntime(RuntimeMode ModeValue) {
		if (Mode != RuntimeMode::Offline && Mode != ModeValue)
			throw std::logic_error("[Character:Control] runtime role is immutable once configured");
		Mode = ModeValue;
	}

	void CharacterControlService::AttachClientBridge(ClientSubmitHandler Submit, ClientActionHandler Action) {
		ClientSubmit = std::move(Submit);
		ClientAction = std::move(Action);
	}

	void CharacterControlService::AttachActionRegistration(ActionRegistrationHandler Register) {
		ActionRegistration = std::move(Register);
		if (!ActionRegistration) return;
		for (const auto &[Name, Action] : ActionsByName) {
			(void)Name;
			if (!ActionRegistration(Action.Definition, Action.Predictable))
				throw std::runtime_error(
					"[Character:Control] registered action could not attach to the network session"
				);
		}
	}

	void CharacterControlService::AttachPredictionMode(PredictionModeHandler Handler) {
		PredictionMode = std::move(Handler);
	}

	void CharacterControlService::BeginSimulationFrame(std::uint64_t Tick, float DeltaSeconds) {
		SimulationTick = Tick;
		SimulationDeltaSeconds = std::isfinite(DeltaSeconds)
									 ? std::clamp(DeltaSeconds, 1.0f / 240.0f, network::MaximumCharacterCommandInterval)
									 : 1.0f / 60.0f;
	}

	void CharacterControlService::DetachRuntime() {
		ClientSubmit = {};
		ClientAction = {};
		ActionRegistration = {};
		PredictionMode = {};
		SimulationTick = 0;
		SimulationDeltaSeconds = 0.0f;
		ClearPolicyReferences();
	}

	std::uint32_t CharacterControlService::ActionToken(std::string_view Name) {
		std::uint32_t Hash = 2166136261u;
		for (const auto Byte : Name) {
			Hash ^= static_cast<std::uint8_t>(Byte);
			Hash *= 16777619u;
		}
		return Hash == 0 ? 1 : Hash;
	}

	CharacterMotionRequest CharacterControlService::EvaluateMovement(
		const network::CharacterInputCommand &Command, const KinematicCharacter &CharacterValue
	) {
		if (!PolicyState || MovementPolicyReference == LUA_NOREF)
			throw std::runtime_error("[Character:Control] no movement policy is installed");
		auto CharacterPointer = std::dynamic_pointer_cast<KinematicCharacter>(
			const_cast<KinematicCharacter &>(CharacterValue).shared_from_this()
		);
		if (!CharacterPointer) throw std::runtime_error("[Character:Control] movement target is unavailable");
		auto *L = PolicyState;
		const auto StackBase = lua_gettop(L);
		ScriptSecurityScope SecurityScope(MovementPolicyContext);
		lua_getref(L, MovementPolicyReference);
		StackValue<std::shared_ptr<KinematicCharacter>>::Push(L, CharacterPointer);
		lua_pushnumber(L, Command.DeltaSeconds);
		StackValue<Vector2>::Push(L, Vector2(Command.MoveIntent));
		lua_pushnumber(L, Command.FacingYawRadians);
		lua_pushboolean(L, Command.JumpRequested());
		const auto Status = lua_pcall(L, 5, 3, 0);
		if (Status != LUA_OK) {
			const auto *Message = lua_tostring(L, -1);
			LOG_ERROR(Lua, "[Character:Control] movement policy failed: %s", Message ? Message : "unknown error");
			lua_settop(L, StackBase);
			throw std::runtime_error("[Character:Control] movement policy failed");
		}
		const auto Translation = CheckStackValue<glm::vec3>(L, -3);
		const auto Velocity = CheckStackValue<glm::vec3>(L, -2);
		const auto Yaw = CheckStackValue<float>(L, -1);
		lua_settop(L, StackBase);
		if (!Finite(Translation) || !Finite(Velocity) || !std::isfinite(Yaw))
			throw std::runtime_error("[Character:Control] movement policy returned non-finite motion");
		return {
			.Translation = Translation,
			.Velocity = Velocity,
			.YawRadians = Yaw,
			.Source = CharacterMotionSource::Script,
		};
	}

	bool CharacterControlService::EvaluateAction(
		const std::shared_ptr<Player> &PlayerValue,
		const std::shared_ptr<KinematicCharacter> &CharacterValue,
		std::uint32_t Token
	) {
		const auto Name = GetActionName(Token);
		if (!Name || !PlayerValue || !CharacterValue) return false;
		if (!PolicyState || ActionPolicyReference == LUA_NOREF) return true;
		auto *L = PolicyState;
		const auto StackBase = lua_gettop(L);
		ScriptSecurityScope SecurityScope(ActionPolicyContext);
		lua_getref(L, ActionPolicyReference);
		StackValue<std::shared_ptr<Player>>::Push(L, PlayerValue);
		StackValue<std::shared_ptr<KinematicCharacter>>::Push(L, CharacterValue);
		StackValue<std::string>::Push(L, *Name);
		const auto Status = lua_pcall(L, 3, 1, 0);
		if (Status != LUA_OK || !lua_isboolean(L, -1)) {
			const auto *Message = lua_tostring(L, -1);
			LOG_ERROR(Lua, "[Character:Control] action policy failed: %s", Message ? Message : "invalid result");
			lua_settop(L, StackBase);
			return false;
		}
		const bool Accepted = lua_toboolean(L, -1);
		lua_settop(L, StackBase);
		return Accepted;
	}

	std::optional<std::string> CharacterControlService::GetActionName(std::uint32_t Token) const {
		auto Found = ActionNamesByToken.find(Token);
		return Found == ActionNamesByToken.end() ? std::nullopt : std::optional(Found->second);
	}

	void CharacterControlService::PublishActionResolution(
		const std::shared_ptr<Character> &CharacterValue, std::uint32_t Token, bool Accepted
	) {
		if (auto Name = GetActionName(Token); Name && CharacterValue)
			ActionResolved->Fire({CharacterValue, *Name, Accepted});
	}

	void
	CharacterControlService::PublishActionEnded(const std::shared_ptr<Character> &CharacterValue, std::uint32_t Token) {
		if (auto Name = GetActionName(Token); Name && CharacterValue && !CharacterValue->GetDestroyed())
			ActionEnded->Fire({CharacterValue, *Name});
	}

	int CharacterControlService::SubmitMoveIntent(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "SubmitMoveIntent");
		const auto Intent = CheckStackValue<Vector2>(L, 2).Value;
		const auto Facing = CheckStackValue<float>(L, 3);
		const auto Jump = CheckStackValue<bool>(L, 4);
		const bool Valid = std::isfinite(Intent.x) && std::isfinite(Intent.y) &&
						   glm::length(Intent) <= network::MaximumCharacterMoveIntentMagnitude &&
						   std::isfinite(Facing) && std::abs(Facing) <= 3.14159265358979323846f &&
						   Service->SimulationTick != 0 && Service->SimulationDeltaSeconds > 0 && Service->ClientSubmit;
		lua_pushboolean(
			L,
			Valid &&
				Service->ClientSubmit(Service->SimulationTick, Service->SimulationDeltaSeconds, Intent, Facing, Jump)
		);
		return 1;
	}

	int CharacterControlService::RequestAction(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "RequestAction");
		const auto Name = std::string(CheckStackValue<std::string_view>(L, 2));
		auto Found = Service->ActionsByName.find(Name);
		lua_pushboolean(
			L,
			Found != Service->ActionsByName.end() && Service->ClientAction && Service->SimulationTick != 0 &&
				Service->ClientAction(Found->second.Definition.Token, Service->SimulationTick)
		);
		return 1;
	}

	int CharacterControlService::SetMovementPolicy(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "SetMovementPolicy");
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto *Main = lua_mainthread(L);
		if (Service->PolicyState && Service->PolicyState != Main)
			throw std::runtime_error("[Character:Control] policies must use one Luau VM");
		Service->PolicyState = Main;
		if (Service->MovementPolicyReference != LUA_NOREF) lua_unref(Main, Service->MovementPolicyReference);
		Service->MovementPolicyReference = lua_ref(L, 2);
		Service->MovementPolicyContext = GetCurrentScriptSecurityContext();
		return 0;
	}

	int CharacterControlService::SetActionPolicy(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "SetActionPolicy");
		if (Service->Mode != RuntimeMode::NetworkServer)
			throw std::runtime_error("[Character:Control] action admission policy is server-only");
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto *Main = lua_mainthread(L);
		if (Service->PolicyState && Service->PolicyState != Main)
			throw std::runtime_error("[Character:Control] policies must use one Luau VM");
		Service->PolicyState = Main;
		if (Service->ActionPolicyReference != LUA_NOREF) lua_unref(Main, Service->ActionPolicyReference);
		Service->ActionPolicyReference = lua_ref(L, 2);
		Service->ActionPolicyContext = GetCurrentScriptSecurityContext();
		return 0;
	}

	int CharacterControlService::RegisterAction(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "RegisterAction");
		const auto Name = std::string(CheckStackValue<std::string_view>(L, 2));
		const auto Reference = std::string(CheckStackValue<std::string_view>(L, 3));
		const auto DurationSeconds = CheckStackValue<float>(L, 4);
		const auto RootTranslation = lua_isnoneornil(L, 5) ? glm::vec3(0.0f) : CheckStackValue<glm::vec3>(L, 5);
		const auto RootYaw = lua_isnoneornil(L, 6) ? 0.0f : CheckStackValue<float>(L, 6);
		const bool Predictable = lua_isnoneornil(L, 7) ? false : CheckStackValue<bool>(L, 7);
		ValidateProtocolString(Name, MaximumActionNameBytes, "Character action name");
		if (Name.empty() || !std::isfinite(DurationSeconds) || DurationSeconds <= 0.0f || DurationSeconds > 600.0f ||
			!Finite(RootTranslation) || !std::isfinite(RootYaw)) {
			lua_pushboolean(L, false);
			return 1;
		}
		auto DataModelValue = Service->GetDataModel();
		auto Assets = DataModelValue
						  ? std::dynamic_pointer_cast<AssetService>(DataModelValue->GetService("AssetService"))
						  : nullptr;
		auto Record = Assets ? Assets->GetAsset(Reference) : std::nullopt;
		if (!Record || Record->Kind != AssetKind::Animation || Record->State != AssetState::Ready ||
			!Record->Id.IsValid() || !Record->ContentId.IsValid()) {
			lua_pushboolean(L, false);
			return 1;
		}
		const auto DurationTicks = static_cast<std::uint32_t>(std::clamp(
			std::llround(static_cast<double>(DurationSeconds) * network::DefaultCharacterSimulationTicksPerSecond),
			1ll,
			static_cast<long long>(network::DefaultCharacterSimulationTicksPerSecond * 60 * 10)
		));
		const auto Token = ActionToken(Name);
		if (auto Collision = Service->ActionNamesByToken.find(Token);
			Collision != Service->ActionNamesByToken.end() && Collision->second != Name) {
			lua_pushboolean(L, false);
			return 1;
		}
		if (auto Existing = Service->ActionsByName.find(Name); Existing != Service->ActionsByName.end()) {
			lua_pushboolean(L, false);
			return 1;
		}
		if (Service->ActionsByName.size() >= MaximumActions) {
			lua_pushboolean(L, false);
			return 1;
		}
		network::CharacterActionDefinition Definition{
			.Token = Token,
			.Animation = Record->Id,
			.ContentRevision = Record->ContentId,
			.DurationTicks = DurationTicks,
			.EvaluateRootMotion = [RootTranslation, RootYaw, DurationTicks](
									  std::uint64_t FromTick, std::uint64_t ToTick
								  ) -> std::optional<RootMotionDelta> {
				if (ToTick <= FromTick) return std::nullopt;
				const auto Fraction = static_cast<float>(ToTick - FromTick) / static_cast<float>(DurationTicks);
				const auto Translation = RootTranslation * Fraction;
				const auto Yaw = RootYaw * Fraction;
				if (glm::length(Translation) > Character::MaximumMotionTranslation ||
					std::abs(Yaw) > Character::MaximumMotionYawRadians)
					return std::nullopt;
				return RootMotionDelta{.Translation = Translation, .YawRadians = Yaw};
			},
		};
		if (!Definition.IsValid() ||
			(Service->ActionRegistration && !Service->ActionRegistration(Definition, Predictable))) {
			lua_pushboolean(L, false);
			return 1;
		}
		Service->ActionNamesByToken.emplace(Token, Name);
		Service->ActionsByName.emplace(Name, RegisteredAction{Name, std::move(Definition), Predictable});
		lua_pushboolean(L, true);
		return 1;
	}

	int CharacterControlService::SetPredictionEnabled(lua_State *L, Instance *InstanceValue) {
		auto *Service = CheckedService(InstanceValue);
		RequireDomain(Service->Mode, "SetPredictionEnabled");
		if (Service->Mode != RuntimeMode::NetworkClient)
			throw std::runtime_error("[Character:Control] prediction mode is client-only");
		const auto Enabled = CheckStackValue<bool>(L, 2);
		if (Service->PredictionMode) Service->PredictionMode(Enabled);
		return 0;
	}

	void CharacterControlService::ClearPolicyReferences() {
		if (PolicyState) {
			if (MovementPolicyReference != LUA_NOREF) lua_unref(PolicyState, MovementPolicyReference);
			if (ActionPolicyReference != LUA_NOREF) lua_unref(PolicyState, ActionPolicyReference);
		}
		MovementPolicyReference = LUA_NOREF;
		ActionPolicyReference = LUA_NOREF;
		PolicyState = nullptr;
	}
}

#include "gargantuan/classes/Player.hpp"

#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <stdexcept>

namespace gargantuan {
	namespace {
		void RequireCharacterMutationCapability() {
			if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("Player character lifecycle requires MutateDataModel");
		}
	}

	Player::Player() {
		Destroying->Connect([this](std::monostate) { ShutdownCharacter(); });
	}

	Player::~Player() {
		ShutdownCharacter();
	}

	void Player::InitializeIdentity(int Value) {
		if (Value <= 0 || PlayerId != 0) throw std::logic_error("Player identity must be initialized exactly once");
		PlayerId = Value;
		NotifyPropertyCommitted("PlayerId");
	}

	void Player::InitializeAuthenticationIdentity(PlayerIdentity Identity) {
		ValidatePlayerIdentity(Identity);
		if (AuthenticationIdentity)
			throw std::logic_error("Player authentication identity is immutable once initialized");
		AuthenticationIdentity = std::move(Identity);
	}

	std::optional<std::shared_ptr<KinematicCharacter>> Player::GetCharacter() const {
		return CharacterValue ? std::optional(CharacterValue) : std::nullopt;
	}

	void Player::SetCharacter(std::optional<std::shared_ptr<KinematicCharacter>> Value) {
		RequireCharacterMutationCapability();
		AssertCanMutate();
		ValidatePropertyMutation("Character", Value);
		auto Replacement = Value ? *Value : nullptr;
		if (Replacement == CharacterValue) return;
		if (Replacement) {
			if (Replacement->GetDestroyed() || Replacement->IsDestroying())
				throw std::invalid_argument("Player character must be live");
			if (!GetDataModel() || Replacement->GetDataModel() != GetDataModel())
				throw std::invalid_argument("Player character must belong to the same DataModel");
		}

		auto Previous = CharacterValue;
		if (CharacterDestroyingConnection) CharacterDestroyingConnection->Disconnect();
		CharacterDestroyingConnection.reset();
		if (Previous) CharacterRemoving->Fire(Previous);
		CharacterValue = Replacement;
		NotifyPropertyCommitted("Character");

		if (Previous && !Previous->GetDestroyed() && !Previous->IsDestroying()) Previous->Destroy();
		if (!Replacement) return;

		auto WeakSelf = std::weak_ptr<Player>(std::dynamic_pointer_cast<Player>(shared_from_this()));
		CharacterDestroyingConnection = Replacement->Destroying->Once(
			[WeakSelf, WeakCharacter = std::weak_ptr<KinematicCharacter>(Replacement)](std::monostate) {
				auto Self = WeakSelf.lock();
				auto Character = WeakCharacter.lock();
				if (!Self || !Character || Self->CharacterValue != Character || Self->ShuttingDownCharacter) return;
				Self->CharacterRemoving->Fire(Character);
				Self->CharacterValue.reset();
				Self->CharacterDestroyingConnection.reset();
				Self->NotifyPropertyCommitted("Character");
			}
		);
		CharacterAdded->Fire(Replacement);
	}

	void Player::LoadCharacter() {
		RequireCharacterMutationCapability();
		AssertCanMutate();
		RemoveCharacter();
		CharacterSpawnRequested->Fire({});
	}

	void Player::ResetCharacter() {
		LoadCharacter();
	}

	void Player::RemoveCharacter() {
		RequireCharacterMutationCapability();
		if (!CharacterValue) return;
		SetCharacter(std::nullopt);
	}

	void Player::ShutdownCharacter() {
		if (ShuttingDownCharacter) return;
		ShuttingDownCharacter = true;
		if (CharacterDestroyingConnection) CharacterDestroyingConnection->Disconnect();
		CharacterDestroyingConnection.reset();
		auto Previous = std::move(CharacterValue);
		if (Previous) {
			CharacterRemoving->Fire(Previous);
			if (!Previous->GetDestroyed() && !Previous->IsDestroying()) Previous->Destroy();
		}
	}
}

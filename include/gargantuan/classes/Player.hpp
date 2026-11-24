#pragma once

#include "gargantuan/classes/generated/Player.hpp"
#include "gargantuan/identity/PlayerIdentity.hpp"

#include <optional>

namespace gargantuan {
	class Players;
	class Character;
	namespace runtime_detail { class ReplicaCharacterProjection; }

	class Player : public Instance {
		I_Player;

		friend class Players;
		friend class runtime_detail::ReplicaCharacterProjection;
		enum class CharacterWriteSource { Gameplay, Replica };
		struct CharacterRetirement {
			ObjectId Target;
			const CharacterRetirement *Previous;
		};

		int PlayerIdValue = 0;
		std::shared_ptr<Character> CharacterValue;
		ObjectId PendingCharacter;
		const CharacterRetirement *RetiringCharacters = nullptr;
		std::optional<PlayerIdentity> AuthenticationIdentity;
		ObjectId PossessedEntityId;
		std::weak_ptr<Instance> PossessedEntityValue;
		SignalConnection::Pointer PossessionDestroyingConnection;
		SignalConnection::Pointer PossessionAncestryConnection;
		std::uint64_t PossessionRevision = 0;
		SignalConnection::Pointer CharacterDestroyingConnection;
		bool PossessionClosed = false;
		bool PublishingPossessionChange = false;
		bool NotifyingCharacterRemoval = false;
		bool ShuttingDownCharacter = false;
		bool GuiProvisionedHere = false;
		bool HadCharacterForGui = false;
		bool ResettingGui = false;

		void InitializeIdentity(int Value);
		void ProvisionGui(const std::shared_ptr<DataModel> &World);
		void ResetGuiOnCharacter(const std::shared_ptr<Character> &Replacement);
		void CommitPossession(const std::shared_ptr<Instance> &Entity);
		void ClosePossession();
		void DisconnectPossessionSignals();
		bool IsRetiringCharacter(ObjectId Target) const;
		void SetCharacterRelationship(std::optional<std::shared_ptr<Character>> Value, CharacterWriteSource Source);
		void NotifyCharacterRemoving(const std::shared_ptr<Character> &Previous, ObjectId Replacement = {});
		void ShutdownCharacter();

	  public:
		Player();
		~Player() override;
		[[nodiscard]] std::uint64_t GetPossessionRevision() const { return PossessionRevision; }
		void InitializeAuthenticationIdentity(PlayerIdentity Identity);
		[[nodiscard]] const std::optional<PlayerIdentity> &GetAuthenticationIdentity() const {
			return AuthenticationIdentity;
		}
	};
}

#pragma once

#include "gargantuan/classes/generated/Player.hpp"
#include "gargantuan/identity/PlayerIdentity.hpp"

#include <optional>

namespace gargantuan {
	class Players;
	class Character;

	class Player : public Instance {
		I_Player;

		friend class Players;
		int PlayerIdValue = 0;
		std::shared_ptr<Character> CharacterValue;
		std::optional<PlayerIdentity> AuthenticationIdentity;
		ObjectId PossessedEntityId;
		std::weak_ptr<Instance> PossessedEntityValue;
		SignalConnection::Pointer PossessionDestroyingConnection;
		SignalConnection::Pointer PossessionAncestryConnection;
		std::uint64_t PossessionRevision = 0;
		SignalConnection::Pointer CharacterDestroyingConnection;
		bool PossessionClosed = false;
		bool PublishingPossessionChange = false;
		bool ShuttingDownCharacter = false;

		void InitializeIdentity(int Value);
		void CommitPossession(const std::shared_ptr<Instance> &Entity);
		void ClosePossession();
		void DisconnectPossessionSignals();
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

#pragma once

#include "gargantuan/classes/generated/Player.hpp"
#include "gargantuan/identity/PlayerIdentity.hpp"

#include <optional>

namespace gargantuan {
	class Players;

	class Player : public Instance {
		I_Player;

		friend class Players;
		std::shared_ptr<KinematicCharacter> CharacterValue;
		std::optional<PlayerIdentity> AuthenticationIdentity;
		SignalConnection::Pointer CharacterDestroyingConnection;
		bool ShuttingDownCharacter = false;

		void InitializeIdentity(int Value);
		void ShutdownCharacter();

	  public:
		Player();
		~Player() override;
		void InitializeAuthenticationIdentity(PlayerIdentity Identity);
		[[nodiscard]] const std::optional<PlayerIdentity> &GetAuthenticationIdentity() const {
			return AuthenticationIdentity;
		}
	};
}

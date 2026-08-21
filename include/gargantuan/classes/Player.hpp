#pragma once

#include "gargantuan/classes/generated/Player.hpp"

#include <optional>

namespace gargantuan {
	class Players;

	class Player : public Instance {
		I_Player;

		friend class Players;
		std::shared_ptr<KinematicCharacter> CharacterValue;
		SignalConnection::Pointer CharacterDestroyingConnection;
		bool ShuttingDownCharacter = false;

		void InitializeIdentity(int Value);
		void ShutdownCharacter();

	  public:
		Player();
		~Player() override;
	};
}

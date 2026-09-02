#pragma once

#include "gargantuan/identity/PlayerIdentity.hpp"
#include "gargantuan/runtime/RuntimeMode.hpp"
#include "gargantuan/services/generated/Players.hpp"

#include <optional>

namespace gargantuan {
	class Engine;
	class Folder;
	class Script;

	class Players : public Instance {
		I_Players;

		friend class Engine;
		std::shared_ptr<Player> LocalPlayerValue;
		std::shared_ptr<Folder> RuntimeModules;
		bool RuntimeStarted = false;
		int NextSessionPlayerId = 1;

		void InitializeLocalPlayer();
		std::shared_ptr<Script> StartDefaultRuntime(RuntimeMode Mode);
		void ShutdownRuntime();

	  public:
		~Players() override;
		[[nodiscard]] std::shared_ptr<Player> CreateSessionPlayer(PlayerIdentity Identity);
		bool RemoveSessionPlayer(const std::shared_ptr<Player> &Value);
		bool SetTrustedLocalPlayer(const std::shared_ptr<Player> &Value);
		void ClearTrustedLocalPlayer();
		[[nodiscard]] bool IsRuntimeModule(ObjectId Object) const;
	};
}

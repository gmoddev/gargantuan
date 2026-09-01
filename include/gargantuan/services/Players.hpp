#pragma once

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

		void InitializeLocalPlayer();
		std::shared_ptr<Script> StartDefaultRuntime();
		void ShutdownRuntime();

	  public:
		~Players() override;
	};
}

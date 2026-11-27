#pragma once

#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/services/generated/Workspace.hpp"

namespace gargantuan {
	class ServiceProvider;
	class Terrain;
	namespace terrain { class Field; }

	class Workspace : public WorldRoot {
		I_Workspace;

		Workspace();
		~Workspace() override;
		void RestoreTerrainField(std::unique_ptr<terrain::Field> Restored);

	  private:
		friend class ServiceProvider;

		void InitializeTerrain();
		std::weak_ptr<Terrain> TerrainChild;
		bool TerrainRestored = false;
	};
}

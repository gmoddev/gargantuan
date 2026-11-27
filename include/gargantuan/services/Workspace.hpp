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


	  private:
		friend class ServiceProvider;

		void InitializeTerrain();
		std::weak_ptr<SpatialRegion> ViewRegion;
		SignalConnection::Pointer ViewRegionAncestry;

	};
}

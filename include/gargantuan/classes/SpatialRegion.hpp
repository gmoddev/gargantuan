#pragma once

#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/classes/generated/SpatialRegion.hpp"

namespace gargantuan {
	class SpatialRegion : public WorldRoot {
		I_SpatialRegion;

		SpatialRegion();
		~SpatialRegion() override;

	  private:
		void RefreshWorld();
		std::weak_ptr<DataModel> RegisteredWorld;
		SignalConnection::Pointer RegionAncestry;
	};
}

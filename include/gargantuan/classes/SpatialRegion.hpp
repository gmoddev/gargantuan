#pragma once

#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/classes/generated/SpatialRegion.hpp"

namespace gargantuan {
	class SpatialRegion : public WorldRoot {
		I_SpatialRegion;

		SpatialRegion();
		~SpatialRegion() override;
		[[nodiscard]] static std::shared_ptr<DataModel> FindAttachedWorld(const Instance &Object);

	  private:
		void RefreshWorld();
		std::weak_ptr<DataModel> RegisteredWorld;
		SignalConnection::Pointer RegionAncestry;
	};
}

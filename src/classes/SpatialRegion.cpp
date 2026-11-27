#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/DataModel.hpp"

namespace gargantuan {
	SpatialRegion::SpatialRegion() {
		RegionAncestry = AncestryChanged->Connect([this](auto) { RefreshWorld(); });
	}

	SpatialRegion::~SpatialRegion() {
		if (RegionAncestry) RegionAncestry->Disconnect();
		if (auto World = RegisteredWorld.lock()) World->SpatialRegions.erase(GetObjectId());
	}

	void SpatialRegion::RefreshWorld() {
		const auto Previous = RegisteredWorld.lock();
		const auto Current = GetDestroyed() || IsDestroying() ? nullptr : GetDataModel();
		if (Previous == Current) return;
		if (Previous) Previous->SpatialRegions.erase(GetObjectId());
		if (Current)
			Current->SpatialRegions.insert_or_assign(
				GetObjectId(), std::static_pointer_cast<SpatialRegion>(shared_from_this())
			);
		RegisteredWorld = Current;
	}
}

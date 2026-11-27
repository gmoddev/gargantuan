#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"

namespace gargantuan {
	SpatialRegion::SpatialRegion() {
		RegionAncestry = AncestryChanged->Connect([this](auto) { RefreshWorld(); });
	}

	SpatialRegion::~SpatialRegion() {
		if (RegionAncestry) RegionAncestry->Disconnect();
		if (auto World = RegisteredWorld.lock()) World->SpatialRegions.erase(GetObjectId());
	}

	std::shared_ptr<DataModel> SpatialRegion::FindAttachedWorld(const Instance &Object) {
		if (Object.GetDestroyed() || Object.IsDestroying()) return {};
		for (auto Parent = Object.ParentReference.lock(); Parent; Parent = Parent->ParentReference.lock()) {
			if (Parent->GetDestroyed() || Parent->IsDestroying() || dynamic_cast<ViewportFrame *>(Parent.get())) return {};
			if (auto Realm = std::dynamic_pointer_cast<DataModel>(Parent)) return Realm;
		}
		return {};
	}

	std::shared_ptr<Terrain> SpatialRegion::GetTerrain() const {
		if (!GetOwnedTerrain() && !GetDestroyed() && !IsDestroying())
			const_cast<SpatialRegion *>(this)->InitializeTerrain();
		return GetOwnedTerrain();
	}

	void SpatialRegion::RefreshWorld() {
		const auto Previous = RegisteredWorld.lock();
		const auto Current = FindAttachedWorld(*this);
		if (Previous == Current) return;
		if (Previous) Previous->SpatialRegions.erase(GetObjectId());
		if (Current) {
			InitializeTerrain();
			Current->SpatialRegions.insert_or_assign(
				GetObjectId(), std::static_pointer_cast<SpatialRegion>(shared_from_this())
			);
		}
		RegisteredWorld = Current;
	}
}

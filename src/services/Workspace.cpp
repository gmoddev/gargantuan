#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/terrain/Field.hpp"

#include <memory>
#include <stdexcept>

namespace gargantuan {
	Workspace::Workspace() {
		CurrentCamera = std::make_shared<Camera>();
	}

	Workspace::~Workspace() {
		if (ViewRegionAncestry) ViewRegionAncestry->Disconnect();

	}

	std::optional<std::shared_ptr<SpatialRegion>> Workspace::GetCurrentSpatialRegion() const {
		const auto Region = ViewRegion.lock();
		if (!Region || !SpatialRegion::FindAttachedWorld(*Region) || SpatialRegion::FindAttachedWorld(*Region) != GetDataModel())
			return std::nullopt;
		return Region;
	}

	void Workspace::SetCurrentSpatialRegion(std::optional<std::shared_ptr<SpatialRegion>> Value) {
		AssertCanMutate();
		ValidatePropertyMutation("CurrentSpatialRegion", Value);
		const auto Region = Value.value_or(nullptr);
		if (Region && (!GetDataModel() || SpatialRegion::FindAttachedWorld(*Region) != GetDataModel()))
			throw std::invalid_argument("[Spatial:View] The view must belong to this live DataModel");
		if (Region == ViewRegion.lock()) return;
		SignalConnection::Pointer Next;
		if (Region) Next = Region->AncestryChanged->Connect([Weak = weak_from_this()](auto) {
			const auto Owner = std::static_pointer_cast<Workspace>(Weak.lock());
			if (!Owner || Owner->GetDestroyed() || Owner->IsDestroying() || Owner->GetCurrentSpatialRegion()) return;
			Owner->ViewRegion.reset();
			if (auto Connection = std::move(Owner->ViewRegionAncestry)) Connection->Disconnect();
			Owner->NotifyPropertyCommitted("CurrentSpatialRegion");
		});
		if (ViewRegionAncestry) ViewRegionAncestry->Disconnect();
		ViewRegionAncestry = std::move(Next);
		ViewRegion = Region;
		NotifyPropertyCommitted("CurrentSpatialRegion");
	}

	std::shared_ptr<Terrain> Workspace::GetTerrain() const {
		return GetOwnedTerrain();
	}

	void Workspace::InitializeTerrain() {
		WorldRoot::InitializeTerrain();
	}
}

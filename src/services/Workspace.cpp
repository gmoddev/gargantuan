#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/classes/Camera.hpp"
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
		ShutdownPhysics();
		if (auto Child = TerrainChild.lock()) Child->RetireField();
	}

	std::shared_ptr<Terrain> Workspace::GetTerrain() const {
		auto Child = TerrainChild.lock();
		if (!Child || Child->GetDestroyed() || Child->IsDestroying()) return {};
		const auto Parent = Child->GetParent();
		return Parent && Parent->get() == this ? std::move(Child) : nullptr;
	}

	void Workspace::InitializeTerrain() {
		if (GetTerrain()) return;
		auto Child = std::shared_ptr<Terrain>(new Terrain());
		TerrainChild = Child;
		Child->SetParent(shared_from_this());
	}

	void Workspace::RestoreTerrainField(std::unique_ptr<terrain::Field> Restored) {
		AssertAuthoritativeMutation("RestoreTerrainField");
		const auto Child = GetTerrain();
		if (!Restored || !Child || !Child->Field || TerrainRestored || HasTerrainRuntime() ||
			Child->Field->GetRevision() != 0 || GetDestroyed() || IsDestroying())
			throw std::runtime_error("[Terrain:Persistence] The scene cannot admit a terrain field");
		Child->Field = std::move(Restored);
		TerrainRestored = true;
	}
}

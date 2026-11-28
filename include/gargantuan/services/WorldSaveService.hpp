#pragma once
#include "gargantuan/services/generated/WorldSaveService.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
namespace gargantuan {
	class WorldSaveService : public Instance {
		I_WorldSaveService;
	  public:
		void BindSlot(const WorldSlot &Slot) { Selected = Slot; }
	  private:
		std::optional<WorldSlot> Selected;
	};
}

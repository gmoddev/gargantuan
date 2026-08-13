#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/classes/DataModel.hpp"

namespace gargantuan {
	std::shared_ptr<DataModel> PrepareDataModelRoot(const std::shared_ptr<Instance> &root) {
		if (auto game = std::dynamic_pointer_cast<DataModel>(root)) return game;
		auto game = std::make_shared<DataModel>();
		root->SetParent(game->GetService("Workspace"));
		return game;
	}
}

#pragma once

#include <memory>

namespace gargantuan {
	class DataModel;
	class Instance;
	std::shared_ptr<DataModel> PrepareDataModelRoot(const std::shared_ptr<Instance> &root);
}

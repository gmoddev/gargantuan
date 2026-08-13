#pragma once

#include <memory>

namespace gargantuan {
	class Instance;
	class ModuleScript;
	std::shared_ptr<ModuleScript> ResolveRequiredModule(const std::shared_ptr<Instance> &instance);
}

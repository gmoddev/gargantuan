#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/ModuleScript.hpp"

#include <stdexcept>

namespace gargantuan {
	std::shared_ptr<ModuleScript> ResolveRequiredModule(const std::shared_ptr<Instance> &instance) {
		auto module = std::dynamic_pointer_cast<ModuleScript>(instance);
		if (!module) throw std::invalid_argument("Required Instance is not a ModuleScript");
		return module;
	}
}

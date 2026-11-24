#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"

#include <stdexcept>

namespace gargantuan {
	std::shared_ptr<ModuleScript> ResolveRequiredModule(const std::shared_ptr<Instance> &instance) {
		auto module = std::dynamic_pointer_cast<ModuleScript>(instance);
		if (!module) throw std::invalid_argument("Required Instance is not a ModuleScript");
		const auto Gui = FindPlayerGuiScope(*module);
		if (IsStarterGuiTemplate(*module) || (Gui &&
			(GetCurrentScriptSecurityContext().Domain != ScriptExecutionDomain::Client ||
				!IsLocalPlayerGui(*Gui) || !module->HasSource())))
			throw std::invalid_argument("Required GUI ModuleScript is outside the caller's execution scope");
		return module;
	}
}

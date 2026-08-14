#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/classes/Instance.hpp"

#include <typeindex>
#include <vector>

namespace gargantuan::InstanceClassRegistry {
	const InstanceClassDefinition *GetDefinitionByType(std::type_index type) {
		return GetActiveRuntimeSchemaRegistry().FindByType(type);
	}

	const InstanceClassDefinition *GetDefinition(Instance *instance) {
		if (!instance) return nullptr;
		return GetDefinitionByType(std::type_index(typeid(*instance)));
	}

	const InstanceClassDefinition *GetDefinitionByName(std::string_view name) {
		return GetActiveRuntimeSchemaRegistry().FindByName(name);
	}

	const InstanceClassDefinition *GetDefinitionBySchemaId(SchemaId id) {
		return GetActiveRuntimeSchemaRegistry().FindById(id);
	}

	std::vector<std::string> GetClassNames() {
		std::vector<std::string> result;
		for (const auto *definition : GetActiveRuntimeSchemaRegistry().Enumerate())
			result.push_back(definition->ClassName);
		return result;
	}
}

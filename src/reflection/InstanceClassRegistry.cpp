#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/classes/Instance.hpp"

#include <typeindex>
#include <vector>

namespace gargantuan::InstanceClassRegistry {
	void InvalidateCaches() { GetRuntimeSchemaRegistry().InvalidateCaches(); }

	InstanceClassDefinition *GetDefinitionByType(std::type_index type) {
		return GetRuntimeSchemaRegistry().FindByType(type);
	}

	InstanceClassDefinition *GetDefinition(Instance *instance) {
		if (!instance) return nullptr;
		auto *definition = GetDefinitionByType(std::type_index(typeid(*instance)));
		if (definition) instance->CachedDefinition = definition;
		return definition;
	}

	InstanceClassDefinition *GetDefinitionByName(std::string_view name) {
		return GetRuntimeSchemaRegistry().FindByName(name);
	}

	InstanceClassDefinition *GetDefinitionBySchemaId(SchemaId id) {
		return GetRuntimeSchemaRegistry().FindById(id);
	}

	std::vector<std::string> GetClassNames() {
		std::vector<std::string> result;
		for (const auto *definition : GetRuntimeSchemaRegistry().Enumerate())
			result.push_back(definition->ClassName);
		return result;
	}
}

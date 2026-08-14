#pragma once

#include "gargantuan/InstanceClassDefinition.hpp"

#include <typeindex>
#include <vector>

namespace gargantuan::InstanceClassRegistry {
	void InvalidateCaches();

	template <typename T> void Register(InstanceClassDefinition definition) {
		GetRuntimeSchemaRegistry().RegisterNative<T>(std::move(definition));
	}

	InstanceClassDefinition *GetDefinitionByType(std::type_index type);

	template <typename T> InstanceClassDefinition *GetDefinition() {
		return GetDefinitionByType(std::type_index(typeid(T)));
	}

	InstanceClassDefinition *GetDefinition(Instance *instance);
	InstanceClassDefinition *GetDefinitionByName(std::string_view name);
	InstanceClassDefinition *GetDefinitionBySchemaId(SchemaId id);
	std::vector<std::string> GetClassNames();
}

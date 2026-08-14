#pragma once

#include "gargantuan/InstanceClassDefinition.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <typeindex>
#include <vector>

namespace gargantuan::InstanceClassRegistry {
	template <typename T> void Register(InstanceClassDefinition definition) {
		RegisterNativeSchemaSeed(std::type_index(typeid(T)), std::move(definition));
	}

	const InstanceClassDefinition *GetDefinitionByType(std::type_index type);

	template <typename T> const InstanceClassDefinition *GetDefinition() {
		return GetDefinitionByType(std::type_index(typeid(T)));
	}

	const InstanceClassDefinition *GetDefinition(Instance *instance);
	const InstanceClassDefinition *GetDefinitionByName(std::string_view name);
	const InstanceClassDefinition *GetDefinitionBySchemaId(SchemaId id);
	std::vector<std::string> GetClassNames();
}

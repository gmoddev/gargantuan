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
		if (instance->GetRuntimeSchemaClassId().IsValid())
			return GetDefinitionBySchemaId(instance->GetRuntimeSchemaClassId());
		return GetDefinitionByType(std::type_index(typeid(*instance)));
	}

	const InstanceClassDefinition *GetDefinitionByName(std::string_view name) {
		return GetActiveRuntimeSchemaRegistry().FindByName(name);
	}

	const InstanceClassDefinition *GetDefinitionBySchemaId(SchemaId id) {
		return GetActiveRuntimeSchemaRegistry().FindById(id);
	}

	bool IsConstructible(const InstanceClassDefinition &definition) {
		return GetActiveRuntimeSchemaRegistry().IsClassConstructible(definition);
	}

	std::shared_ptr<Instance> Construct(const InstanceClassDefinition &definition) {
		const auto &registry = GetActiveRuntimeSchemaRegistry();
		if (!registry.IsClassConstructible(definition)) return nullptr;
		if (definition.ConstructionKind == SchemaClassConstructionKind::Native)
			return definition.Constructor();
		auto *host = registry.FindClassById(definition.NativeHostClassId);
		if (!host || !host->Constructor) return nullptr;
		auto instance = host->Constructor();
		if (!instance) return nullptr;
		instance->BindRuntimeSchemaClass(definition.Id);
		return instance;
	}

	std::shared_ptr<Instance> ConstructByName(std::string_view name) {
		auto *definition = GetDefinitionByName(name);
		return definition ? Construct(*definition) : nullptr;
	}

	std::vector<std::string> GetClassNames() {
		std::vector<std::string> result;
		for (const auto *definition : GetActiveRuntimeSchemaRegistry().Enumerate())
			result.push_back(definition->ConstructionKind == SchemaClassConstructionKind::CustomData
				? definition->CanonicalName : definition->ClassName);
		return result;
	}
}

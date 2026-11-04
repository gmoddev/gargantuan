#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"

#include <SDL3/SDL.h>
#include <optional>

namespace gargantuan {
	namespace {
		bool IsCanonicalService(
			ServiceProvider *Provider,
			const std::shared_ptr<Instance> &Service,
			SchemaId ClassId
		) {
			if (!Service || Service->GetDestroyed() || Service->IsDestroying()) return false;
			auto *Definition = InstanceClassRegistry::GetDefinition(Service.get());
			if (!Definition || Definition->Id != ClassId) return false;
			auto Parent = Service->GetParent();
			return Parent && Parent->get() == Provider && Service->GetDataModel() == Provider->GetDataModel();
		}
	}

	std::optional<std::shared_ptr<Instance>> ServiceProvider::FindService(std::string Name) {
		auto Definition = GetServiceDefinitions().find(Name);
		if (Definition == GetServiceDefinitions().end()) return std::nullopt;
		const auto *Class = InstanceClassRegistry::GetDefinitionBySchemaId(Definition->second);
		if (Class && Class->ServicePolicy == SchemaServicePolicy::ServerOnly &&
			GetCurrentScriptSecurityContext().Domain == ScriptExecutionDomain::Client) return std::nullopt;
		auto Existing = Services.find(Name);
		if (Existing == Services.end()) return std::nullopt;
		if (IsCanonicalService(this, Existing->second, Definition->second)) return Existing->second;
		Services.erase(Existing);
		return std::nullopt;
	}

	std::optional<std::shared_ptr<Instance>> ServiceProvider::ResolveService(std::string_view NameView) {
		auto Name = std::string(NameView);
		const auto &Definitions = GetServiceDefinitions();
		auto Registered = Definitions.find(Name);
		if (Registered == Definitions.end()) return std::nullopt;
		if (auto Existing = FindService(Name)) return Existing;

		auto *Definition = InstanceClassRegistry::GetDefinitionBySchemaId(Registered->second);
		if (!Definition || !InstanceClassRegistry::IsConstructible(*Definition))
			throw std::runtime_error(std::format("Service '{}' has no constructible schema", Name));
		if (Definition->ServicePolicy == SchemaServicePolicy::ServerOnly &&
			GetCurrentScriptSecurityContext().Domain == ScriptExecutionDomain::Client) return std::nullopt;

		const auto ClassName = Definition->ConstructionKind == SchemaClassConstructionKind::CustomData
			? Definition->CanonicalName : Definition->ClassName;
		if (auto Existing = FindFirstChildOfClass(ClassName, std::nullopt);
			IsCanonicalService(this, Existing, Registered->second)) {
			Services.emplace(Name, Existing);
			return Existing;
		}

		auto Service = InstanceClassRegistry::Construct(*Definition);
		Service->SetName(Name);
		if (GetArchivable()) Service->MarkPersistenceSubtreeArchivable();
		Service->SetParent(this->shared_from_this());
		Services.emplace(Name, Service);
		return Service;
	}

	std::shared_ptr<Instance> ServiceProvider::GetService(std::string Name) {
		if (auto Service = ResolveService(Name)) return *Service;
		throw std::runtime_error(std::format("Unknown service named '{}'", Name));
	}
}

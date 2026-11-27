#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/services/AudioMixService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>
#include <optional>

namespace gargantuan {
	namespace {
		constexpr auto AudioMixServiceId = SchemaId::FromNativeName("Engine", "AudioMixService");
		constexpr auto WorkspaceId = SchemaId::FromNativeName("Engine", "Workspace");

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
		const auto IsAvailable = [&](const std::shared_ptr<Instance> &Service) {
			if (!IsCanonicalService(this, Service, Definition->second)) return false;
			return Definition->second != AudioMixServiceId || static_cast<AudioMixService &>(*Service).HasAttachedRuntime();
		};
		if (Existing != Services.end()) {
			if (IsAvailable(Existing->second)) return Existing->second;
			Services.erase(Existing);
		}
		// Local presentation services are installed by the runtime host, so no
		// script can materialize a mix in an authoring world or on the server.
		if (Definition->second == AudioMixServiceId) {
			auto Attached = FindFirstChildOfClass("AudioMixService", false);
			if (IsAvailable(Attached)) {
				Services.emplace(Name, Attached);
				return Attached;
			}
		}
		return std::nullopt;
	}

	std::optional<std::shared_ptr<Instance>> ServiceProvider::ResolveService(std::string_view NameView) {
		auto Name = std::string(NameView);
		const auto &Definitions = GetServiceDefinitions();
		auto Registered = Definitions.find(Name);
		if (Registered == Definitions.end()) return std::nullopt;
		const auto Prepare = [&](const std::shared_ptr<Instance> &Service) {
			if (Registered->second == WorkspaceId && dynamic_cast<DataModel *>(this))
				static_cast<Workspace &>(*Service).InitializeTerrain();
			return Service;
		};
		if (auto Existing = FindService(Name)) return Prepare(*Existing);
		if (Registered->second == AudioMixServiceId) return std::nullopt;

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
			return Prepare(Existing);
		}

		auto Service = InstanceClassRegistry::Construct(*Definition);
		Service->SetName(Name);
		if (GetArchivable()) Service->MarkPersistenceSubtreeArchivable();
		Service->SetParent(this->shared_from_this());
		Services.emplace(Name, Service);
		return Prepare(Service);
	}

	std::shared_ptr<Instance> ServiceProvider::GetService(std::string Name) {
		if (auto Service = ResolveService(Name)) return *Service;
		throw std::runtime_error(std::format("Unknown service named '{}'", Name));
	}
}

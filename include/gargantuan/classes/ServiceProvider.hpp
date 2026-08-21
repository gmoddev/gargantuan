#pragma once

#include "gargantuan/classes/generated/ServiceProvider.hpp"

#include <string_view>
#include <unordered_map>

namespace gargantuan {
	class ServiceProvider : public Instance {
		I_ServiceProvider;

		[[nodiscard]] std::optional<std::shared_ptr<Instance>> ResolveService(std::string_view Name);

	  protected:
		using ServiceDefinitions = std::unordered_map<std::string, SchemaId>;
		virtual const ServiceDefinitions &GetServiceDefinitions() const = 0;

	  private:
		std::unordered_map<std::string, std::shared_ptr<Instance>> Services;
	};
}

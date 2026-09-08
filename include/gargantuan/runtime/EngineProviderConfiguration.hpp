#pragma once

#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/runtime/RuntimeMode.hpp"

#include <memory>
#include <optional>

namespace gargantuan {
	class IEntitlementProvider;

	// Trusted host composition only. Provider objects and their deployment
	// configuration never enter the DataModel or project serialization.
	struct EngineProviderConfiguration final {
		std::shared_ptr<IEntitlementProvider> Entitlements;
		std::optional<ContentAvailabilityConfiguration> Content;
		bool AudioEnabled = false;
		RuntimeMode Mode = RuntimeMode::Offline;
	};
}

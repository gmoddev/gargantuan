#pragma once

#include <memory>

namespace gargantuan {
	class IEntitlementProvider;

	// Trusted host composition only. Provider objects and their deployment
	// configuration never enter the DataModel or project serialization.
	struct EngineProviderConfiguration final {
		std::shared_ptr<IEntitlementProvider> Entitlements;
	};
}

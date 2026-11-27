#pragma once

#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/runtime/RuntimeMode.hpp"

#include <memory>
#include <optional>

namespace gargantuan {
	class IEntitlementProvider;
	class IPersistenceProvider;

	// Trusted host composition only. Provider objects and their deployment
	// configuration never enter the DataModel or project serialization.
	struct EngineProviderConfiguration final {
		std::shared_ptr<IEntitlementProvider> Entitlements;
		std::optional<ContentAvailabilityConfiguration> Content;
		bool AudioEnabled = false;
		RuntimeMode Mode = RuntimeMode::Offline;
		// The official on-demand server enables 3M. Native 3L fixtures and
		// manual-demand hosts keep their established request ownership.
		bool EnableResidencyDemandPolicy = false;
		bool EnableStreamingDiagnostics = false;
		std::shared_ptr<IPersistenceProvider> Persistence;
		// Set by the packaged network host after matching terrain delivery admission.
		bool AdmitNetworkTerrain = false;
		// Native qualification hosts can select the renderer-only terrain detail trial.
		bool ExperimentalTerrainVisualDensity = false;
		bool AdmitSpatialContent = false;
	};
}

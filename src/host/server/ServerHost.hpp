#pragma once

#include "gargantuan/content/ContentAvailability.hpp"

#include <memory>

namespace gargantuan::host {
	struct ServerHostConfiguration final {
		std::shared_ptr<IContentAvailabilityProvider> ContentProvider;
		ContentResidencyMode ContentMode = ContentResidencyMode::FullyResident;
	};

	int RunDedicatedServer(
		int ArgumentCount,
		char *Arguments[],
		ServerHostConfiguration HostConfiguration = {}
	);
}

#pragma once

#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>

namespace gargantuan::host {
	struct LocalServerContentConfiguration final {
		ContentResidencyMode Mode = ContentResidencyMode::FullyResident;
	};

	struct NodeServerContentConfiguration final {
		std::string Endpoint;
		std::filesystem::path RootCertificateFile;
		std::string WorkloadTokenEnvironment;
		ContentResidencyMode Mode = ContentResidencyMode::FullyResident;
		std::chrono::milliseconds BootstrapDeadline{30'000};
	};

	// Retained as an internal composition seam for focused host tests. Official
	// GargantuanServer deployment configuration selects Local or Node instead.
	struct InjectedServerContentConfiguration final {
		std::shared_ptr<IContentAvailabilityProvider> Provider;
		ContentResidencyMode Mode = ContentResidencyMode::FullyResident;
	};

	using ServerContentConfiguration = std::variant<
		LocalServerContentConfiguration,
		NodeServerContentConfiguration,
		InjectedServerContentConfiguration
	>;

	struct ServerHostConfiguration final {
		ServerContentConfiguration Content = LocalServerContentConfiguration{};
		std::optional<network::ReliableServiceProfile> ReliableService;
	};

	int RunDedicatedServer(
		int ArgumentCount,
		char *Arguments[],
		ServerHostConfiguration HostConfiguration = {}
	);
}

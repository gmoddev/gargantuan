#pragma once

#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/network/GameAdmission.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include "gargantuan/network/Transport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
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

	struct ServerListeningEvidence final {
		PackageContentNamespace Package;
		AssetContentId ContentManifestDigest;
		network::TransportEndpoint Endpoint;
	};

	struct ManagedServerSnapshot final {
		bool AuthorityCurrent = false;
		bool ReadyAcknowledged = false;
		bool DrainRequested = false;
	};

	// A trusted startup-owned wrapper supplies cached state. Callbacks must only
	// signal its asynchronous worker; they must not perform I/O or block ticks.
	class IManagedServerLifecycle {
	  public:
		virtual ~IManagedServerLifecycle() = default;
		// ReadyAcknowledged may become true only after OnListening and the
		// wrapper's external lease/READY acknowledgement have both completed.
		[[nodiscard]] virtual ManagedServerSnapshot Snapshot() const noexcept = 0;
		virtual void OnListening(ServerListeningEvidence Evidence) noexcept = 0;
		virtual void OnDraining() noexcept = 0;
		virtual void OnStopped() noexcept = 0;
	};

	struct ServerHostConfiguration final {
		ServerContentConfiguration Content = LocalServerContentConfiguration{};
		std::optional<network::ReliableServiceProfile> ReliableService;
		// Both are required for managed admission. Default official servers remain
		// DevelopmentLocal and never claim external authority.
		std::shared_ptr<network::IGameAdmissionProvider> AdmissionProvider;
		std::shared_ptr<IManagedServerLifecycle> ManagedLifecycle;
		std::chrono::milliseconds ManagedStartupTimeout{6'000};
	};

	int RunDedicatedServer(
		int ArgumentCount,
		char *Arguments[],
		ServerHostConfiguration HostConfiguration = {}
	);
}

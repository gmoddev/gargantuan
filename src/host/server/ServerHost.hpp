#pragma once

#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/network/GameAdmission.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"
#include "gargantuan/network/Transport.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace gargantuan { class IPersistenceProvider; struct WorldSlot; }

namespace gargantuan::host {
	// v3 includes the persistence provider in the by-value host configuration.
	// The managed lifecycle snapshot and callback slots retain their v2 layout.
	[[nodiscard]] std::uint32_t GetManagedHostLifecycleContractVersion() noexcept;
	inline constexpr std::uint32_t ManagedHostLifecycleContractVersion = 3;

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
		network::TransportServerPublicKey ServerPublicKey{};
	};

	struct ManagedSessionEvidence final {
		std::uint64_t AcceptedPeers = 0;
		std::uint64_t PlayersCreated = 0;
		std::uint64_t PlayersRemoved = 0;
	};

	struct ManagedServerSnapshot final {
		// An authenticated DRAIN may retain local control readiness for one
		// host observation after the admission gate closes. Unexpected revoke
		// clears both readiness flags and must remain a failed host exit.
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
		// An optional native accounting witness. Ordinary managed hosts retain
		// their existing lifecycle contract when they do not consume it.
		virtual void OnSessionStopped(ManagedSessionEvidence) noexcept {}
		virtual void OnStopped() noexcept = 0;
	};

	// A private process controller may additionally implement this interface.
	// Existing managed lifecycle adapters keep their v2 layout and virtual slots.
	class IPrivateGameplayControl {
	  public:
		virtual ~IPrivateGameplayControl() = default;
		[[nodiscard]] virtual std::uint64_t GetHoldRequest() const noexcept = 0;
		virtual bool IsPreparingLeave() const noexcept = 0;
		virtual void ReportLeave(char Result) noexcept = 0;
		virtual void AcknowledgeHold(std::uint64_t Request) noexcept = 0;
		[[nodiscard]] virtual const WorldSlot *GetWorldSlot() const noexcept = 0;
		[[nodiscard]] virtual const std::filesystem::path &GetProfile() const noexcept = 0;
		[[nodiscard]] virtual bool AwaitingOwnerReady() const noexcept { return false; }
		virtual void ConfirmOwnerReady() noexcept {}
		virtual void ServiceGuestInvitations() {}
	};

	struct ServerHostConfiguration final {
		ServerContentConfiguration Content = LocalServerContentConfiguration{};
		std::optional<network::ReliableServiceProfile> ReliableService;
		std::shared_ptr<IPersistenceProvider> Persistence;
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

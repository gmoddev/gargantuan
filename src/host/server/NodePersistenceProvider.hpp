#pragma once

#include "gargantuan/persistence/PersistenceProvider.hpp"
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include <optional>
#endif

#include <filesystem>
#include <memory>
#include <string>

namespace gargantuan::host {
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
	struct NodeDevelopmentBinding final {
		ProjectId Project;
		std::string Tenant;
		std::string Database;
		std::string DatabaseGeneration;
		std::string NodeInstance;
		std::string CertificateSha256;
	};
#endif

	struct NodePersistenceProviderConfiguration final {
		std::string Endpoint;
		std::filesystem::path RootCertificateFile;
		std::filesystem::path WorkloadTokenFile;
#if defined(GARGANTUAN_WITH_STUDIO_NODE_DEVELOPMENT)
		std::optional<NodeDevelopmentBinding> Development;
#endif
	};

	// A server-owned adapter. The authenticated workload credential binds the
	// Node tenant. WorkloadTokenFile is re-read on the worker for each call.
	class NodePersistenceProvider final : public IPersistenceProvider {
	  public:
		explicit NodePersistenceProvider(NodePersistenceProviderConfiguration Configuration);
		~NodePersistenceProvider() override;

		PersistenceCapabilities Capabilities() const noexcept override;
		PersistenceResult Execute(const PersistenceRequest &Request, std::stop_token Stop) override;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
} // namespace gargantuan::host

#pragma once

#include "gargantuan/persistence/PersistenceProvider.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace gargantuan::host {
	struct NodePersistenceProviderConfiguration final {
		std::string Endpoint;
		std::filesystem::path RootCertificateFile;
		std::filesystem::path WorkloadTokenFile;
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

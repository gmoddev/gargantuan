#pragma once

#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/classes/generated/DataModel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/runtime/AuthoritativeTransactions.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <cstdint>
#include <filesystem>

namespace gargantuan {
	class ScopedAuthoritativeRevisionDeferral;
	class DataModel : public ServiceProvider {
		I_DataModel;

		const ServiceDefinitions &GetServiceDefinitions() const override;

		std::filesystem::path Root;
		BaseFilesystem *Filesystem = nullptr;
		TagIndex Tags;
		AuthoritativeTransactionHistory Transactions;

		static constexpr std::uint64_t InitialProjectRevision = 1;
		[[nodiscard]] std::uint64_t GetAuthoritativeRevision() const { return AuthoritativeRevision; }
		void EnsureAuthoritativeRevisionAvailable() const;
		void AdvanceAuthoritativeRevision();
		void BeginAuthoritativeRevisionBatch();
		void CommitAuthoritativeRevisionBatch();
		void CancelAuthoritativeRevisionBatch() noexcept;
		void InitializeLoadedProjectRevision();
		[[nodiscard]] bool IsProtectedService(const std::shared_ptr<Instance> &InstanceValue) const;
		[[nodiscard]] bool IsProtectedServiceClass(SchemaId ClassSchemaId) const;

	  private:
		friend class ScopedAuthoritativeRevisionDeferral;
		std::uint64_t AuthoritativeRevision = InitialProjectRevision;
		bool RevisionBatchActive = false;
		bool RevisionBatchChanged = false;
	};

	class ScopedAuthoritativeRevisionDeferral {
	  public:
		ScopedAuthoritativeRevisionDeferral(DataModel &World, bool &Changed);
		~ScopedAuthoritativeRevisionDeferral();
		ScopedAuthoritativeRevisionDeferral(const ScopedAuthoritativeRevisionDeferral &) = delete;
		ScopedAuthoritativeRevisionDeferral &operator=(const ScopedAuthoritativeRevisionDeferral &) = delete;

	  private:
		DataModel *World;
	};
}

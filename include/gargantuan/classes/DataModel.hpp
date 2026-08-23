#pragma once

#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/classes/generated/DataModel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/runtime/AuthoritativeTransactions.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <cstdint>
#include <filesystem>
#include <cstddef>

namespace gargantuan {
	class ScopedAuthoritativeRevisionDeferral;
	class AssetService;
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
		[[nodiscard]] bool CanAdoptInstances(std::size_t Count) const;
		void AdoptInstances(std::size_t Count);
		void ReleaseInstance() noexcept;
		[[nodiscard]] std::size_t GetOwnedInstanceCount() const { return OwnedInstanceCount; }

	  private:
		friend class ScopedAuthoritativeRevisionDeferral;
		friend class AssetService;
		std::uint64_t AuthoritativeRevision = InitialProjectRevision;
		bool RevisionBatchActive = false;
		bool RevisionBatchChanged = false;
		std::size_t OwnedInstanceCount = 1;
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

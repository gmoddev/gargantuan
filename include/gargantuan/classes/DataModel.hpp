#pragma once

#include "gargantuan/classes/ServiceProvider.hpp"
#include "gargantuan/classes/generated/DataModel.hpp"
#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/runtime/AuthoritativeTransactions.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <cstddef>

namespace gargantuan {
	class ScopedAuthoritativeRevisionDeferral;
	class AssetService;
	class PackageBuilder;
	class SpatialRegion;
	namespace network { class SpatialTransferProtection; }
	class DataModel : public ServiceProvider {
		friend class ApplicationPresentation;
		bool ApplicationGraph = false;
		I_DataModel;
		~DataModel() override;

		const ServiceDefinitions &GetServiceDefinitions() const override;

		std::filesystem::path Root;
		BaseFilesystem *Filesystem = nullptr;
		TagIndex Tags;
		[[nodiscard]] std::vector<std::shared_ptr<SpatialRegion>> GetSpatialRegions() const;
		[[nodiscard]] std::shared_ptr<network::SpatialTransferProtection> AcquireSpatialTransferProtection(std::function<bool(ObjectId)> IsControlled);
		[[nodiscard]] std::shared_ptr<network::SpatialTransferProtection> GetSpatialTransferProtection() const { return MovementProtection.lock(); }

	  private:
		friend class SpatialRegion;
		std::map<ObjectId, std::weak_ptr<SpatialRegion>> SpatialRegions;
		std::weak_ptr<network::SpatialTransferProtection> MovementProtection;

	  public:
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
		// The current game protocol admits a single coordinate domain and has no native cell stream.
		struct DefaultNetworkSpatialProfile { bool SpatialContent{}; };
		[[nodiscard]] std::shared_ptr<const DefaultNetworkSpatialProfile> AcquireDefaultNetworkSpatialProfile(bool SpatialContent = false);
		[[nodiscard]] bool SupportsNetworkSpatialContent() const {
			const auto Profile = NetworkSpatialProfile.lock();
			return Profile && Profile->SpatialContent;
		}
		[[nodiscard]] bool HasDefaultNetworkSpatialProfile() const { return !NetworkSpatialProfile.expired(); }
		static void CheckDefaultNetworkSpatialParticipant(const Instance &Node, bool SpatialContent = false);
		[[nodiscard]] std::string_view GetPackagedClientCodeIdentity() const { return PackagedClientCodeIdentity; }

	  private:
		friend class ScopedAuthoritativeRevisionDeferral;
		friend class AssetService;
		friend class PackageBuilder;
		friend class MutationGateway;
		friend class PreparedPropertyCommit;
		friend class PreparedGridTransfer;
		friend struct PreparedPropertyTestAccess;
		[[nodiscard]] bool HasDeferredRevision() const noexcept;
		std::uint64_t AuthoritativeRevision = InitialProjectRevision;
		bool RevisionBatchActive = false;
		std::weak_ptr<const DefaultNetworkSpatialProfile> NetworkSpatialProfile;
		bool RevisionBatchChanged = false;
		std::string PackagedClientCodeIdentity;
		std::size_t OwnedInstanceCount = 1;
		mutable ServiceDefinitions RegisteredServices;
		mutable std::uint64_t ServiceSchemaGeneration = 0;
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

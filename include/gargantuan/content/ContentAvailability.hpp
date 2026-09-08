#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	class DataModel;
	class Instance;
	class Workspace;

	inline constexpr std::uint32_t PackageContentManifestVersion = 1;
	inline constexpr std::uint32_t PackageContentInstanceSchemaVersion = 4;
	inline constexpr std::size_t MaximumPackageContentUnits = 65'536;
	inline constexpr std::size_t MaximumPackageContentDependencies = 262'144;
	inline constexpr std::size_t MaximumPackageContentDependenciesPerUnit = 64;
	inline constexpr std::size_t MaximumPackageContentKeyBytes = 128;
	inline constexpr std::size_t MaximumPackageSpaceKeyBytes = 64;
	inline constexpr std::size_t MaximumPackageBlobReferenceBytes = 256;
	inline constexpr std::size_t MaximumPackageContentManifestBytes = 32 * 1024 * 1024;
	inline constexpr std::size_t MaximumPackageContentManifestJsonNodes = 2 * 1024 * 1024;
	inline constexpr std::size_t MaximumPackageContentPayloadBytes = 8 * 1024 * 1024;
	inline constexpr std::size_t MaximumPackageContentObjectsPerUnit = 512;

	struct PackageContentNamespace final {
		ProjectId Project;
		std::uint64_t PackageVersion = 0;
		auto operator<=>(const PackageContentNamespace &) const = default;
	};

	struct PackageContentIdentity final {
		PackageContentNamespace Package;
		std::string Key;
		auto operator<=>(const PackageContentIdentity &) const = default;
	};

	struct PackageCoarseBounds final {
		std::array<float, 3> Minimum{};
		std::array<float, 3> Maximum{};
	};

	enum class PackageContentFlags : std::uint8_t {
		None = 0,
		RequiredAtBootstrap = 1 << 0,
		ImmutableBaseline = 1 << 1,
	};

	[[nodiscard]] constexpr PackageContentFlags operator|(PackageContentFlags Left, PackageContentFlags Right) {
		return static_cast<PackageContentFlags>(static_cast<unsigned>(Left) | static_cast<unsigned>(Right));
	}
	[[nodiscard]] constexpr bool HasPackageContentFlag(PackageContentFlags Value, PackageContentFlags Flag) {
		return (static_cast<unsigned>(Value) & static_cast<unsigned>(Flag)) != 0;
	}

	struct PackageContentEntry final {
		std::string Key;
		std::string BlobReference;
		AssetContentId Digest;
		std::uint64_t CompressedBytes = 0;
		std::uint64_t UncompressedBytes = 0;
		std::uint32_t ObjectCount = 0;
		std::vector<std::string> Dependencies;
		std::string PackageSpaceKey = "default";
		std::optional<PackageCoarseBounds> CoarseBounds;
		PackageContentFlags Flags = PackageContentFlags::ImmutableBaseline;
	};

	struct PackageContentManifest final {
		PackageContentNamespace Package;
		std::uint32_t InstanceSchemaVersion = PackageContentInstanceSchemaVersion;
		std::vector<PackageContentEntry> Entries;
	};

	using PackageContentManifestResult = std::expected<PackageContentManifest, std::string>;
	[[nodiscard]] std::string EncodePackageContentManifest(const PackageContentManifest &Manifest);
	[[nodiscard]] PackageContentManifestResult ParsePackageContentManifest(std::string_view Encoded);
	[[nodiscard]] bool IsValidPackageContentKey(std::string_view Value);

	class PackageContentCoarseIndex final {
	  public:
		explicit PackageContentCoarseIndex(const PackageContentManifest &Manifest, float CellSize = 512.0f);
		[[nodiscard]] std::vector<std::size_t> Query(
			std::string_view PackageSpaceKey,
			const PackageCoarseBounds &Bounds,
			std::size_t MaximumResults
		) const;
		[[nodiscard]] std::size_t GetMembershipCount() const;

	  private:
		struct Impl;
		std::shared_ptr<const Impl> State;
	};

	enum class ContentProviderErrorCode : std::uint8_t {
		Cancelled,
		DeadlineExceeded,
		Unavailable,
		NotFound,
		InvalidResponse,
		ResourceExhausted,
	};

	struct ContentProviderError final {
		ContentProviderErrorCode Code = ContentProviderErrorCode::Unavailable;
		std::string Message;
	};

	struct ContentRequestContext final {
		std::shared_ptr<const std::atomic_bool> Cancelled;
		std::chrono::steady_clock::time_point Deadline;
		std::uint64_t SessionGeneration = 0;

		[[nodiscard]] bool IsCancelled() const;
		[[nodiscard]] bool IsExpired() const;
	};

	struct ContentPayload final {
		PackageContentIdentity Identity;
		AssetContentId Digest;
		std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
	};

	using ContentManifestProviderResult = std::expected<std::string, ContentProviderError>;
	using ContentPayloadProviderResult = std::expected<ContentPayload, ContentProviderError>;
	using ContentProviderLifecycleResult = std::expected<void, ContentProviderError>;

	class IContentAvailabilityProvider {
	  public:
		virtual ~IContentAvailabilityProvider() = default;
		[[nodiscard]] virtual std::string_view Name() const = 0;
		[[nodiscard]] virtual ContentProviderLifecycleResult Start(const ContentRequestContext &) { return {}; }
		virtual void Stop() {}
		[[nodiscard]] virtual ContentManifestProviderResult GetManifest(
			const ContentRequestContext &Context,
			const PackageContentNamespace &Package
		) = 0;
		[[nodiscard]] virtual ContentPayloadProviderResult GetContent(
			const ContentRequestContext &Context,
			const PackageContentIdentity &Identity
		) = 0;
	};

	class LocalPackageContentProvider final : public IContentAvailabilityProvider {
	  public:
		LocalPackageContentProvider(
			std::filesystem::path PackageRoot,
			std::string ManifestReference,
			AssetContentId ExpectedManifestDigest
		);
		[[nodiscard]] std::string_view Name() const override { return "local-package"; }
		[[nodiscard]] ContentProviderLifecycleResult Start(const ContentRequestContext &Context) override;
		void Stop() override;
		[[nodiscard]] ContentManifestProviderResult GetManifest(
			const ContentRequestContext &Context,
			const PackageContentNamespace &Package
		) override;
		[[nodiscard]] ContentPayloadProviderResult GetContent(
			const ContentRequestContext &Context,
			const PackageContentIdentity &Identity
		) override;

	  private:
		struct Impl;
		std::shared_ptr<Impl> State;
	};

	enum class ContentResidencyMode : std::uint8_t { FullyResident, OnDemand };
	enum class ContentResidencyState : std::uint8_t {
		Unavailable,
		Requested,
		Acquiring,
		Available,
		Admitting,
		Resident,
		Evicting,
		Failed,
	};

	struct ContentAvailabilityLimits final {
		std::size_t WorkerCount = 2;
		std::size_t MaximumInFlight = 4;
		std::size_t MaximumPendingRequests = 1024;
		std::size_t MaximumCompletedPayloadBytes = 16 * 1024 * 1024;
		std::size_t MaximumCachedPayloadBytes = 32 * 1024 * 1024;
		std::size_t MaximumCompletionsPerTick = 4;
		std::size_t MaximumAdmissionUnitsPerTick = 2;
		std::size_t MaximumAdmissionObjectsPerTick = MaximumPackageContentObjectsPerUnit;
		std::size_t MaximumAdmissionBytesPerTick = MaximumPackageContentPayloadBytes;
		std::size_t MaximumEvictionUnitsPerTick = 2;
		std::size_t MaximumEvictionObjectsPerTick = MaximumPackageContentObjectsPerUnit;
		std::chrono::milliseconds RequestTimeout{10'000};
	};

	struct ContentAvailabilityConfiguration final {
		std::shared_ptr<IContentAvailabilityProvider> Provider;
		PackageContentNamespace Package;
		AssetContentId ManifestDigest;
		ContentResidencyMode Mode = ContentResidencyMode::FullyResident;
		ContentAvailabilityLimits Limits;
	};

	struct ContentAvailabilityMetrics final {
		std::uint64_t ManifestBytes = 0;
		std::uint64_t Requests = 0;
		std::uint64_t Acquisitions = 0;
		std::uint64_t AcquisitionDeduplications = 0;
		std::uint64_t CacheHits = 0;
		std::uint64_t Cancellations = 0;
		std::uint64_t Failures = 0;
		std::uint64_t Admissions = 0;
		std::uint64_t Evictions = 0;
		std::uint64_t ObjectsAdmitted = 0;
		std::uint64_t ObjectsEvicted = 0;
		std::uint64_t BytesAcquired = 0;
		std::uint64_t BytesVerified = 0;
		std::uint64_t InFlightHighWater = 0;
		std::uint64_t PendingHighWater = 0;
		std::uint64_t AdmissionPendingHighWater = 0;
		std::uint64_t CompletedPayloadBytesHighWater = 0;
		std::uint64_t CachedPayloadBytesHighWater = 0;
	};

	class ContentAvailabilityService final {
	  public:
		ContentAvailabilityService(
			std::shared_ptr<DataModel> World,
			std::shared_ptr<Workspace> WorkspaceValue,
			ContentAvailabilityConfiguration Configuration,
			std::function<void(std::string, std::string)> Diagnostic = {}
		);
		~ContentAvailabilityService();

		void Step();
		void Stop();
		[[nodiscard]] bool RequestContent(std::string_view Key);
		[[nodiscard]] bool ReleaseContent(std::string_view Key);
		[[nodiscard]] bool PinContent(std::string_view Key);
		[[nodiscard]] bool UnpinContent(std::string_view Key);
		[[nodiscard]] bool RetryContent(std::string_view Key);
		[[nodiscard]] std::optional<ContentResidencyState> GetState(std::string_view Key) const;
		[[nodiscard]] bool IsManifestAvailable() const;
		[[nodiscard]] bool IsBootstrapComplete() const;
		[[nodiscard]] bool IsFullyResident() const;
		[[nodiscard]] std::size_t GetActiveRequestCount() const;
		[[nodiscard]] ContentAvailabilityMetrics GetMetrics() const;
		[[nodiscard]] const PackageContentManifest *GetManifest() const;

		ContentAvailabilityService(const ContentAvailabilityService &) = delete;
		ContentAvailabilityService &operator=(const ContentAvailabilityService &) = delete;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}

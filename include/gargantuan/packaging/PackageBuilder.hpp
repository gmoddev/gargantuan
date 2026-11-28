#pragma once
#include "gargantuan/blockgrid/Persistence.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/content/ContentAvailability.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/terrain/Persistence.hpp"
#include "gargantuan/packaging/RuntimeDistribution.hpp"
#include "gargantuan/packaging/RuntimePackageCrypto.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	class DataModel;
	class LuaSourceContainer;
	class Project;
	class RuntimeSchemaRegistry;
	class RuntimeScriptCode;
	struct RuntimeDistributionCodeCatalog;

	inline constexpr std::uint32_t GamePackageFormatVersion = 2;
	inline constexpr std::uint32_t DistributedPlayerPackageFormatVersion = 3;
	inline constexpr std::uint32_t RuntimeDistributionPackageFormatVersion = 4;
	inline constexpr std::uint32_t StandalonePackageFormatVersion = 5;
	inline constexpr std::uint32_t RuntimeCompatibilityVersion = 1;

	enum class PackageConfiguration : std::uint8_t { Development, Release };
	enum class PackageRuntimeLoadMode : std::uint8_t { Complete, LoopbackDevelopmentClient };
	enum class PackageAudience : std::uint8_t { Trusted, DistributedPlayer };
	enum class PackagePhase : std::uint8_t {
		Snapshot,
		Validate,
		StageRuntime,
		StageContent,
		HashManifest,
		Finalize,
		Complete,
	};
	enum class PackageDiagnosticSeverity : std::uint8_t { Info, Warning, Error };
	enum class PackageFailurePoint : std::uint8_t {
		None,
		Snapshot,
		Validate,
		StageRuntime,
		StageContent,
		HashManifest,
		ManifestWrite,
		Finalize,
	};

	struct PackageDiagnostic {
		PackageDiagnosticSeverity Severity = PackageDiagnosticSeverity::Info;
		std::string Category;
		std::string Code;
		std::string Message;
		std::string Item;
	};

	struct PackageCancellationToken {
		std::shared_ptr<std::atomic_bool> Cancelled = std::make_shared<std::atomic_bool>(false);
		void Cancel() const {
			Cancelled->store(true, std::memory_order_release);
		}
		[[nodiscard]] bool IsCancelled() const {
			return Cancelled->load(std::memory_order_acquire);
		}
	};

	class DistributedPlayerCapture final {
		friend class PackageBuilder;

		DistributedPlayerCapture(ProjectId Project, std::uint64_t Revision, std::string Code, AssetRuntimeSnapshot Assets)
			: Project(Project), Revision(Revision), Code(std::move(Code)), Assets(std::move(Assets)) {}

		const ProjectId Project;
		const std::uint64_t Revision;
		const std::string Code;
		const AssetRuntimeSnapshot Assets;
	};

	struct GamePayload {
		ProjectId Identity;
		std::string DisplayName;
		std::uint64_t AuthoritativeRevision = 0;
		bool UnsavedChanges = false;
		std::string ProjectJson;
		std::optional<std::string> ApplicationTemplate;
		std::optional<std::string> PreRunSource;
		AssetRuntimeSnapshot Assets;
		// Captured on Main; keeps the frozen reflection generation alive during worker packaging.
		std::shared_ptr<const RuntimeSchemaRegistry> SchemaSnapshot;
		// Native capture observation only; includes lazy canonical mesh decoding.
		std::chrono::microseconds SpatialInspectionDuration{};
		struct ContentUnit final {
			PackageContentEntry Entry;
			std::string Payload;
		};
		std::vector<ContentUnit> ContentUnits;
		// Captured units use the current schema. A qualified legacy payload may
		// select version 4 only when every content unit matches that version.
		std::uint32_t ContentInstanceSchemaVersion = PackageContentInstanceSchemaVersion;
		std::vector<terrain::TerrainArtifact> TerrainArtifacts;
		blockgrid::SegmentMap GridSegments;
		std::shared_ptr<const DistributedPlayerCapture> DistributedPlayer;
		std::optional<RuntimeDistributionPlan> DistributionPlan;
	};

	struct PackageSizeBreakdown {
		std::uint64_t TotalBytes = 0;
		std::uint64_t RuntimeBytes = 0;
		std::uint64_t ProjectBytes = 0;
		std::uint64_t AssetBytes = 0;
		std::uint64_t ShaderBytes = 0;
		std::uint64_t OtherBytes = 0;
	};

	struct PackagePhaseTiming {
		PackagePhase Phase = PackagePhase::Snapshot;
		std::chrono::microseconds Duration{};
	};

	struct PackageBuildRequest {
		GamePayload Payload;
		std::filesystem::path RuntimeDistributionRoot;
		std::filesystem::path OutputDirectory;
		PackageConfiguration Configuration = PackageConfiguration::Release;
		PackageCancellationToken Cancellation;
		std::function<void(PackagePhase)> Progress;
		PackageFailurePoint FailurePoint = PackageFailurePoint::None;
		std::optional<RuntimePackageProvisionedSecurity> DistributionSecurity;
	};

	struct PackageBuildResult {
		bool Ok = false;
		bool Cancelled = false;
		ProjectId Identity;
		std::uint64_t PackagedRevision = 0;
		bool PackagedUnsavedChanges = false;
		std::filesystem::path OutputDirectory;
		std::filesystem::path PlayerExecutable;
		PackageSizeBreakdown Size;
		std::vector<PackagePhaseTiming> Timings;
		std::vector<PackageDiagnostic> Diagnostics;
		std::optional<RuntimeDistributionTarget> Target;
		std::string ManifestSha256;
		std::string PlayerExecutableSha256;
		std::filesystem::path PrivateDiagnosticsPath;
	};

	struct RuntimeDistributionAdmission final {
		std::optional<std::string> ExpectedManifestSha256;
		std::optional<RuntimePackageProvisionedSecurity> Security;
		bool RequirePublisher = false;
		bool VerifyExecutableArtifacts = true;
	};

	struct PackageInspection {
		ProjectId Identity;
		std::string DisplayName;
		PackageConfiguration Configuration = PackageConfiguration::Release;
		std::uint32_t FormatVersion = 0;
		std::uint32_t RuntimeCompatibility = 0;
		std::uint64_t Revision = 0;
		std::uint64_t ContentCount = 0;
		std::uint64_t ContentBytes = 0;
		std::filesystem::path PlayerExecutable;
		PackageAudience Audience = PackageAudience::Trusted;
		std::optional<RuntimeDistributionTarget> Target;
		std::string ManifestSha256;
	};

	struct RuntimePackagePayload {
		PackageInspection Inspection;
		bool SpatialContent{};
		blockgrid::SharedAssetPins SharedGridAssets;
		std::string WorkspaceTerrainManifest;
		int WorkspaceTerrainSpacingShift{};
		std::string ProjectJson;
		std::optional<std::string> ApplicationTemplate;
		std::optional<std::string> PreRunSource;
		AssetRuntimeSnapshot Assets;
		std::optional<std::string> ContentManifestReference;
		AssetContentId ContentManifestDigest;
		std::vector<terrain::TerrainArtifact> TerrainArtifacts;
		blockgrid::SegmentMap GridSegments;
		PackageRuntimeLoadMode LoadMode = PackageRuntimeLoadMode::Complete;
		std::optional<std::string> ClientCodeJson;
		std::shared_ptr<const RuntimeDistributionCodeCatalog> RuntimeCode;
		std::string ClientCodeIdentity;
	};

	class PackageBuilder final {
	  public:
		[[nodiscard]] static GamePayload Capture(
			const Project &ProjectValue,
			const std::shared_ptr<DataModel> &World,
			std::uint64_t AuthoritativeRevision,
			std::uint64_t PersistedRevision,
			PackageAudience Audience = PackageAudience::Trusted,
			std::optional<RuntimeDistributionRequest> RuntimeDistribution = std::nullopt
		);
		[[nodiscard]] static RuntimeDistributionPlan CaptureRuntimeDistribution(
			const Project &ProjectValue, const std::shared_ptr<DataModel> &World,
			const RuntimeDistributionRequest &Request, bool RetainSource
		);
		[[nodiscard]] static PackageBuildResult Build(const PackageBuildRequest &Request);
		[[nodiscard]] static std::vector<PackageDiagnostic> Validate(const std::filesystem::path &PackageRoot,
			std::optional<PackageAudience> ExpectedAudience = std::nullopt,
			const RuntimeDistributionAdmission &Admission = {});
		[[nodiscard]] static std::optional<PackageInspection>
		Inspect(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics);
		[[nodiscard]] static std::optional<RuntimePackagePayload>
		Load(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics,
			const RuntimeDistributionAdmission &Admission = {},
			PackageRuntimeLoadMode Mode = PackageRuntimeLoadMode::Complete);
		[[nodiscard]] static std::shared_ptr<DataModel>
		LoadWorld(const RuntimePackagePayload &Payload, const std::filesystem::path &PackageRoot);
		// Code hydration source for the network client. Session admission supplies
		// authoritative terrain to a separately materialized replica world.
		[[nodiscard]] static std::shared_ptr<DataModel>
		LoadClientCodeWorld(const RuntimePackagePayload &Payload, const std::filesystem::path &PackageRoot);
		[[nodiscard]] static std::optional<ContentAvailabilityConfiguration> GetLocalContentConfiguration(
			const RuntimePackagePayload &Payload,
			const std::filesystem::path &PackageRoot,
			ContentResidencyMode Mode = ContentResidencyMode::FullyResident
		);
		[[nodiscard]] static std::size_t HydrateClientCode(
			const std::shared_ptr<DataModel> &TrustedPackageWorld, const std::shared_ptr<DataModel> &ReplicatedWorld
		);
		[[nodiscard]] static std::function<bool(const std::shared_ptr<LuaSourceContainer> &)>
		MakePlayerGuiCodeHydrator(const std::shared_ptr<DataModel> &TrustedPackageWorld);
		[[nodiscard]] static std::function<bool(const std::shared_ptr<LuaSourceContainer> &)>
		MakeDistributedPlayerGuiCodeHydrator(const RuntimePackagePayload &Payload);
		[[nodiscard]] static std::function<void(ObjectId, const std::shared_ptr<LuaSourceContainer> &, bool)>
		MakeDistributedCodeHydrator(const RuntimePackagePayload &Payload);
		[[nodiscard]] static std::size_t HydrateDistributedClientCode(
			const RuntimePackagePayload &Payload, const std::shared_ptr<DataModel> &ReplicatedWorld
		);
	};

	[[nodiscard]] std::string_view GetPackageConfigurationName(PackageConfiguration Configuration);
	[[nodiscard]] std::optional<PackageConfiguration> ParsePackageConfiguration(std::string_view Value);
	[[nodiscard]] std::string_view GetPackageAudienceName(PackageAudience Audience);
	[[nodiscard]] std::optional<PackageAudience> ParsePackageAudience(std::string_view Value);
	[[nodiscard]] std::string_view GetPackagePhaseName(PackagePhase Phase);
	[[nodiscard]] std::filesystem::path GetPackageUserDataRoot(ProjectId Identity);
}

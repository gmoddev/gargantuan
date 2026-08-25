#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	class DataModel;
	class Project;

	inline constexpr std::uint32_t GamePackageFormatVersion = 1;
	inline constexpr std::uint32_t RuntimeCompatibilityVersion = 1;

	enum class PackageConfiguration : std::uint8_t { Development, Release };
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

	struct GamePayload {
		ProjectId Identity;
		std::string DisplayName;
		std::uint64_t AuthoritativeRevision = 0;
		bool UnsavedChanges = false;
		std::string ProjectJson;
		std::optional<std::string> PreRunSource;
		AssetRuntimeSnapshot Assets;
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
	};

	struct RuntimePackagePayload {
		PackageInspection Inspection;
		std::string ProjectJson;
		std::optional<std::string> PreRunSource;
		AssetRuntimeSnapshot Assets;
	};

	class PackageBuilder final {
	  public:
		[[nodiscard]] static GamePayload Capture(
			const Project &ProjectValue,
			const std::shared_ptr<DataModel> &World,
			std::uint64_t AuthoritativeRevision,
			std::uint64_t PersistedRevision
		);
		[[nodiscard]] static PackageBuildResult Build(const PackageBuildRequest &Request);
		[[nodiscard]] static std::vector<PackageDiagnostic> Validate(const std::filesystem::path &PackageRoot);
		[[nodiscard]] static std::optional<PackageInspection>
		Inspect(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics);
		[[nodiscard]] static std::optional<RuntimePackagePayload>
		Load(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics);
		[[nodiscard]] static std::shared_ptr<DataModel>
		LoadWorld(const RuntimePackagePayload &Payload, const std::filesystem::path &PackageRoot);
	};

	[[nodiscard]] std::string_view GetPackageConfigurationName(PackageConfiguration Configuration);
	[[nodiscard]] std::optional<PackageConfiguration> ParsePackageConfiguration(std::string_view Value);
	[[nodiscard]] std::string_view GetPackagePhaseName(PackagePhase Phase);
	[[nodiscard]] std::filesystem::path GetPackageUserDataRoot(ProjectId Identity);
}

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::telemetry {
	enum class Component : std::uint32_t {
		Studio = 1,
		Engine = 2,
		EditorHost = 3,
		PackagedGame = 4,
	};

	enum class Phase : std::uint32_t {
		Unknown = 0,
		Startup = 1,
		RuntimeSchemaBootstrap = 2,
		EditorHostLoop = 3,
		ProjectOpen = 4,
		PlaySession = 5,
		EngineLoop = 6,
		Rendering = 7,
		StudioBootstrap = 8,
		StudioUi = 9,
		StudioIpc = 10,
		Shutdown = 11,
	};

	struct Consent {
		bool CrashReportsEnabled = false;
		bool PerformanceSnapshotsEnabled = false;
	};

	struct Configuration {
		Component HostComponent = Component::Engine;
		Consent CategoryConsent;
		std::filesystem::path LibraryPath;
		std::filesystem::path StorageDirectory;
		std::string ApplicationVersion = "0.0.0";
		std::string BuildId = "gargantuan";
		std::string BuildConfiguration = "unknown";
		std::string CollectorEndpoint;
		std::string RoutingKey;
		std::optional<std::array<std::uint8_t, 16>> ParentLaunchId;
	};

	enum class Availability {
		Disabled,
		Unavailable,
		Active,
		Failed,
	};

	class OptionalTelemetry final {
	  public:
		OptionalTelemetry();
		~OptionalTelemetry();
		OptionalTelemetry(OptionalTelemetry &&) noexcept;
		OptionalTelemetry &operator=(OptionalTelemetry &&) noexcept;
		OptionalTelemetry(const OptionalTelemetry &) = delete;
		OptionalTelemetry &operator=(const OptionalTelemetry &) = delete;

		[[nodiscard]] static OptionalTelemetry Load(const Configuration &Configuration);
		[[nodiscard]] static std::filesystem::path DefaultLibraryPath();
		[[nodiscard]] static std::filesystem::path DefaultStorageDirectory();
		[[nodiscard]] static std::optional<std::array<std::uint8_t, 16>> ParseLaunchId(std::string_view Text);

		[[nodiscard]] Availability GetAvailability() const noexcept;
		[[nodiscard]] std::uint32_t GetDiagnosticCount() const noexcept;
		void SetConsent(Consent CategoryConsent) noexcept;
		void SetPhase(Phase CurrentPhase) noexcept;
		void ReportControlledFatal(std::uint64_t CrashCode) noexcept;
		void ObserveFrame(std::chrono::steady_clock::duration FrameInterval) noexcept;
		void PollPerformance() noexcept;
		void Shutdown() noexcept;

	  private:
		struct Implementation;
		explicit OptionalTelemetry(std::unique_ptr<Implementation> State);
		std::unique_ptr<Implementation> State;
	};
}

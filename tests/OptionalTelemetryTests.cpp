#include "telemetry/GargantuanTelemetryAbi.hpp"
#include "telemetry/OptionalTelemetry.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
	using namespace gargantuan::telemetry;

	struct FakeState {
		std::uint32_t InitializeCount;
		std::uint32_t SetConsentCount;
		std::uint32_t CrashCount;
		std::uint32_t PerformanceCount;
		std::uint32_t ScheduleCount;
		std::uint32_t ShutdownCount;
		std::uint32_t LastCrashConsent;
		std::uint32_t LastPerformanceConsent;
		std::uint32_t LastComponent;
		std::uint32_t LastPhase;
		abi::CrashReportV1 LastCrash;
		abi::PerformanceSnapshotV1 LastPerformance;
		std::string ForwardedText;
	};

	void Check(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	void SetMode(const char *Mode) {
#if defined(_WIN32)
		_putenv_s("GARGANTUAN_TEST_TELEMETRY_MODE", Mode);
#else
		setenv("GARGANTUAN_TEST_TELEMETRY_MODE", Mode, 1);
#endif
	}

	class FakeInspector {
	  public:
		explicit FakeInspector(const std::filesystem::path &Path) {
#if defined(_WIN32)
			Handle = LoadLibraryExW(
				Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
			);
			if (!Handle) throw std::runtime_error("Could not inspect fake telemetry library");
			auto ResetSymbol = GetProcAddress(Handle, "GargantuanTelemetry_TestReset");
			auto StateSymbol = GetProcAddress(Handle, "GargantuanTelemetry_TestState");
#else
			Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
			if (!Handle) throw std::runtime_error("Could not inspect fake telemetry library");
			auto ResetSymbol = dlsym(Handle, "GargantuanTelemetry_TestReset");
			auto StateSymbol = dlsym(Handle, "GargantuanTelemetry_TestState");
#endif
			if (!ResetSymbol || !StateSymbol) throw std::runtime_error("Fake telemetry inspection export is missing");
			std::memcpy(&Reset, &ResetSymbol, sizeof(Reset));
			std::memcpy(&GetState, &StateSymbol, sizeof(GetState));
		}

		~FakeInspector() {
#if defined(_WIN32)
			if (Handle) FreeLibrary(Handle);
#else
			if (Handle) dlclose(Handle);
#endif
		}

		void Clear() const {
			Reset();
		}
		[[nodiscard]] const FakeState &State() const {
			return *static_cast<const FakeState *>(GetState());
		}

	  private:
#if defined(_WIN32)
		HMODULE Handle = nullptr;
#else
		void *Handle = nullptr;
#endif
		void (*Reset)() = nullptr;
		const void *(*GetState)() = nullptr;
	};

	Configuration
	Config(const std::filesystem::path &Library, const std::filesystem::path &Storage, Consent ConsentValue) {
		return {
			.HostComponent = Component::Engine,
			.CategoryConsent = ConsentValue,
			.LibraryPath = std::filesystem::absolute(Library),
			.StorageDirectory = std::filesystem::absolute(Storage),
			.ApplicationVersion = "1.2.3",
			.BuildId = "telemetry-test",
			.BuildConfiguration = "test",
		};
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		Check(ArgumentCount == 3, "usage: OptionalTelemetryTests <valid-library> <missing-export-library>");
		const auto ValidLibrary = std::filesystem::absolute(Arguments[1]);
		const auto MissingExportLibrary = std::filesystem::absolute(Arguments[2]);
		const auto Storage = std::filesystem::temp_directory_path() / "gargantuan-optional-telemetry-tests";
		std::error_code Error;
		std::filesystem::remove_all(Storage, Error);
		struct Cleanup {
			std::filesystem::path Path;
			~Cleanup() {
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} CleanupState{Storage};

		FakeInspector Inspector(ValidLibrary);
		SetMode("valid");
		Inspector.Clear();

		auto DisabledTelemetry = OptionalTelemetry::Load(Config(
			Storage / "not-a-library", Storage, {.CrashReportsEnabled = false, .PerformanceSnapshotsEnabled = false}
		));
		Check(
			DisabledTelemetry.GetAvailability() == Availability::Disabled,
			"all-disabled consent unexpectedly required or loaded a library"
		);

		auto MissingTelemetry = OptionalTelemetry::Load(Config(
			Storage / "absent-library", Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false}
		));
		Check(
			MissingTelemetry.GetAvailability() == Availability::Unavailable &&
				MissingTelemetry.GetDiagnosticCount() == 0,
			"an absent optional library was not a silent fail-open state"
		);

		SetMode("wrong_abi");
		auto WrongAbi = OptionalTelemetry::Load(
			Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
		);
		Check(
			WrongAbi.GetAvailability() == Availability::Failed && WrongAbi.GetDiagnosticCount() == 1,
			"an incompatible ABI did not produce one bounded diagnostic and disable telemetry"
		);

		SetMode("wrong_size");
		auto WrongSize = OptionalTelemetry::Load(
			Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
		);
		Check(WrongSize.GetAvailability() == Availability::Failed, "a short returned API table was accepted");

		auto MissingExport = OptionalTelemetry::Load(
			Config(MissingExportLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
		);
		Check(
			MissingExport.GetAvailability() == Availability::Failed,
			"a library without GargantuanTelemetry_GetApi did not fail open"
		);

		SetMode("init_failure");
		auto InitializationFailure = OptionalTelemetry::Load(
			Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
		);
		Check(
			InitializationFailure.GetAvailability() == Availability::Failed,
			"telemetry initialization failure escaped the adapter"
		);

		SetMode("valid");
		Inspector.Clear();
		{
			auto Active = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = true})
			);
			Check(
				Active.GetAvailability() == Availability::Active && Inspector.State().InitializeCount == 1,
				"valid telemetry did not negotiate and initialize exactly once"
			);
			Active.SetPhase(Phase::EngineLoop);
			Active.ReportControlledFatal(0x1234);
			Active.ObserveFrame(std::chrono::milliseconds(20));
			const auto &Observed = Inspector.State();
			Check(
				Observed.LastComponent == static_cast<std::uint32_t>(Component::Engine) && Observed.CrashCount == 1 &&
					Observed.LastCrash.CrashCode == 0x1234 && Observed.LastCrash.FrameCount == 0 &&
					Observed.PerformanceCount == 1 && Observed.LastPerformance.HasFrameMetrics == 1 &&
					Observed.LastPerformance.FrameCount == 1,
				"allowlisted crash/performance data did not cross the valid V1 table exactly"
			);
			for (const auto Forbidden :
				 {"project", "script", "username", "machine", "account", "instance", "environment"})
				Check(
					Observed.ForwardedText.find(Forbidden) == std::string::npos,
					"forbidden project/user data appeared in telemetry initialization"
				);
			Active.SetConsent({.CrashReportsEnabled = false, .PerformanceSnapshotsEnabled = false});
			const auto CrashesBeforeRevokedSubmit = Inspector.State().CrashCount;
			Active.ReportControlledFatal(0x9999);
			Check(
				Inspector.State().SetConsentCount == 1 && Inspector.State().LastCrashConsent == 0 &&
					Inspector.State().LastPerformanceConsent == 0 &&
					Inspector.State().CrashCount == CrashesBeforeRevokedSubmit,
				"consent revocation was not propagated or did not stop later collection"
			);
			for (int Index = 0; Index < 8; ++Index)
				Active.SetPhase(Phase::PlaySession);
			Check(
				Inspector.State().InitializeCount == 1,
				"repeated Play/Stop-style phase changes accumulated telemetry lifecycles"
			);
		}

		Inspector.Clear();
		{
			auto CrashOnly = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
			);
			CrashOnly.ObserveFrame(std::chrono::milliseconds(17));
			Check(Inspector.State().PerformanceCount == 0, "crash-only consent submitted performance data");
		}

		Inspector.Clear();
		SetMode("future_schedule");
		{
			auto FutureSchedule = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = false, .PerformanceSnapshotsEnabled = true})
			);
			for (int Index = 0; Index < 1'000; ++Index)
				FutureSchedule.ObserveFrame(std::chrono::milliseconds(16));
			Check(
				Inspector.State().ScheduleCount == 1 && Inspector.State().PerformanceCount == 0,
				"the per-frame hot path crossed the telemetry ABI before the cached schedule was due"
			);
		}

		Inspector.Clear();
		SetMode("valid");
		{
			auto PerformanceOnly = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = false, .PerformanceSnapshotsEnabled = true})
			);
			PerformanceOnly.ReportControlledFatal(7);
			Check(Inspector.State().CrashCount == 0, "performance-only consent submitted a crash report");
		}

		SetMode("performance_failure");
		{
			auto RuntimeFailure = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = false, .PerformanceSnapshotsEnabled = true})
			);
			RuntimeFailure.ObserveFrame(std::chrono::milliseconds(16));
			Check(
				RuntimeFailure.GetAvailability() == Availability::Failed,
				"runtime submit failure did not degrade telemetry alone to no-op"
			);
		}

		SetMode("shutdown_failure");
		{
			auto ShutdownFailure = OptionalTelemetry::Load(
				Config(ValidLibrary, Storage, {.CrashReportsEnabled = true, .PerformanceSnapshotsEnabled = false})
			);
			ShutdownFailure.Shutdown();
			Check(
				ShutdownFailure.GetAvailability() == Availability::Disabled,
				"shutdown failure prevented clean host-side shutdown"
			);
		}

		Check(
			OptionalTelemetry::ParseLaunchId("00112233-4455-6677-8899-aabbccddeeff").has_value() &&
				!OptionalTelemetry::ParseLaunchId("project-name").has_value(),
			"launch correlation ID parsing accepted non-random arbitrary text"
		);
		std::cout << "Optional telemetry host tests passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "Optional telemetry host tests failed: " << Error.what() << '\n';
		return 1;
	}
}

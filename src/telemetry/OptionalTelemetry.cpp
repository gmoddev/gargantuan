#include "telemetry/OptionalTelemetry.hpp"

#include "telemetry/GargantuanTelemetryAbi.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace gargantuan::telemetry {
	namespace {
		constexpr std::array<std::uint64_t, abi::HistogramBuckets> HistogramUpperBounds{
			8'333,
			16'667,
			25'000,
			33'333,
			50'000,
			100'000,
			250'000,
			1'000'000,
		};
		constexpr std::uint64_t SlowFrameThresholdMicroseconds = 50'000;

		struct NativeLibrary {
#if defined(_WIN32)
			HMODULE Handle = nullptr;
#else
			void *Handle = nullptr;
#endif
		};

		[[nodiscard]] NativeLibrary OpenLibrary(const std::filesystem::path &Path) noexcept {
			NativeLibrary Result;
#if defined(_WIN32)
			Result.Handle = LoadLibraryExW(
				Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
			);
#else
			Result.Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
			return Result;
		}

		void CloseLibrary(NativeLibrary Library) noexcept {
			if (!Library.Handle) return;
#if defined(_WIN32)
			FreeLibrary(Library.Handle);
#else
			dlclose(Library.Handle);
#endif
		}

		[[nodiscard]] abi::GetApiFn ResolveGetApi(NativeLibrary Library) noexcept {
			abi::GetApiFn Result = nullptr;
#if defined(_WIN32)
			auto Symbol = GetProcAddress(Library.Handle, "GargantuanTelemetry_GetApi");
#else
			auto Symbol = dlsym(Library.Handle, "GargantuanTelemetry_GetApi");
#endif
			if (Symbol) static_assert(sizeof(Result) == sizeof(Symbol));
			if (Symbol) std::memcpy(&Result, &Symbol, sizeof(Result));
			return Result;
		}

		[[nodiscard]] abi::Utf8Span Span(const std::string &Value) noexcept {
			return {
				reinterpret_cast<const std::uint8_t *>(Value.data()),
				static_cast<std::uint32_t>(Value.size()),
			};
		}

		[[nodiscard]] std::array<std::uint8_t, 16> RandomLaunchId() {
			std::array<std::uint8_t, 16> Result{};
			std::random_device Random;
			for (auto &Byte : Result)
				Byte = static_cast<std::uint8_t>(Random());
			Result[6] = static_cast<std::uint8_t>((Result[6] & 0x0f) | 0x40);
			Result[8] = static_cast<std::uint8_t>((Result[8] & 0x3f) | 0x80);
			return Result;
		}

		[[nodiscard]] bool IsApiComplete(const abi::ApiV1 &Api) noexcept {
			return Api.StructSize >= sizeof(abi::ApiV1) && Api.AbiVersion == abi::V1 && Api.Initialize &&
				   Api.SetConsent && Api.SetPhase && Api.ReportCrash && Api.GetPerformanceSchedule &&
				   Api.SubmitPerformanceSnapshot && Api.FlushBestEffort && Api.Shutdown;
		}

		[[nodiscard]] std::uint64_t SaturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
			return Left > std::numeric_limits<std::uint64_t>::max() - Right ? std::numeric_limits<std::uint64_t>::max()
																			: Left + Right;
		}

		[[nodiscard]] std::filesystem::path ExecutablePath() {
#if defined(_WIN32)
			std::wstring Buffer(32'768, L'\0');
			const auto Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
			if (Length == 0 || Length >= Buffer.size()) return {};
			Buffer.resize(Length);
			return std::filesystem::path(Buffer);
#elif defined(__APPLE__)
			std::uint32_t Size = 0;
			_NSGetExecutablePath(nullptr, &Size);
			std::string Buffer(Size, '\0');
			if (_NSGetExecutablePath(Buffer.data(), &Size) != 0) return {};
			Buffer.resize(std::strlen(Buffer.c_str()));
			return std::filesystem::weakly_canonical(Buffer);
#else
			std::error_code Error;
			return std::filesystem::read_symlink("/proc/self/exe", Error);
#endif
		}
	}

	struct OptionalTelemetry::Implementation {
		Availability CurrentAvailability = Availability::Unavailable;
		NativeLibrary Library;
		abi::ApiV1 Api{};
		Consent CategoryConsent;
		Phase CurrentPhase = Phase::Unknown;
		std::chrono::steady_clock::time_point StartedAt = std::chrono::steady_clock::now();
		std::uint64_t NextDueUptimeMilliseconds = std::numeric_limits<std::uint64_t>::max();
		std::uint64_t FrameCount = 0;
		std::uint64_t TotalFrameMicroseconds = 0;
		std::uint64_t MaximumFrameMicroseconds = 0;
		std::uint64_t SlowFrameCount = 0;
		std::array<std::uint64_t, abi::HistogramBuckets> Histogram{};
		std::uint32_t DiagnosticCount = 0;
		bool Initialized = false;
		bool ShutdownRequested = false;

		void DiagnoseOnce() noexcept {
			if (DiagnosticCount != 0) return;
			++DiagnosticCount;
			std::cerr << "[Telemetry:Host] Optional telemetry is incompatible or unhealthy; telemetry is disabled.\n";
		}

		void Fail() noexcept {
			CurrentAvailability = Availability::Failed;
			DiagnoseOnce();
		}

		[[nodiscard]] std::uint64_t UptimeMilliseconds() const noexcept {
			const auto Elapsed = std::chrono::steady_clock::now() - StartedAt;
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed).count());
		}

		void RefreshSchedule() noexcept {
			if (CurrentAvailability != Availability::Active || !CategoryConsent.PerformanceSnapshotsEnabled) {
				NextDueUptimeMilliseconds = std::numeric_limits<std::uint64_t>::max();
				return;
			}
			abi::PerformanceScheduleV1 Schedule{
				.StructSize = sizeof(abi::PerformanceScheduleV1),
				.AbiVersion = abi::V1,
			};
			try {
				const auto Status = Api.GetPerformanceSchedule(&Schedule);
				if (Status == abi::Ok)
					NextDueUptimeMilliseconds = Schedule.NextDueUptimeMilliseconds;
				else if (Status == abi::Disabled)
					NextDueUptimeMilliseconds = std::numeric_limits<std::uint64_t>::max();
				else
					Fail();
			} catch (...) {
				Fail();
			}
		}

		[[nodiscard]] std::uint64_t Percentile(std::uint64_t Numerator) const noexcept {
			if (FrameCount == 0) return 0;
			const auto Target = std::max<std::uint64_t>(1, (FrameCount * Numerator + 99) / 100);
			std::uint64_t Seen = 0;
			for (std::size_t Index = 0; Index < Histogram.size(); ++Index) {
				Seen = SaturatingAdd(Seen, Histogram[Index]);
				if (Seen >= Target) return HistogramUpperBounds[Index];
			}
			return HistogramUpperBounds.back();
		}

		void ResetFrames() noexcept {
			FrameCount = 0;
			TotalFrameMicroseconds = 0;
			MaximumFrameMicroseconds = 0;
			SlowFrameCount = 0;
			Histogram.fill(0);
		}
	};

	OptionalTelemetry::OptionalTelemetry() = default;
	OptionalTelemetry::OptionalTelemetry(std::unique_ptr<Implementation> State) : State(std::move(State)) {}
	OptionalTelemetry::~OptionalTelemetry() {
		Shutdown();
	}
	OptionalTelemetry::OptionalTelemetry(OptionalTelemetry &&) noexcept = default;
	OptionalTelemetry &OptionalTelemetry::operator=(OptionalTelemetry &&) noexcept = default;

	OptionalTelemetry OptionalTelemetry::Load(const Configuration &Configuration) {
		auto State = std::make_unique<Implementation>();
		State->CategoryConsent = Configuration.CategoryConsent;
		if (!Configuration.CategoryConsent.CrashReportsEnabled &&
			!Configuration.CategoryConsent.PerformanceSnapshotsEnabled) {
			State->CurrentAvailability = Availability::Disabled;
			return OptionalTelemetry(std::move(State));
		}
		if (!Configuration.LibraryPath.is_absolute() || !Configuration.StorageDirectory.is_absolute() ||
			Configuration.ApplicationVersion.empty() || Configuration.BuildId.empty() ||
			Configuration.BuildConfiguration.empty()) {
			State->Fail();
			return OptionalTelemetry(std::move(State));
		}

		State->Library = OpenLibrary(Configuration.LibraryPath);
		if (!State->Library.Handle) return OptionalTelemetry(std::move(State));
		const auto GetApi = ResolveGetApi(State->Library);
		if (!GetApi) {
			State->Fail();
			CloseLibrary(State->Library);
			State->Library = {};
			return OptionalTelemetry(std::move(State));
		}

		try {
			const auto Status = GetApi(abi::V1, sizeof(State->Api), &State->Api);
			if (Status != abi::Ok || !IsApiComplete(State->Api)) {
				State->Fail();
				CloseLibrary(State->Library);
				State->Library = {};
				return OptionalTelemetry(std::move(State));
			}
		} catch (...) {
			State->Fail();
			CloseLibrary(State->Library);
			State->Library = {};
			return OptionalTelemetry(std::move(State));
		}

		std::error_code DirectoryError;
		std::filesystem::create_directories(Configuration.StorageDirectory, DirectoryError);
		if (DirectoryError) {
			State->Fail();
			CloseLibrary(State->Library);
			State->Library = {};
			return OptionalTelemetry(std::move(State));
		}

		const auto LaunchId = RandomLaunchId();
		const auto Storage = Configuration.StorageDirectory.u8string();
		const std::string StorageUtf8(Storage.begin(), Storage.end());
		abi::InitializeV1 Initialize{
			.StructSize = sizeof(abi::InitializeV1),
			.AbiVersion = abi::V1,
			.Component = static_cast<std::uint32_t>(Configuration.HostComponent),
			.CrashReportsEnabled = Configuration.CategoryConsent.CrashReportsEnabled ? 1u : 0u,
			.PerformanceSnapshotsEnabled = Configuration.CategoryConsent.PerformanceSnapshotsEnabled ? 1u : 0u,
			.HasParentLaunchId = Configuration.ParentLaunchId ? 1u : 0u,
			.ApplicationVersion = Span(Configuration.ApplicationVersion),
			.BuildId = Span(Configuration.BuildId),
			.BuildConfiguration = Span(Configuration.BuildConfiguration),
			.StorageDirectory = Span(StorageUtf8),
			.CollectorEndpoint = Span(Configuration.CollectorEndpoint),
			.RoutingKey = Span(Configuration.RoutingKey),
		};
		std::ranges::copy(LaunchId, Initialize.LaunchId);
		if (Configuration.ParentLaunchId) std::ranges::copy(*Configuration.ParentLaunchId, Initialize.ParentLaunchId);
		try {
			const auto Status = State->Api.Initialize(&Initialize);
			if (Status != abi::Ok) {
				State->Fail();
				CloseLibrary(State->Library);
				State->Library = {};
				return OptionalTelemetry(std::move(State));
			}
		} catch (...) {
			State->Fail();
			CloseLibrary(State->Library);
			State->Library = {};
			return OptionalTelemetry(std::move(State));
		}

		State->Initialized = true;
		State->CurrentAvailability = Availability::Active;
		State->RefreshSchedule();
		return OptionalTelemetry(std::move(State));
	}

	std::filesystem::path OptionalTelemetry::DefaultLibraryPath() {
		const auto Executable = ExecutablePath();
		if (Executable.empty()) return {};
#if defined(_WIN32)
		return Executable.parent_path() / "gargantuan_telemetry.dll";
#elif defined(__APPLE__)
		return Executable.parent_path() / "libgargantuan_telemetry.dylib";
#else
		return Executable.parent_path() / "libgargantuan_telemetry.so";
#endif
	}

	std::filesystem::path OptionalTelemetry::DefaultStorageDirectory() {
#if defined(_WIN32)
		if (const auto *LocalApplicationData = _wgetenv(L"LOCALAPPDATA"))
			return std::filesystem::absolute(LocalApplicationData) / "Gargantuan" / "Telemetry";
#elif defined(__APPLE__)
		if (const auto *Home = std::getenv("HOME"))
			return std::filesystem::absolute(Home) / "Library" / "Application Support" / "Gargantuan" / "Telemetry";
#else
		if (const auto *StateHome = std::getenv("XDG_STATE_HOME"))
			return std::filesystem::absolute(StateHome) / "gargantuan" / "telemetry";
		if (const auto *Home = std::getenv("HOME"))
			return std::filesystem::absolute(Home) / ".local" / "state" / "gargantuan" / "telemetry";
#endif
		return std::filesystem::temp_directory_path() / "Gargantuan" / "Telemetry";
	}

	std::optional<std::array<std::uint8_t, 16>> OptionalTelemetry::ParseLaunchId(std::string_view Text) {
		std::array<std::uint8_t, 16> Result{};
		std::size_t Output = 0;
		int High = -1;
		for (const char Character : Text) {
			if (Character == '-') continue;
			int Value = -1;
			if (Character >= '0' && Character <= '9')
				Value = Character - '0';
			else if (Character >= 'a' && Character <= 'f')
				Value = Character - 'a' + 10;
			else if (Character >= 'A' && Character <= 'F')
				Value = Character - 'A' + 10;
			else
				return std::nullopt;
			if (High < 0)
				High = Value;
			else {
				if (Output >= Result.size()) return std::nullopt;
				Result[Output++] = static_cast<std::uint8_t>((High << 4) | Value);
				High = -1;
			}
		}
		if (High >= 0 || Output != Result.size() || std::ranges::all_of(Result, [](auto Byte) { return Byte == 0; }))
			return std::nullopt;
		return Result;
	}

	Availability OptionalTelemetry::GetAvailability() const noexcept {
		return State ? State->CurrentAvailability : Availability::Disabled;
	}

	std::uint32_t OptionalTelemetry::GetDiagnosticCount() const noexcept {
		return State ? State->DiagnosticCount : 0;
	}

	void OptionalTelemetry::SetConsent(Consent CategoryConsent) noexcept {
		if (!State || State->CurrentAvailability != Availability::Active) return;
		try {
			const auto Status = State->Api.SetConsent(
				CategoryConsent.CrashReportsEnabled ? 1u : 0u, CategoryConsent.PerformanceSnapshotsEnabled ? 1u : 0u
			);
			if (Status != abi::Ok) {
				State->Fail();
				return;
			}
			State->CategoryConsent = CategoryConsent;
			if (!CategoryConsent.PerformanceSnapshotsEnabled) State->ResetFrames();
			State->RefreshSchedule();
		} catch (...) {
			State->Fail();
		}
	}

	void OptionalTelemetry::SetPhase(Phase CurrentPhase) noexcept {
		if (!State || State->CurrentAvailability != Availability::Active) return;
		try {
			if (State->Api.SetPhase(static_cast<std::uint32_t>(CurrentPhase)) != abi::Ok) {
				State->Fail();
				return;
			}
			State->CurrentPhase = CurrentPhase;
		} catch (...) {
			State->Fail();
		}
	}

	void OptionalTelemetry::ReportControlledFatal(std::uint64_t CrashCode) noexcept {
		if (!State || State->CurrentAvailability != Availability::Active || !State->CategoryConsent.CrashReportsEnabled)
			return;
		abi::CrashReportV1 Report{
			.StructSize = sizeof(abi::CrashReportV1),
			.AbiVersion = abi::V1,
			.CrashKind = 1,
			.CurrentPhase = static_cast<std::uint32_t>(State->CurrentPhase),
			.CrashCode = CrashCode,
			.ProcessUptimeMilliseconds = State->UptimeMilliseconds(),
		};
		try {
			const auto Status = State->Api.ReportCrash(&Report);
			if (Status != abi::Ok && Status != abi::Disabled && Status != abi::StorageUnavailable) State->Fail();
		} catch (...) {
			State->Fail();
		}
	}

	void OptionalTelemetry::ObserveFrame(std::chrono::steady_clock::duration FrameInterval) noexcept {
		if (!State || State->CurrentAvailability != Availability::Active ||
			!State->CategoryConsent.PerformanceSnapshotsEnabled)
			return;
		const auto SignedMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(FrameInterval).count();
		const auto Microseconds = SignedMicroseconds > 0 ? static_cast<std::uint64_t>(SignedMicroseconds) : 0;
		State->FrameCount = SaturatingAdd(State->FrameCount, 1);
		State->TotalFrameMicroseconds = SaturatingAdd(State->TotalFrameMicroseconds, Microseconds);
		State->MaximumFrameMicroseconds = std::max(State->MaximumFrameMicroseconds, Microseconds);
		if (Microseconds > SlowFrameThresholdMicroseconds)
			State->SlowFrameCount = SaturatingAdd(State->SlowFrameCount, 1);
		const auto Bucket = std::ranges::find_if(HistogramUpperBounds, [&](auto Bound) {
			return Microseconds <= Bound;
		});
		const auto Index = Bucket == HistogramUpperBounds.end()
							   ? HistogramUpperBounds.size() - 1
							   : static_cast<std::size_t>(std::distance(HistogramUpperBounds.begin(), Bucket));
		State->Histogram[Index] = SaturatingAdd(State->Histogram[Index], 1);
		PollPerformance();
	}

	void OptionalTelemetry::PollPerformance() noexcept {
		if (!State || State->CurrentAvailability != Availability::Active ||
			!State->CategoryConsent.PerformanceSnapshotsEnabled ||
			State->UptimeMilliseconds() < State->NextDueUptimeMilliseconds)
			return;
		abi::PerformanceSnapshotV1 Snapshot{
			.StructSize = sizeof(abi::PerformanceSnapshotV1),
			.AbiVersion = abi::V1,
			.HasFrameMetrics = State->FrameCount > 0 ? 1u : 0u,
			.FrameCount = State->FrameCount,
			.AverageFrameIntervalMicroseconds = State->FrameCount == 0
													? 0
													: State->TotalFrameMicroseconds / State->FrameCount,
			.ApproximateP50Microseconds = State->Percentile(50),
			.ApproximateP95Microseconds = State->Percentile(95),
			.MaximumFrameIntervalMicroseconds = State->MaximumFrameMicroseconds,
			.SlowFrameCount = State->SlowFrameCount,
		};
		std::ranges::copy(State->Histogram, Snapshot.FrameTimeHistogram);
		try {
			const auto Status = State->Api.SubmitPerformanceSnapshot(&Snapshot);
			if (Status == abi::Ok)
				State->ResetFrames();
			else if (Status != abi::NotDue && Status != abi::QueueFull && Status != abi::Disabled) {
				State->Fail();
				return;
			}
			State->RefreshSchedule();
		} catch (...) {
			State->Fail();
		}
	}

	void OptionalTelemetry::Shutdown() noexcept {
		if (!State || State->ShutdownRequested) return;
		State->ShutdownRequested = true;
		if (!State->Initialized) return;
		try {
			(void)State->Api.FlushBestEffort(100);
			(void)State->Api.SetPhase(static_cast<std::uint32_t>(Phase::Shutdown));
			if (State->Api.Shutdown() != abi::Ok) State->DiagnoseOnce();
		} catch (...) {
			State->DiagnoseOnce();
		}
		State->CurrentAvailability = Availability::Disabled;
		State->Initialized = false;
		// V1 stays mapped until process exit. This prevents callbacks or a late native
		// shutdown path from observing unloaded code.
	}
}

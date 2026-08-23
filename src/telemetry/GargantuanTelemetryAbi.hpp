#pragma once

#include <cstdint>

#if defined(_WIN32)
#define GARGANTUAN_TELEMETRY_CALL __cdecl
#else
#define GARGANTUAN_TELEMETRY_CALL
#endif

namespace gargantuan::telemetry::abi {
	inline constexpr std::uint32_t V1 = 1;
	inline constexpr std::uint32_t HistogramBuckets = 8;
	inline constexpr std::uint32_t FrameCapacity = 32;

	using Status = std::uint32_t;
	inline constexpr Status Ok = 0;
	inline constexpr Status Disabled = 1;
	inline constexpr Status NotInitialized = 2;
	inline constexpr Status AlreadyInitialized = 3;
	inline constexpr Status InvalidArgument = 4;
	inline constexpr Status UnsupportedAbi = 5;
	inline constexpr Status NotDue = 6;
	inline constexpr Status QueueFull = 7;
	inline constexpr Status StorageUnavailable = 8;
	inline constexpr Status NetworkUnavailable = 9;
	inline constexpr Status InternalError = 10;
	inline constexpr Status ShuttingDown = 11;

	struct Utf8Span {
		const std::uint8_t *Ptr;
		std::uint32_t Length;
	};

	struct InitializeV1 {
		std::uint32_t StructSize;
		std::uint32_t AbiVersion;
		std::uint32_t Component;
		std::uint32_t CrashReportsEnabled;
		std::uint32_t PerformanceSnapshotsEnabled;
		std::uint32_t HasParentLaunchId;
		std::uint8_t LaunchId[16];
		std::uint8_t ParentLaunchId[16];
		Utf8Span ApplicationVersion;
		Utf8Span BuildId;
		Utf8Span BuildConfiguration;
		Utf8Span StorageDirectory;
		Utf8Span CollectorEndpoint;
		Utf8Span RoutingKey;
		std::uint32_t AllowInsecureLoopback;
		std::uint32_t Reserved;
	};

	struct FrameV1 {
		std::uint8_t ModuleIdentifier[16];
		std::uint64_t ModuleRelativeOffset;
	};

	struct CrashReportV1 {
		std::uint32_t StructSize;
		std::uint32_t AbiVersion;
		std::uint32_t CrashKind;
		std::uint32_t CurrentPhase;
		std::uint64_t CrashCode;
		std::uint64_t ProcessUptimeMilliseconds;
		std::uint32_t FrameCount;
		std::uint32_t Reserved;
		FrameV1 Frames[FrameCapacity];
	};

	struct PerformanceSnapshotV1 {
		std::uint32_t StructSize;
		std::uint32_t AbiVersion;
		std::uint32_t HasFrameMetrics;
		std::uint32_t Reserved;
		std::uint64_t FrameCount;
		std::uint64_t AverageFrameIntervalMicroseconds;
		std::uint64_t ApproximateP50Microseconds;
		std::uint64_t ApproximateP95Microseconds;
		std::uint64_t MaximumFrameIntervalMicroseconds;
		std::uint64_t SlowFrameCount;
		std::uint64_t FrameTimeHistogram[HistogramBuckets];
	};

	struct PerformanceScheduleV1 {
		std::uint32_t StructSize;
		std::uint32_t AbiVersion;
		std::uint64_t NextDueUptimeMilliseconds;
		std::uint32_t SubmittedCount;
		std::uint32_t MaximumCount;
	};

	using InitializeFn = Status(GARGANTUAN_TELEMETRY_CALL *)(const InitializeV1 *);
	using SetConsentFn = Status(GARGANTUAN_TELEMETRY_CALL *)(std::uint32_t, std::uint32_t);
	using SetPhaseFn = Status(GARGANTUAN_TELEMETRY_CALL *)(std::uint32_t);
	using ReportCrashFn = Status(GARGANTUAN_TELEMETRY_CALL *)(const CrashReportV1 *);
	using GetPerformanceScheduleFn = Status(GARGANTUAN_TELEMETRY_CALL *)(PerformanceScheduleV1 *);
	using SubmitPerformanceSnapshotFn = Status(GARGANTUAN_TELEMETRY_CALL *)(const PerformanceSnapshotV1 *);
	using FlushBestEffortFn = Status(GARGANTUAN_TELEMETRY_CALL *)(std::uint32_t);
	using ShutdownFn = Status(GARGANTUAN_TELEMETRY_CALL *)();

	struct ApiV1 {
		std::uint32_t StructSize;
		std::uint32_t AbiVersion;
		InitializeFn Initialize;
		SetConsentFn SetConsent;
		SetPhaseFn SetPhase;
		ReportCrashFn ReportCrash;
		GetPerformanceScheduleFn GetPerformanceSchedule;
		SubmitPerformanceSnapshotFn SubmitPerformanceSnapshot;
		FlushBestEffortFn FlushBestEffort;
		ShutdownFn Shutdown;
	};

	using GetApiFn = Status(GARGANTUAN_TELEMETRY_CALL *)(std::uint32_t, std::uint32_t, ApiV1 *);
}

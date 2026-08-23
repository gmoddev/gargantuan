#include "telemetry/GargantuanTelemetryAbi.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
	using namespace gargantuan::telemetry;

	struct TestState {
		std::uint32_t InitializeCount = 0;
		std::uint32_t SetConsentCount = 0;
		std::uint32_t CrashCount = 0;
		std::uint32_t PerformanceCount = 0;
		std::uint32_t ScheduleCount = 0;
		std::uint32_t ShutdownCount = 0;
		std::uint32_t LastCrashConsent = 0;
		std::uint32_t LastPerformanceConsent = 0;
		std::uint32_t LastComponent = 0;
		std::uint32_t LastPhase = 0;
		abi::CrashReportV1 LastCrash{};
		abi::PerformanceSnapshotV1 LastPerformance{};
		std::string ForwardedText;
	};

	TestState State;

	[[nodiscard]] std::string_view Mode() {
		const auto *Value = std::getenv("GARGANTUAN_TEST_TELEMETRY_MODE");
		return Value ? std::string_view(Value) : std::string_view("valid");
	}

	[[nodiscard]] std::string Text(abi::Utf8Span Span) {
		if (!Span.Ptr || Span.Length == 0) return {};
		return std::string(reinterpret_cast<const char *>(Span.Ptr), Span.Length);
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL Initialize(const abi::InitializeV1 *Value) {
		if (Mode() == "init_failure") return abi::InternalError;
		if (!Value) return abi::InvalidArgument;
		++State.InitializeCount;
		State.LastCrashConsent = Value->CrashReportsEnabled;
		State.LastPerformanceConsent = Value->PerformanceSnapshotsEnabled;
		State.LastComponent = Value->Component;
		State.ForwardedText = Text(Value->ApplicationVersion) + "|" + Text(Value->BuildId) + "|" +
							  Text(Value->BuildConfiguration) + "|" + Text(Value->StorageDirectory) + "|" +
							  Text(Value->CollectorEndpoint) + "|" + Text(Value->RoutingKey);
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL SetConsent(std::uint32_t Crash, std::uint32_t Performance) {
		++State.SetConsentCount;
		State.LastCrashConsent = Crash;
		State.LastPerformanceConsent = Performance;
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL SetPhase(std::uint32_t Value) {
		State.LastPhase = Value;
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL ReportCrash(const abi::CrashReportV1 *Value) {
		if (Mode() == "crash_failure") return abi::InternalError;
		if (!Value) return abi::InvalidArgument;
		++State.CrashCount;
		State.LastCrash = *Value;
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL GetSchedule(abi::PerformanceScheduleV1 *Value) {
		if (!Value) return abi::InvalidArgument;
		++State.ScheduleCount;
		Value->NextDueUptimeMilliseconds = Mode() == "future_schedule" ? 600'000 : 0;
		Value->SubmittedCount = State.PerformanceCount;
		Value->MaximumCount = 3;
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL SubmitPerformance(const abi::PerformanceSnapshotV1 *Value) {
		if (Mode() == "performance_failure") return abi::InternalError;
		if (!Value) return abi::InvalidArgument;
		++State.PerformanceCount;
		State.LastPerformance = *Value;
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL Flush(std::uint32_t) {
		return abi::Ok;
	}

	abi::Status GARGANTUAN_TELEMETRY_CALL Shutdown() {
		++State.ShutdownCount;
		return Mode() == "shutdown_failure" ? abi::InternalError : abi::Ok;
	}
}

TEST_EXPORT gargantuan::telemetry::abi::Status GARGANTUAN_TELEMETRY_CALL GargantuanTelemetry_GetApi(
	std::uint32_t RequestedAbi, std::uint32_t ApiStructSize, gargantuan::telemetry::abi::ApiV1 *Api
) {
	using namespace gargantuan::telemetry;
	if (Mode() == "wrong_abi" || RequestedAbi != abi::V1) return abi::UnsupportedAbi;
	if (!Api || ApiStructSize < sizeof(*Api)) return abi::InvalidArgument;
	*Api = {
		.StructSize = Mode() == "wrong_size" ? 8u : static_cast<std::uint32_t>(sizeof(*Api)),
		.AbiVersion = abi::V1,
		.Initialize = Initialize,
		.SetConsent = SetConsent,
		.SetPhase = SetPhase,
		.ReportCrash = ReportCrash,
		.GetPerformanceSchedule = GetSchedule,
		.SubmitPerformanceSnapshot = SubmitPerformance,
		.FlushBestEffort = Flush,
		.Shutdown = Shutdown,
	};
	return abi::Ok;
}

TEST_EXPORT void GargantuanTelemetry_TestReset() {
	State = {};
}
TEST_EXPORT const void *GargantuanTelemetry_TestState() {
	return &State;
}

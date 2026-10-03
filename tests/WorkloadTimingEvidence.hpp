#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gargantuan::test_detail {

[[nodiscard]] inline std::uint64_t WorkloadTimestamp(std::chrono::steady_clock::time_point Value) noexcept {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Value.time_since_epoch()).count());
}

[[nodiscard]] inline std::uint64_t WorkloadNativeThread() noexcept {
#if defined(_WIN32)
	return GetCurrentThreadId();
#else
	return 0; // Explicitly unmeasured, never a fabricated native thread identity.
#endif
}

struct WorkloadClockAnchor {
	std::uint64_t Process = 0, Thread = 0, SteadyNs = 0, QpcBefore = 0, QpcAfter = 0, QpcFrequency = 0;
	bool NativeValid = false;
	[[nodiscard]] static WorkloadClockAnchor Capture() noexcept {
		WorkloadClockAnchor Value;
#if defined(_WIN32)
		LARGE_INTEGER Before{}, After{}, Frequency{};
		const bool BeforeValid = QueryPerformanceCounter(&Before) != 0;
#endif
		Value.SteadyNs = WorkloadTimestamp(std::chrono::steady_clock::now());
#if defined(_WIN32)
		const bool AfterValid = QueryPerformanceCounter(&After) != 0;
		Value.Process = GetCurrentProcessId(); Value.Thread = WorkloadNativeThread();
		Value.NativeValid = BeforeValid && AfterValid && QueryPerformanceFrequency(&Frequency) &&
			Before.QuadPart >= 0 && After.QuadPart >= Before.QuadPart && Frequency.QuadPart > 0;
		if (Value.NativeValid) {
			Value.QpcBefore = static_cast<std::uint64_t>(Before.QuadPart);
			Value.QpcAfter = static_cast<std::uint64_t>(After.QuadPart);
			Value.QpcFrequency = static_cast<std::uint64_t>(Frequency.QuadPart);
		}
#endif
		return Value;
	}
	void Print(std::ostream &Output, std::string_view Case, std::string_view Profile, std::string_view Boundary) const {
		Output << "[Qualification:ClockAnchor] case=" << Case << " profile=" << Profile << " boundary=" << Boundary
			<< " pid=" << Process << " native_tid=" << Thread << " native_valid=" << NativeValid
			<< " steady_ns=" << SteadyNs << " qpc_before=" << QpcBefore << " qpc_after=" << QpcAfter
			<< " qpc_frequency=" << QpcFrequency << '\n';
	}
};

// Fixture-only counters. CPU readings are diagnostics, never a latency clock.
struct WorkloadCpuSample {
	std::uint64_t Thread100ns = 0, Process100ns = 0;
	bool ThreadValid = false, ProcessValid = false;
};

[[nodiscard]] inline WorkloadCpuSample CaptureWorkloadCpu() noexcept {
	WorkloadCpuSample Result;
#if defined(_WIN32)
	const auto Value = [](const FILETIME &Time) noexcept {
		return (static_cast<std::uint64_t>(Time.dwHighDateTime) << 32) | Time.dwLowDateTime;
	};
	FILETIME Created{}, Exited{}, Kernel{}, User{};
	if (GetThreadTimes(GetCurrentThread(), &Created, &Exited, &Kernel, &User)) {
		Result.Thread100ns = Value(Kernel) + Value(User);
		Result.ThreadValid = true;
	}
	if (GetProcessTimes(GetCurrentProcess(), &Created, &Exited, &Kernel, &User)) {
		Result.Process100ns = Value(Kernel) + Value(User);
		Result.ProcessValid = true;
	}
#endif
	return Result;
}

enum class WorkloadPhase : std::size_t {
	StepInterval, ClientRuntime, ServerRuntime, ServerPoll, ClientPoll,
	ServerSession, ClientSession, Observer, Sleep, Count
};

struct WorkloadSpanEvidence {
	bool Observed = false;
	std::uint64_t Step = 0;
	std::uint64_t StartNs = 0, EndNs = 0, NativeThread = 0;
	double WallMs = 0, RequestedSleepMs = 0, ActualSleepMs = 0;
	std::optional<double> ThreadCpuMs, ProcessCpuMs;
};

struct WorkloadTimingEvidence {
	std::array<WorkloadSpanEvidence, static_cast<std::size_t>(WorkloadPhase::Count)> Maxima{};

	[[nodiscard]] static std::optional<double> CpuDelta(std::uint64_t Before, std::uint64_t After,
		bool BeforeValid, bool AfterValid) noexcept {
		if (!BeforeValid || !AfterValid || After < Before) return {};
		return static_cast<double>(After - Before) / 10'000.0;
	}

	void Record(WorkloadPhase Phase, std::uint64_t Step, double WallMs,
		const WorkloadCpuSample &Before, const WorkloadCpuSample &After,
		double RequestedSleepMs = 0, double ActualSleepMs = 0,
		std::uint64_t StartNs = 0, std::uint64_t EndNs = 0) noexcept {
		auto &Maximum = Maxima[static_cast<std::size_t>(Phase)];
		if (Maximum.Observed && Maximum.WallMs >= WallMs) return;
		// Keep CPU deltas from this exact wall-time span; independent maxima
		// cannot establish whether the slow span was actually on CPU.
		Maximum = {.Observed = true, .Step = Step, .StartNs = StartNs, .EndNs = EndNs,
			.NativeThread = WorkloadNativeThread(), .WallMs = WallMs,
			.RequestedSleepMs = RequestedSleepMs, .ActualSleepMs = ActualSleepMs,
			.ThreadCpuMs = CpuDelta(Before.Thread100ns, After.Thread100ns, Before.ThreadValid, After.ThreadValid),
			.ProcessCpuMs = CpuDelta(Before.Process100ns, After.Process100ns, Before.ProcessValid, After.ProcessValid)};
	}

	void Print(std::ostream &Output, std::string_view Case, std::string_view Profile = "NOT_MEASURED") const {
		constexpr std::array Names{"step_interval", "client_runtime", "server_runtime", "server_poll",
			"client_poll", "server_session", "client_session", "observer", "sleep"};
		static_assert(Names.size() == static_cast<std::size_t>(WorkloadPhase::Count));
		for (std::size_t Index = 0; Index < Maxima.size(); ++Index) {
			const auto &Value = Maxima[Index];
			if (!Value.Observed) continue;
			Output << "[Qualification:WorkloadCpu] case=" << Case << " phase=" << Names[Index]
				<< " step=" << Value.Step << " wall_ms=" << Value.WallMs << " thread_cpu_ms=";
			if (Value.ThreadCpuMs) Output << *Value.ThreadCpuMs; else Output << "NOT_MEASURED";
			Output << " process_cpu_ms=";
			if (Value.ProcessCpuMs) Output << *Value.ProcessCpuMs; else Output << "NOT_MEASURED";
			Output << " requested_sleep_ms=" << Value.RequestedSleepMs << " actual_sleep_ms=" << Value.ActualSleepMs << '\n';
			Output << "[Qualification:WorkloadPhaseSpan] case=" << Case << " profile=" << Profile << " phase=" << Names[Index]
				<< " step=" << Value.Step << " native_tid=" << Value.NativeThread << " start_ns=" << Value.StartNs
				<< " end_ns=" << Value.EndNs << " timestamps_valid=" << (Value.StartNs > 0 && Value.EndNs >= Value.StartNs) << '\n';
		}
	}
};

} // namespace gargantuan::test_detail

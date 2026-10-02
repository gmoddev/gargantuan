#pragma once

#include <array>
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
		double RequestedSleepMs = 0, double ActualSleepMs = 0) noexcept {
		auto &Maximum = Maxima[static_cast<std::size_t>(Phase)];
		if (Maximum.Observed && Maximum.WallMs >= WallMs) return;
		// Keep CPU deltas from this exact wall-time span; independent maxima
		// cannot establish whether the slow span was actually on CPU.
		Maximum = {.Observed = true, .Step = Step, .WallMs = WallMs,
			.RequestedSleepMs = RequestedSleepMs, .ActualSleepMs = ActualSleepMs,
			.ThreadCpuMs = CpuDelta(Before.Thread100ns, After.Thread100ns, Before.ThreadValid, After.ThreadValid),
			.ProcessCpuMs = CpuDelta(Before.Process100ns, After.Process100ns, Before.ProcessValid, After.ProcessValid)};
	}

	void Print(std::ostream &Output, std::string_view Case) const {
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
		}
	}
};

} // namespace gargantuan::test_detail

#include "GasBenchmarkAllocations.hpp"

#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#else
#error The GAS resource benchmark requires Windows or Linux.
#endif

std::uint64_t GasBenchmarkAllocations::ReadPeakResidentBytes() {
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS Counters{};
	Counters.cb = sizeof(Counters);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters)))
		throw std::runtime_error("Could not query process peak working set");
	return static_cast<std::uint64_t>(Counters.PeakWorkingSetSize);
#else
	rusage Usage{};
	if (getrusage(RUSAGE_SELF, &Usage) != 0 || Usage.ru_maxrss < 0)
		throw std::runtime_error("Could not query process peak resident memory");
	return static_cast<std::uint64_t>(Usage.ru_maxrss) * 1024;
#endif
}

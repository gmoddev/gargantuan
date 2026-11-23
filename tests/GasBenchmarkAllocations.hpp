#pragma once

#include <cstdint>

namespace GasBenchmarkAllocations {
	// OS-reported whole-process high-water of resident memory, including the
	// live fixture, all threads, and allocator/engine overhead.
	[[nodiscard]] std::uint64_t ReadPeakResidentBytes();
}

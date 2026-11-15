#pragma once

#include <cstdint>

namespace BenchmarkAllocationProbe {
	struct Counts {
		std::uint64_t Calls = 0;
		std::uint64_t Bytes = 0;
	};

	// Counts successful C++ new requests made on the entering thread only.
	// SDL C malloc, driver allocations, and other threads are outside this scope.
	class Scope final {
	  public:
		explicit Scope(Counts &Destination) noexcept;
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;
		~Scope();

	  private:
		Counts *Previous = nullptr;
	};
}

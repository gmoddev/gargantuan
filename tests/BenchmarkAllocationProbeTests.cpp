#include "BenchmarkAllocationProbe.hpp"

#include <cstddef>
#include <iostream>
#include <new>

namespace {
	struct alignas(64) Aligned { char Bytes[64]; };
}

int main() {
	BenchmarkAllocationProbe::Counts Counts;
	{
		BenchmarkAllocationProbe::Scope Scope(Counts);
		// Explicit allocation-function calls cannot be elided as unused
		// new-expressions by an optimized qualification build.
		auto *Scalar = ::operator new(sizeof(int));
		auto *Array = ::operator new[](7 * sizeof(int));
		auto *Wide = ::operator new(sizeof(Aligned), std::align_val_t(alignof(Aligned)));
		auto *NoThrow = ::operator new(sizeof(int), std::nothrow);
		if (!Scalar || !Array || !Wide || !NoThrow) return 1;
		::operator delete(Scalar);
		::operator delete[](Array);
		::operator delete(Wide, std::align_val_t(alignof(Aligned)));
		::operator delete(NoThrow);
	}
	std::cout << "[Render:AllocationProbe] calls=" << Counts.Calls << " bytes=" << Counts.Bytes << '\n';
	return Counts.Calls == 4 &&
		Counts.Bytes == 2 * sizeof(int) + 7 * sizeof(int) + sizeof(Aligned) ? 0 : 2;
}

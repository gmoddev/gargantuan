#include "BenchmarkAllocationProbe.hpp"

#include <cstddef>
#include <new>

namespace {
	struct alignas(64) Aligned { char Bytes[64]; };
}

int main() {
	BenchmarkAllocationProbe::Counts Counts;
	{
		BenchmarkAllocationProbe::Scope Scope(Counts);
		auto *Scalar = new int(7);
		auto *Array = new int[7]{};
		auto *Wide = new Aligned{};
		auto *NoThrow = new (std::nothrow) int(9);
		if (!Scalar || !Array || !Wide || !NoThrow) return 1;
		delete Scalar;
		delete[] Array;
		delete Wide;
		delete NoThrow;
	}
	return Counts.Calls == 4 &&
		Counts.Bytes == 2 * sizeof(int) + 7 * sizeof(int) + sizeof(Aligned) ? 0 : 2;
}

#include "simd.h"
#include "block_allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// Keep the pointer opaque to the caller so optimized runs exercise the load.
static NOINLINE b3V32 LoadThree(const float *Source) {
	return b3LoadV(Source);
}

static uint32_t BitsOf(float Value) {
	uint32_t Bits;
	memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

static int CheckBlockAllocator(int ElementSize, int InitialCount) {
	enum { ElementCount = 300 };
	void *Elements[ElementCount];
	void *Reused[ElementCount];
	const int BeforeBytes = b3GetByteCount();
	b3BlockAllocator Allocator = b3CreateBlockAllocator(ElementSize, InitialCount);
	int SawFourModEight = 0;
	int Valid = 1;
	for (int Index = 0; Index < ElementCount; ++Index) {
		Elements[Index] = b3AllocateElement(&Allocator);
		if (((uintptr_t)Elements[Index] & 3u) != 0u) Valid = 0;
		if (((uintptr_t)Elements[Index] & 7u) == 4u) SawFourModEight = 1;
	}
	if (Allocator.allocationCount != ElementCount || !SawFourModEight) Valid = 0;
	for (int Index = 0; Index < ElementCount; ++Index)
		b3FreeElement(&Allocator, Elements[Index]);
	if (Allocator.allocationCount != 0) Valid = 0;
	for (int Index = 0; Index < ElementCount; ++Index) {
		Reused[Index] = b3AllocateElement(&Allocator);
		if (Reused[Index] != Elements[ElementCount - 1 - Index]) Valid = 0;
	}
	for (int Index = 0; Index < ElementCount; ++Index)
		b3FreeElement(&Allocator, Reused[Index]);
	if (Allocator.allocationCount != 0) Valid = 0;
	b3DestroyBlockAllocator(&Allocator);
	if (b3GetByteCount() != BeforeBytes) Valid = 0;
	if (!Valid)
		fprintf(stderr, "[Physics:Box3DCompatibility] %d-byte allocator failed with initial count %d\n",
			ElementSize, InitialCount);
	return Valid;
}

int main(void) {
	_Alignas(8) float Storage[4];
	const uint32_t Cases[2][3] = {
		{0x80000000u, 0x3f800000u, 0xc0200000u},
		{0x00000000u, 0xc15c0000u, 0x42f70000u},
	};
	const float *Source = Storage + 1;
	if (((uintptr_t)Source & 7u) != 4u) {
		fprintf(stderr, "[Physics:Box3DCompatibility] fixture is not 4-byte aligned\n");
		return 1;
	}
	for (int Case = 0; Case < 2; ++Case) {
		memcpy(Storage + 1, Cases[Case], sizeof(Cases[Case]));
		const b3V32 Loaded = LoadThree(Source);
		if (BitsOf(b3GetXV(Loaded)) != Cases[Case][0] ||
			BitsOf(b3GetYV(Loaded)) != Cases[Case][1] ||
			BitsOf(b3GetZV(Loaded)) != Cases[Case][2] ||
			BitsOf(b3GetV(Loaded, 3)) != 0u) {
			fprintf(stderr, "[Physics:Box3DCompatibility] SIMD lanes changed\n");
			return 1;
		}
	}
	if (!CheckBlockAllocator(12, 0) || !CheckBlockAllocator(20, 0) ||
		!CheckBlockAllocator(12, 300) || !CheckBlockAllocator(20, 300)) return 1;
	puts("[Physics:Box3DCompatibility] SIMD loads and 4-byte-aligned block free lists passed");
	return 0;
}

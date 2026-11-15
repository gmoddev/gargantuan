#include "BenchmarkAllocationProbe.hpp"

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {
	thread_local BenchmarkAllocationProbe::Counts *ActiveCounts = nullptr;

	void Count(std::size_t Bytes) noexcept {
		if (!ActiveCounts) return;
		++ActiveCounts->Calls;
		ActiveCounts->Bytes += Bytes;
	}

	void *Allocate(std::size_t Bytes) {
		if (void *Memory = std::malloc(Bytes ? Bytes : 1)) {
			Count(Bytes);
			return Memory;
		}
		throw std::bad_alloc();
	}

	void *AllocateAligned(std::size_t Bytes, std::size_t Alignment) {
		if (Bytes > std::numeric_limits<std::size_t>::max() - (Alignment - 1)) throw std::bad_alloc();
		const auto Rounded = ((Bytes ? Bytes : 1) + Alignment - 1) & ~(Alignment - 1);
#ifdef _WIN32
		void *Memory = _aligned_malloc(Rounded, Alignment);
#else
		void *Memory = std::aligned_alloc(Alignment, Rounded);
#endif
		if (!Memory) throw std::bad_alloc();
		Count(Bytes);
		return Memory;
	}

	void FreeAligned(void *Memory) noexcept {
#ifdef _WIN32
		_aligned_free(Memory);
#else
		std::free(Memory);
#endif
	}
}

namespace BenchmarkAllocationProbe {
	Scope::Scope(Counts &Destination) noexcept : Previous(ActiveCounts) { ActiveCounts = &Destination; }
	Scope::~Scope() { ActiveCounts = Previous; }
}

void *operator new(std::size_t Bytes) { return Allocate(Bytes); }
void *operator new[](std::size_t Bytes) { return Allocate(Bytes); }
void *operator new(std::size_t Bytes, std::align_val_t Alignment) {
	return AllocateAligned(Bytes, static_cast<std::size_t>(Alignment));
}
void *operator new[](std::size_t Bytes, std::align_val_t Alignment) {
	return AllocateAligned(Bytes, static_cast<std::size_t>(Alignment));
}
void *operator new(std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return Allocate(Bytes); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return Allocate(Bytes); } catch (...) { return nullptr; }
}
void *operator new(std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return AllocateAligned(Bytes, static_cast<std::size_t>(Alignment)); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return AllocateAligned(Bytes, static_cast<std::size_t>(Alignment)); } catch (...) { return nullptr; }
}
void operator delete(void *Memory) noexcept { std::free(Memory); }
void operator delete[](void *Memory) noexcept { std::free(Memory); }
void operator delete(void *Memory, std::size_t) noexcept { std::free(Memory); }
void operator delete[](void *Memory, std::size_t) noexcept { std::free(Memory); }
void operator delete(void *Memory, std::align_val_t) noexcept { FreeAligned(Memory); }
void operator delete[](void *Memory, std::align_val_t) noexcept { FreeAligned(Memory); }
void operator delete(void *Memory, std::size_t, std::align_val_t) noexcept { FreeAligned(Memory); }
void operator delete[](void *Memory, std::size_t, std::align_val_t) noexcept { FreeAligned(Memory); }
void operator delete(void *Memory, const std::nothrow_t &) noexcept { std::free(Memory); }
void operator delete[](void *Memory, const std::nothrow_t &) noexcept { std::free(Memory); }
void operator delete(void *Memory, std::align_val_t, const std::nothrow_t &) noexcept { FreeAligned(Memory); }
void operator delete[](void *Memory, std::align_val_t, const std::nothrow_t &) noexcept { FreeAligned(Memory); }

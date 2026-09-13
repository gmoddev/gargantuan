#pragma once

// Test-executable-only allocation reuse, following ContentAvailabilityBenchmark's
// complete allocation override. Normal content tests use the standard allocator.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace ContentOwnershipAllocation {
struct Header { void *Raw; std::size_t Bytes, Alignment; };
inline thread_local std::uintptr_t Target = 0;
inline thread_local void *Held = nullptr;
inline thread_local bool Reuse = false;

inline void *Allocate(std::size_t Bytes, std::size_t Alignment) {
	Alignment = std::max(Alignment, alignof(Header));
	if (Reuse && Held) {
		auto *Metadata = static_cast<Header *>(Held) - 1;
		if (Metadata->Bytes == Bytes && Metadata->Alignment == Alignment) {
			auto *Result = Held;
			Held = nullptr; Reuse = false;
			return Result;
		}
	}
	if (Bytes > std::numeric_limits<std::size_t>::max() - sizeof(Header) - Alignment) throw std::bad_alloc();
	auto *Raw = std::malloc(Bytes + sizeof(Header) + Alignment - 1);
	if (!Raw) throw std::bad_alloc();
	const auto Start = reinterpret_cast<std::uintptr_t>(Raw) + sizeof(Header);
	const auto Aligned = (Start + Alignment - 1) & ~(static_cast<std::uintptr_t>(Alignment) - 1);
	*(reinterpret_cast<Header *>(Aligned) - 1) = {Raw, Bytes, Alignment};
	return reinterpret_cast<void *>(Aligned);
}
inline void Free(void *Pointer) noexcept {
	if (!Pointer) return;
	auto *Metadata = static_cast<Header *>(Pointer) - 1;
	const auto Start = reinterpret_cast<std::uintptr_t>(Pointer);
	if (Target && Target >= Start && Target - Start < Metadata->Bytes) {
		Held = Pointer; Target = 0;
		return;
	}
	std::free(Metadata->Raw);
}
inline void Reset() {
	Target = 0; Reuse = false;
	Free(Held); Held = nullptr;
}
}

void *operator new(std::size_t Bytes) { return ContentOwnershipAllocation::Allocate(Bytes, alignof(std::max_align_t)); }
void *operator new[](std::size_t Bytes) { return ContentOwnershipAllocation::Allocate(Bytes, alignof(std::max_align_t)); }
void *operator new(std::size_t Bytes, std::align_val_t Alignment) { return ContentOwnershipAllocation::Allocate(Bytes, static_cast<std::size_t>(Alignment)); }
void *operator new[](std::size_t Bytes, std::align_val_t Alignment) { return ContentOwnershipAllocation::Allocate(Bytes, static_cast<std::size_t>(Alignment)); }
void *operator new(std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return operator new(Bytes); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, const std::nothrow_t &) noexcept {
	try { return operator new[](Bytes); } catch (...) { return nullptr; }
}
void *operator new(std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return operator new(Bytes, Alignment); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t Bytes, std::align_val_t Alignment, const std::nothrow_t &) noexcept {
	try { return operator new[](Bytes, Alignment); } catch (...) { return nullptr; }
}
void operator delete(void *Pointer) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete(void *Pointer, std::size_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer, std::size_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete(void *Pointer, std::align_val_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer, std::align_val_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete(void *Pointer, std::size_t, std::align_val_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer, std::size_t, std::align_val_t) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete(void *Pointer, const std::nothrow_t &) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer, const std::nothrow_t &) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete(void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { ContentOwnershipAllocation::Free(Pointer); }
void operator delete[](void *Pointer, std::align_val_t, const std::nothrow_t &) noexcept { ContentOwnershipAllocation::Free(Pointer); }

#include "ExperimentSupport.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <box3d/base.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {
#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
struct AllocationHeader { void* Raw; size_t Bytes; };
constinit std::atomic<uint64_t> AllocationCalls{}, FreeCalls{}, AllocatedBytes{}, FreedBytes{}, LiveBytes{}, PeakBytes{};

void* AllocateTracked(size_t Bytes, size_t Alignment = alignof(std::max_align_t)) {
    Alignment = std::max(Alignment, alignof(std::max_align_t));
    const size_t Storage = std::max(Bytes, size_t(1));
    if ((Alignment & (Alignment - 1)) || Alignment > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader)
        || Storage > std::numeric_limits<size_t>::max() - Alignment - sizeof(AllocationHeader))
        throw std::bad_alloc();
    void* Raw = std::malloc(Storage + Alignment - 1 + sizeof(AllocationHeader));
    if (!Raw) throw std::bad_alloc();
    const auto Address = (reinterpret_cast<uintptr_t>(Raw) + sizeof(AllocationHeader) + Alignment - 1) & ~(Alignment - 1);
    auto* Header = reinterpret_cast<AllocationHeader*>(Address) - 1;
    Header->Raw = Raw;
    Header->Bytes = Bytes;
    AllocationCalls.fetch_add(1, std::memory_order_relaxed);
    AllocatedBytes.fetch_add(Bytes, std::memory_order_relaxed);
    const auto Live = LiveBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
    auto Peak = PeakBytes.load(std::memory_order_relaxed);
    while (Peak < Live && !PeakBytes.compare_exchange_weak(Peak, Live, std::memory_order_relaxed)) {}
    return reinterpret_cast<void*>(Address);
}

void FreeTracked(void* Pointer) noexcept {
    if (!Pointer) return;
    const auto* Header = reinterpret_cast<AllocationHeader*>(Pointer) - 1;
    FreeCalls.fetch_add(1, std::memory_order_relaxed);
    FreedBytes.fetch_add(Header->Bytes, std::memory_order_relaxed);
    LiveBytes.fetch_sub(Header->Bytes, std::memory_order_relaxed);
    std::free(Header->Raw);
}

void* ReallocateTracked(void* Pointer, size_t OldBytes, size_t NewBytes) {
    void* Replacement = AllocateTracked(NewBytes);
    if (Pointer) {
        const auto* Header = reinterpret_cast<AllocationHeader*>(Pointer) - 1;
        std::memcpy(Replacement, Pointer, std::min({OldBytes, Header->Bytes, NewBytes}));
        FreeTracked(Pointer);
    }
    return Replacement;
}
void* AllocateBox(int32_t Bytes, int32_t Alignment) { return AllocateTracked(size_t(Bytes), size_t(Alignment)); }
void* AllocateJolt(size_t Bytes) { return AllocateTracked(Bytes); }
#endif
}

#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
void* operator new(size_t Bytes) { return AllocateTracked(Bytes); }
void* operator new[](size_t Bytes) { return AllocateTracked(Bytes); }
void* operator new(size_t Bytes, std::align_val_t Alignment) { return AllocateTracked(Bytes, size_t(Alignment)); }
void* operator new[](size_t Bytes, std::align_val_t Alignment) { return AllocateTracked(Bytes, size_t(Alignment)); }
void* operator new(size_t Bytes, const std::nothrow_t&) noexcept { try { return AllocateTracked(Bytes); } catch (...) { return nullptr; } }
void* operator new[](size_t Bytes, const std::nothrow_t&) noexcept { try { return AllocateTracked(Bytes); } catch (...) { return nullptr; } }
void* operator new(size_t Bytes, std::align_val_t Alignment, const std::nothrow_t&) noexcept { try { return AllocateTracked(Bytes, size_t(Alignment)); } catch (...) { return nullptr; } }
void* operator new[](size_t Bytes, std::align_val_t Alignment, const std::nothrow_t&) noexcept { try { return AllocateTracked(Bytes, size_t(Alignment)); } catch (...) { return nullptr; } }
void operator delete(void* Pointer) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer) noexcept { FreeTracked(Pointer); }
void operator delete(void* Pointer, size_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer, size_t) noexcept { FreeTracked(Pointer); }
void operator delete(void* Pointer, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete(void* Pointer, size_t, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer, size_t, std::align_val_t) noexcept { FreeTracked(Pointer); }
void operator delete(void* Pointer, const std::nothrow_t&) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer, const std::nothrow_t&) noexcept { FreeTracked(Pointer); }
void operator delete(void* Pointer, std::align_val_t, const std::nothrow_t&) noexcept { FreeTracked(Pointer); }
void operator delete[](void* Pointer, std::align_val_t, const std::nothrow_t&) noexcept { FreeTracked(Pointer); }
#endif

namespace PhysicsEvaluation {
void InstallPhysicsAllocators() {
    JPH::RegisterDefaultAllocator();
#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
    JPH::Allocate = AllocateJolt;
    JPH::Reallocate = ReallocateTracked;
    JPH::Free = FreeTracked;
    JPH::AlignedAllocate = AllocateTracked;
    JPH::AlignedFree = FreeTracked;
    b3SetAllocator(AllocateBox, FreeTracked);
#endif
}

void SuppressErrorDialogs() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

ResourceSnapshot CaptureResources() {
    ResourceSnapshot Result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS Memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &Memory, sizeof(Memory))) {
        Result.WorkingSetBytes = Memory.WorkingSetSize;
        Result.PeakWorkingSetBytes = Memory.PeakWorkingSetSize;
    }
    FILETIME Created{}, Exited{}, Kernel{}, User{};
    if (GetProcessTimes(GetCurrentProcess(), &Created, &Exited, &Kernel, &User)) {
        const uint64_t KernelTicks = (uint64_t(Kernel.dwHighDateTime) << 32) | Kernel.dwLowDateTime;
        const uint64_t UserTicks = (uint64_t(User.dwHighDateTime) << 32) | User.dwLowDateTime;
        Result.ProcessCpuSeconds = double(KernelTicks + UserTicks) / 10000000.0;
    }
#else
    rusage Usage{};
    if (getrusage(RUSAGE_SELF, &Usage) == 0) {
        Result.ProcessCpuSeconds = double(Usage.ru_utime.tv_sec + Usage.ru_stime.tv_sec)
            + double(Usage.ru_utime.tv_usec + Usage.ru_stime.tv_usec) / 1000000.0;
#if defined(__APPLE__)
        Result.PeakWorkingSetBytes = uint64_t(Usage.ru_maxrss);
#else
        Result.PeakWorkingSetBytes = uint64_t(Usage.ru_maxrss) * 1024;
#endif
    }
#if defined(__linux__)
    if (FILE* File = std::fopen("/proc/self/statm", "r")) {
        unsigned long Pages{}, Resident{};
        if (std::fscanf(File, "%lu %lu", &Pages, &Resident) == 2)
            Result.WorkingSetBytes = uint64_t(Resident) * uint64_t(sysconf(_SC_PAGESIZE));
        std::fclose(File);
    }
#endif
#endif
#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
    Result.AllocationCalls = AllocationCalls.load(std::memory_order_relaxed);
    Result.FreeCalls = FreeCalls.load(std::memory_order_relaxed);
    Result.AllocatedBytes = AllocatedBytes.load(std::memory_order_relaxed);
    Result.FreedBytes = FreedBytes.load(std::memory_order_relaxed);
    Result.LiveBytes = LiveBytes.load(std::memory_order_relaxed);
    Result.PeakBytes = PeakBytes.load(std::memory_order_relaxed);
#endif
    return Result;
}

nlohmann::json ResourceDelta(const ResourceSnapshot& Before, const ResourceSnapshot& After) {
    nlohmann::json Result = {
        {"WorkingSetBeforeBytes", Before.WorkingSetBytes}, {"WorkingSetAfterBytes", After.WorkingSetBytes},
        {"PeakWorkingSetProcessLifetimeBytes", After.PeakWorkingSetBytes},
        {"ProcessCpuSeconds", After.ProcessCpuSeconds - Before.ProcessCpuSeconds},
        {"PeakScope", "Process lifetime; not a phase-local peak or heap-retirement proof"}
    };
#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
    Result["AllocationInstrumentation"] = true;
    Result["AllocationCalls"] = After.AllocationCalls - Before.AllocationCalls;
    Result["FreeCalls"] = After.FreeCalls - Before.FreeCalls;
    Result["AllocatedBytes"] = After.AllocatedBytes - Before.AllocatedBytes;
    Result["FreedBytes"] = After.FreedBytes - Before.FreedBytes;
    Result["LiveBytesDelta"] = int64_t(After.LiveBytes) - int64_t(Before.LiveBytes);
    Result["TrackedPeakProcessLifetimeBytes"] = After.PeakBytes;
    Result["AllocationScope"] = "Requested bytes: process C++ new plus Box3D/Jolt hooks, including harness work inside the phase; excludes other malloc, allocator metadata, OS and library storage. Separate instrumented pass; do not rank its timings.";
#else
    Result["AllocationInstrumentation"] = false;
    Result["AllocationCalls"] = nullptr;
    Result["AllocationScope"] = "Unavailable in timing executable; use separate Allocations target";
#endif
    return Result;
}
}

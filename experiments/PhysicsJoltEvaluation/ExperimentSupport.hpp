#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace PhysicsEvaluation {
struct ResourceSnapshot {
    uint64_t AllocationCalls{}, FreeCalls{}, AllocatedBytes{}, FreedBytes{};
    uint64_t LiveBytes{}, PeakBytes{}, WorkingSetBytes{}, PeakWorkingSetBytes{};
    double ProcessCpuSeconds{};
};

// Fresh-process setup only, before any Jolt or Box3D allocation. Timing and
// instrumented-allocation executables are distinct build targets.
void InstallPhysicsAllocators();
void SuppressErrorDialogs();
ResourceSnapshot CaptureResources();
nlohmann::json ResourceDelta(const ResourceSnapshot& Before, const ResourceSnapshot& After);
}

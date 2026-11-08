#include <Jolt/Jolt.h>
#include <Jolt/Core/Profiler.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

#if defined(JPH_EXTERNAL_PROFILE) && defined(JPH_SHARED_LIBRARY)
#error "[Physics:Selective] External profile hook requires static Jolt"
#endif

namespace {
    enum class Stage : unsigned char {
        Broadphase, Narrowphase, Gravity, IslandBuild, VelocitySolve,
        RigidIntegrate, Ccd, PositionSolve, SoftPrepare, SoftCollide,
        SoftSimulate, SoftFinalize, SoftIntegrate, SoftEdges, SoftVolumes,
        SoftContactVelocity, SoftState, BuildJobs, Barrier, Other, Count
    };

    constexpr std::array<const char *, static_cast<size_t>(Stage::Count)> StageNames = {
        "Broadphase", "Narrowphase", "Gravity", "IslandBuild", "VelocitySolve",
        "RigidIntegrate", "Ccd", "PositionSolve", "SoftPrepare", "SoftCollide",
        "SoftSimulate", "SoftFinalize", "SoftIntegrate", "SoftEdges", "SoftVolumes",
        "SoftContactVelocity", "SoftState", "BuildJobs", "Barrier", "Other"
    };

    struct Counter final {
        std::atomic<unsigned long long> Nanoseconds{0};
        std::atomic<unsigned long long> Count{0};
    };

    std::array<Counter, static_cast<size_t>(Stage::Count)> Counters;

    bool Has(const char *Name, const char *Part) { return std::strstr(Name, Part) != nullptr; }

    Stage Classify(const char *Name) {
        if (Has(Name, "BroadPhase") || Has(Name, "Broadphase")) return Stage::Broadphase;
        if (Has(Name, "FindCollisions") || Has(Name, "ProcessBodyPair")) return Stage::Narrowphase;
        if (Has(Name, "ApplyGravity")) return Stage::Gravity;
        if (Has(Name, "BuildIslands") || Has(Name, "FinalizeIslands")) return Stage::IslandBuild;
        if (Has(Name, "SolveVelocityConstraints") || Has(Name, "SetupVelocityConstraints")) return Stage::VelocitySolve;
        if (Has(Name, "IntegrateVelocity")) return Stage::RigidIntegrate;
        if (Has(Name, "CCD")) return Stage::Ccd;
        if (Has(Name, "SolvePositionConstraints")) return Stage::PositionSolve;
        if (Has(Name, "SoftBodyPrepare")) return Stage::SoftPrepare;
        if (Has(Name, "SoftBodyCollide")) return Stage::SoftCollide;
        if (Has(Name, "SoftBodySimulate")) return Stage::SoftSimulate;
        if (Has(Name, "SoftBodyFinalize")) return Stage::SoftFinalize;
        if (Has(Name, "IntegratePositions")) return Stage::SoftIntegrate;
        if (Has(Name, "ApplyEdgeConstraints")) return Stage::SoftEdges;
        if (Has(Name, "ApplyVolumeConstraints")) return Stage::SoftVolumes;
        if (Has(Name, "ApplyCollisionConstraintsAndUpdateVelocities")) return Stage::SoftContactVelocity;
        if (Has(Name, "UpdateSoftBodyState")) return Stage::SoftState;
        if (Has(Name, "Build Jobs")) return Stage::BuildJobs;
        if (Has(Name, "barrier") || Has(Name, "Barrier")) return Stage::Barrier;
        return Stage::Other;
    }

    struct Sample final {
        std::chrono::steady_clock::time_point Start;
        Stage Kind;
    };
}

#if defined(JPH_EXTERNAL_PROFILE) && !defined(JPH_SHARED_LIBRARY)
JPH_NAMESPACE_BEGIN
ExternalProfileMeasurement::ExternalProfileMeasurement(const char *Name, uint32) {
    static_assert(sizeof(Sample) <= sizeof(mUserData));
    new (mUserData) Sample{std::chrono::steady_clock::now(), Classify(Name)};
}

ExternalProfileMeasurement::~ExternalProfileMeasurement() {
    const auto *Measurement = reinterpret_cast<const Sample *>(mUserData);
    const auto Duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - Measurement->Start).count();
    auto &Bucket = Counters[static_cast<size_t>(Measurement->Kind)];
    Bucket.Nanoseconds.fetch_add(static_cast<unsigned long long>(Duration), std::memory_order_relaxed);
    Bucket.Count.fetch_add(1, std::memory_order_relaxed);
}
JPH_NAMESPACE_END
#endif

void DumpExternalProfile() {
#if defined(JPH_EXTERNAL_PROFILE)
    std::cout << "{\"ProfileWorkNanoseconds\":{";
    for (size_t Index = 0; Index < StageNames.size(); ++Index) {
        if (Index) std::cout << ',';
        std::cout << '"' << StageNames[Index] << "\":{\"Count\":"
            << Counters[Index].Count.load(std::memory_order_relaxed)
            << ",\"Nanoseconds\":" << Counters[Index].Nanoseconds.load(std::memory_order_relaxed)
            << '}';
    }
    std::cout << "},\"Note\":\"Nested work time, not wall time or a critical path\"}\n";
#endif
}

void ResetExternalProfile() {
#if defined(JPH_EXTERNAL_PROFILE)
    for (auto &Bucket : Counters) {
        Bucket.Nanoseconds.store(0, std::memory_order_relaxed);
        Bucket.Count.store(0, std::memory_order_relaxed);
    }
#endif
}

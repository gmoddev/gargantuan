#include "../SoftFixtures.hpp"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

void DumpExternalProfile();

// This is a deliberately narrow experiment: layer 3 cannot interact with
// layers 1/2, including soft-body contact. The global reference has exactly
// the same authored collision rules. A general all-collide scene is refused.
namespace {

constexpr JPH::ObjectLayer StaticLayer = 0;
constexpr JPH::ObjectLayer CoupledRigidLayer = 1;
constexpr JPH::ObjectLayer SoftLayer = 2;
constexpr JPH::ObjectLayer IsolatedRigidLayer = 3;
constexpr float FrameDelta = 1.0f / 60.0f;

enum class Partition { Coupled, Isolated };

struct Layers final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 4; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer Layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(Layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Selective"; }
#endif
};

struct Pairs final : JPH::ObjectLayerPairFilter {
    bool Separate = true;
    explicit Pairs(bool InSeparate = true) : Separate(InSeparate) {}
    bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override {
        if (Separate && ((A == IsolatedRigidLayer && (B == CoupledRigidLayer || B == SoftLayer)) ||
                         (B == IsolatedRigidLayer && (A == CoupledRigidLayer || A == SoftLayer))))
            return false;
        return true;
    }
};

struct BroadPairs final : JPH::ObjectVsBroadPhaseLayerFilter {
    const Pairs &Filter;
    explicit BroadPairs(const Pairs &InFilter) : Filter(InFilter) {}
    bool ShouldCollide(JPH::ObjectLayer A, JPH::BroadPhaseLayer B) const override {
        return Filter.ShouldCollide(A, B.GetValue());
    }
};

struct ExecutionResources final {
    JPH::TempAllocatorImpl Temp{32 * 1024 * 1024};
    JPH::JobSystemThreadPool Jobs;

    explicit ExecutionResources(int Workers) :
        Jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, Workers - 1) {}
};

struct World final {
    Layers LayerMap;
    Pairs PairFilter;
    BroadPairs BroadFilter;
    ExecutionResources &Resources;
    JPH::PhysicsSystem Physics;
    std::vector<JPH::BodyID> Bodies;
    std::vector<JPH::Ref<JPH::TwoBodyConstraint>> Joints;

    World(ExecutionResources &InResources, bool Separate) : PairFilter(Separate),
        BroadFilter(PairFilter), Resources(InResources) {
        Physics.Init(8192, 0, 16384, 8192, LayerMap, BroadFilter, PairFilter);
        Physics.SetGravity(JPH::Vec3::sZero());
    }

    ~World() {
        for (const auto &Joint : Joints) Physics.RemoveConstraint(Joint.GetPtr());
        auto &Interface = Physics.GetBodyInterface();
        for (JPH::BodyID Id : Bodies) {
            Interface.RemoveBody(Id);
            Interface.DestroyBody(Id);
        }
    }

    JPH::BodyID AddSphere(JPH::RVec3 Position, JPH::ObjectLayer Layer, JPH::Vec3 Velocity,
                          float Radius = 0.45f) {
        JPH::BodyCreationSettings Settings(new JPH::SphereShape(Radius), Position,
            JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, Layer);
        Settings.mAllowSleeping = false;
        Settings.mLinearDamping = 0.0f;
        Settings.mAngularDamping = 0.0f;
        const JPH::BodyID Id = Physics.GetBodyInterface().CreateAndAddBody(Settings, JPH::EActivation::Activate);
        if (Id.IsInvalid()) throw std::runtime_error("[Physics:Selective] Body admission failed");
        Physics.GetBodyInterface().SetLinearVelocity(Id, Velocity);
        Bodies.push_back(Id);
        return Id;
    }

    JPH::BodyID AddSoft(const JPH::Ref<JPH::SoftBodySharedSettings> &Shared) {
        JPH::SoftBodyCreationSettings Settings(Shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), SoftLayer);
        Settings.mNumIterations = 12;
        Settings.mAllowSleeping = false;
        Settings.mLinearDamping = 2.4f;
        Settings.mVertexRadius = 0.05f;
        Settings.mFacesDoubleSided = true;
        JPH::BodyID Id = Physics.GetBodyInterface().CreateAndAddSoftBody(Settings, JPH::EActivation::Activate);
        if (Id.IsInvalid()) throw std::runtime_error("[Physics:Selective] Soft admission failed");
        Bodies.push_back(Id);
        return Id;
    }

    void Join(JPH::BodyID A, JPH::BodyID B, JPH::RVec3 PointA, JPH::RVec3 PointB) {
        JPH::DistanceConstraintSettings Settings;
        Settings.mPoint1 = PointA;
        Settings.mPoint2 = PointB;
        JPH::BodyLockWrite LockA(Physics.GetBodyLockInterfaceNoLock(), A);
        JPH::BodyLockWrite LockB(Physics.GetBodyLockInterfaceNoLock(), B);
        if (!LockA.Succeeded() || !LockB.Succeeded())
            throw std::runtime_error("[Physics:Selective] Joint body unavailable");
        JPH::Ref<JPH::TwoBodyConstraint> Joint = Settings.Create(LockA.GetBody(), LockB.GetBody());
        LockB.ReleaseLock();
        LockA.ReleaseLock();
        Physics.AddConstraint(Joint.GetPtr());
        Joints.push_back(Joint);
    }

    double Step(float DeltaTime, int CollisionSteps) {
        const auto Start = PhysicsJoltEvaluation::Clock::now();
        if (Physics.Update(DeltaTime, CollisionSteps, &Resources.Temp, &Resources.Jobs) != JPH::EPhysicsUpdateError::None)
            throw std::runtime_error("[Physics:Selective] Update failed");
        return PhysicsJoltEvaluation::Milliseconds(Start);
    }
};

struct Certificate final {
    bool CrossPairEligible = false;
    bool CrossJoint = false;
    bool CrossSoftInfluence = false;

    void Require() const {
        if (CrossPairEligible || CrossJoint || CrossSoftInfluence)
            throw std::runtime_error("[Physics:Selective] Partition lacks an interaction certificate");
    }
};

void RegisterJoint(Partition A, Partition B, Certificate &State) {
    if (A != B) State.CrossJoint = true;
    State.Require();
}

void RequireSpatialMargin(double Gap, double ClosingSpeed, double Horizon) {
    if (!std::isfinite(Gap) || !std::isfinite(ClosingSpeed) || !std::isfinite(Horizon) ||
        Gap <= 10.0 + ClosingSpeed * Horizon)
        throw std::runtime_error("[Physics:Selective] Spatial margin refused before step");
}

struct Options final {
    std::string_view Mode = "global";
    int Frames = 120;
    int Workers = 1;
    int IsolatedBodies = 32;
};

int ParseInteger(std::string_view Value) {
    int Result = 0;
    const auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), Result);
    if (Parsed.ec != std::errc{} || Parsed.ptr != Value.data() + Value.size())
        throw std::runtime_error("[Physics:Selective] Invalid integer option");
    return Result;
}

Options Parse(int Count, char **Arguments) {
    Options Result;
    for (int Index = 1; Index < Count; Index += 2) {
        if (Index + 1 >= Count) throw std::runtime_error("[Physics:Selective] Missing option value");
        const std::string_view Key = Arguments[Index];
        const std::string_view Value = Arguments[Index + 1];
        if (Key == "--mode") Result.Mode = Value;
        else if (Key == "--frames") Result.Frames = ParseInteger(Value);
        else if (Key == "--workers") Result.Workers = ParseInteger(Value);
        else if (Key == "--isolated-bodies") Result.IsolatedBodies = ParseInteger(Value);
        else throw std::runtime_error("[Physics:Selective] Unknown option");
    }
    if (Result.Frames < 1 || Result.Frames > 600 || Result.Workers < 1 || Result.Workers > 8 ||
        Result.IsolatedBodies < 1 || Result.IsolatedBodies > 512)
        throw std::runtime_error("[Physics:Selective] Option out of bounds");
    return Result;
}

void RefusalChecks() {
    Certificate State;
    State.CrossPairEligible = true;
    try { State.Require(); throw std::runtime_error("[Physics:Selective] Ordinary pair accepted"); }
    catch (const std::runtime_error &Error) {
        if (std::string_view(Error.what()).find("lacks an interaction certificate") == std::string_view::npos) throw;
    }
    State = {};
    try { RegisterJoint(Partition::Coupled, Partition::Isolated, State);
        throw std::runtime_error("[Physics:Selective] Cross joint accepted"); }
    catch (const std::runtime_error &Error) {
        if (std::string_view(Error.what()).find("lacks an interaction certificate") == std::string_view::npos) throw;
    }
    State = {};
    State.CrossSoftInfluence = true;
    try { State.Require(); throw std::runtime_error("[Physics:Selective] Cross soft influence accepted"); }
    catch (const std::runtime_error &Error) {
        if (std::string_view(Error.what()).find("lacks an interaction certificate") == std::string_view::npos) throw;
    }
    for (const auto [Gap, Speed, Horizon] :
        {std::array<double, 3>{2.0, 0.0, 1.0}, std::array<double, 3>{80.0, 100.0, 1.0}}) {
        try { RequireSpatialMargin(Gap, Speed, Horizon);
            throw std::runtime_error("[Physics:Selective] Near or crossing pair accepted"); }
        catch (const std::runtime_error &Error) {
            if (std::string_view(Error.what()).find("Spatial margin refused") == std::string_view::npos) throw;
        }
    }
}

struct Scene final {
    ExecutionResources Resources;
    std::unique_ptr<World> Coupled;
    std::unique_ptr<World> Isolated;
    JPH::BodyID Soft;
    std::array<JPH::BodyID, 2> CoupledRigids;
    std::vector<JPH::BodyID> IsolatedRigids;
    JPH::Ref<JPH::SoftBodySharedSettings> Shared;

    Scene(const Options &Config, bool Selective) : Resources(Config.Workers) {
        Coupled = std::make_unique<World>(Resources, true);
        if (Selective) Isolated = std::make_unique<World>(Resources, true);
        Shared = PhysicsJoltEvaluation::Cloth(13, 4.0f, false, false);
        Soft = Coupled->AddSoft(Shared);
        CoupledRigids[0] = Coupled->AddSphere(JPH::RVec3(0.0, 0.0, -2.0),
            CoupledRigidLayer, JPH::Vec3(0.0f, 0.0f, 10.0f));
        CoupledRigids[1] = Coupled->AddSphere(JPH::RVec3(0.8, 0.0, -2.0),
            CoupledRigidLayer, JPH::Vec3(0.0f, 0.0f, 10.0f), 0.2f);
        Coupled->Join(CoupledRigids[0], CoupledRigids[1], JPH::RVec3(0.0, 0.0, -2.0),
            JPH::RVec3(0.8, 0.0, -2.0));
        World &IsolatedWorld = Selective ? *Isolated : *Coupled;
        const double OriginX = Config.Mode == "refusal-near" ? 5.0 : 80.0;
        const float TravelSpeed = Config.Mode == "refusal-crossing" ? -100.0f : 0.0f;
        for (int Index = 0; Index < Config.IsolatedBodies; ++Index) {
            const int Column = Index % 8;
            const int Row = Index / 8;
            IsolatedRigids.push_back(IsolatedWorld.AddSphere(
                JPH::RVec3(OriginX + Column * 0.91, Row * 0.91, 0.0), IsolatedRigidLayer,
                JPH::Vec3(TravelSpeed + ((Column & 1) ? -0.3f : 0.3f), 0.0f, 0.0f)));
        }
    }
};

double X(JPH::RVec3 Point) { return static_cast<double>(Point.GetX()); }

void Mix(std::uint64_t &Hash, float Value) {
    Hash ^= std::bit_cast<std::uint32_t>(Value);
    Hash *= 1099511628211ULL;
}

void Mix(std::uint64_t &Hash, JPH::Vec3 Value) {
    Mix(Hash, Value.GetX());
    Mix(Hash, Value.GetY());
    Mix(Hash, Value.GetZ());
}

void CaptureCoupledState(const Scene &Worlds, std::uint64_t &Hash) {
    const auto &Bodies = Worlds.Coupled->Physics.GetBodyInterface();
    for (JPH::BodyID Id : Worlds.CoupledRigids) {
        const JPH::RVec3 Position = Bodies.GetPosition(Id);
        Mix(Hash, static_cast<float>(Position.GetX()));
        Mix(Hash, static_cast<float>(Position.GetY()));
        Mix(Hash, static_cast<float>(Position.GetZ()));
        Mix(Hash, Bodies.GetLinearVelocity(Id));
    }
    JPH::BodyLockRead SoftLock(Worlds.Coupled->Physics.GetBodyLockInterface(), Worlds.Soft);
    if (!SoftLock.Succeeded()) throw std::runtime_error("[Physics:Selective] Soft body lost");
    const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(SoftLock.GetBody().GetMotionProperties());
    for (const auto &Vertex : Motion->GetVertices()) {
        Mix(Hash, Vertex.mPosition);
        Mix(Hash, Vertex.mVelocity);
    }
}

void CheckSpatialMargin(const Scene &Worlds, float SecondsRemaining) {
    // An additional refusal guard for this fixture. Semantic collision-group
    // exclusion is the full-run certificate; this margin is not relied on to
    // prove that arbitrary future velocities or soft displacements are safe.
    const auto &CoupledBodies = Worlds.Coupled->Physics.GetBodyInterface();
    const auto &IsolatedBodies = (Worlds.Isolated ? Worlds.Isolated : Worlds.Coupled)->Physics.GetBodyInterface();
    const double CoupledX = std::max(X(CoupledBodies.GetPosition(Worlds.CoupledRigids[0])),
        X(CoupledBodies.GetPosition(Worlds.CoupledRigids[1])));
    double MaxCoupledVx = -std::numeric_limits<double>::infinity();
    for (JPH::BodyID Id : Worlds.CoupledRigids)
        MaxCoupledVx = std::max(MaxCoupledVx,
            static_cast<double>(CoupledBodies.GetLinearVelocity(Id).GetX()));
    double MinIsolatedX = std::numeric_limits<double>::infinity();
    double MinIsolatedVx = std::numeric_limits<double>::infinity();
    for (JPH::BodyID Id : Worlds.IsolatedRigids) {
        MinIsolatedX = std::min(MinIsolatedX, X(IsolatedBodies.GetPosition(Id)));
        MinIsolatedVx = std::min(MinIsolatedVx,
            static_cast<double>(IsolatedBodies.GetLinearVelocity(Id).GetX()));
    }
    RequireSpatialMargin(MinIsolatedX - CoupledX,
        4.0 + std::max(0.0, MaxCoupledVx - MinIsolatedVx), SecondsRemaining);
}

} // namespace

int main(int Count, char **Arguments) {
    try {
        const Options Config = Parse(Count, Arguments);
        RefusalChecks();
        if (Config.Mode == "refusal") {
            std::cout << "{\"RefusalChecks\":5}\n";
            return 0;
        }
        if (Config.Mode != "global" && Config.Mode != "selective" &&
            Config.Mode != "refusal-near" && Config.Mode != "refusal-crossing" &&
            Config.Mode != "refusal-joint")
            throw std::runtime_error("[Physics:Selective] Unknown mode");
        const bool ExpectRefusal = Config.Mode.starts_with("refusal-");
        const bool Selective = Config.Mode != "global";
        Certificate State;
        State.CrossPairEligible = Pairs(true).ShouldCollide(CoupledRigidLayer, IsolatedRigidLayer);
        State.CrossSoftInfluence = Pairs(true).ShouldCollide(SoftLayer, IsolatedRigidLayer);
        if (Config.Mode == "refusal-joint") {
            try { RegisterJoint(Partition::Coupled, Partition::Isolated, State); }
            catch (const std::runtime_error &) {
                std::cout << "{\"RefusedBeforeUpdate\":true,\"Reason\":\"CrossJoint\"}\n";
                return 0;
            }
            throw std::runtime_error("[Physics:Selective] Cross joint accepted");
        }
        State.Require();
        PhysicsJoltEvaluation::JoltRuntime Runtime;
        const auto SceneStart = PhysicsJoltEvaluation::Clock::now();
        Scene Worlds(Config, Selective);
        if (ExpectRefusal) {
            try { CheckSpatialMargin(Worlds, Config.Frames * FrameDelta); }
            catch (const std::runtime_error &) {
                std::cout << "{\"RefusedBeforeUpdate\":true,\"Reason\":\"SpatialMargin\"}\n";
                return 0;
            }
            throw std::runtime_error("[Physics:Selective] Unsafe scene accepted");
        }
        const double SceneSetupMs = PhysicsJoltEvaluation::Milliseconds(SceneStart);
        double CoupledMs = 0.0;
        double IsolatedMs = 0.0;
        double GuardMs = 0.0;
        std::uint64_t CoupledTrajectoryHash = 1469598103934665603ULL;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            const auto GuardStart = PhysicsJoltEvaluation::Clock::now();
            CheckSpatialMargin(Worlds, (Config.Frames - Frame) * FrameDelta);
            GuardMs += PhysicsJoltEvaluation::Milliseconds(GuardStart);
            for (int Update = 0; Update < 2; ++Update)
                CoupledMs += Worlds.Coupled->Step(FrameDelta / 2.0f, 8);
            if (Selective) IsolatedMs += Worlds.Isolated->Step(FrameDelta, 4);
            CaptureCoupledState(Worlds, CoupledTrajectoryHash);
        }
        const auto &CoupledBodies = Worlds.Coupled->Physics.GetBodyInterface();
        const auto &IsolatedBodies = (Worlds.Isolated ? Worlds.Isolated : Worlds.Coupled)->Physics.GetBodyInterface();
        JPH::BodyLockRead SoftLock(Worlds.Coupled->Physics.GetBodyLockInterface(), Worlds.Soft);
        if (!SoftLock.Succeeded()) throw std::runtime_error("[Physics:Selective] Soft body lost");
        const auto *SoftMotion = static_cast<const JPH::SoftBodyMotionProperties *>(SoftLock.GetBody().GetMotionProperties());
        const auto &Vertices = SoftMotion->GetVertices();
        const double MaxEdgeStretch = PhysicsJoltEvaluation::MaxEdgeStretch(*Worlds.Shared, *SoftMotion);
        double SoftPositionChecksum = 0.0;
        for (const auto &Vertex : Vertices)
            SoftPositionChecksum += Vertex.mPosition.GetX() + 3.0 * Vertex.mPosition.GetY() + 7.0 * Vertex.mPosition.GetZ();
        double FinalMaxIsolatedSpeed = 0.0;
        double FinalMaxIsolatedPenetration = 0.0;
        std::vector<JPH::RVec3> IsolatedPositions;
        for (JPH::BodyID Id : Worlds.IsolatedRigids) {
            IsolatedPositions.push_back(IsolatedBodies.GetPosition(Id));
            FinalMaxIsolatedSpeed = std::max(FinalMaxIsolatedSpeed,
                static_cast<double>(IsolatedBodies.GetLinearVelocity(Id).Length()));
        }
        for (size_t A = 0; A < IsolatedPositions.size(); ++A)
            for (size_t B = A + 1; B < IsolatedPositions.size(); ++B)
                FinalMaxIsolatedPenetration = std::max(FinalMaxIsolatedPenetration,
                    std::max(0.0, 0.9 - static_cast<double>((IsolatedPositions[A] - IsolatedPositions[B]).Length())));
        std::cout << std::setprecision(17) << "{\"Mode\":\"" << Config.Mode << "\",\"Frames\":"
            << Config.Frames << ",\"Workers\":" << Config.Workers
            << ",\"IsolatedBodies\":" << Config.IsolatedBodies
            << ",\"SceneSetupMs\":" << SceneSetupMs
            << ",\"GuardMs\":" << GuardMs
            << ",\"CoupledWorldMs\":" << CoupledMs
            << ",\"IsolatedWorldMs\":" << IsolatedMs
            << ",\"TotalUpdateMs\":" << CoupledMs + IsolatedMs
            << ",\"CoupledRigidX\":" << X(CoupledBodies.GetPosition(Worlds.CoupledRigids[0]))
            << ",\"IsolatedRigidX\":" << X(IsolatedBodies.GetPosition(Worlds.IsolatedRigids[0]))
            << ",\"FinalMaxIsolatedSpeed\":" << FinalMaxIsolatedSpeed
            << ",\"FinalMaxIsolatedPenetration\":" << FinalMaxIsolatedPenetration
            << ",\"MaxEdgeStretch\":" << MaxEdgeStretch
            << ",\"CoupledTrajectoryHash\":" << CoupledTrajectoryHash
            << ",\"SoftPositionChecksum\":" << SoftPositionChecksum << "}\n";
        DumpExternalProfile();
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << Error.what() << '\n';
        return 2;
    }
}

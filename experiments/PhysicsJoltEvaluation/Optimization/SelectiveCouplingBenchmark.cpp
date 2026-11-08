#include "../SoftFixtures.hpp"
#include "../ExperimentSupport.hpp"
#include "../ExperimentalVelocityReconstruction.hpp"

#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>

#include <array>
#include <atomic>
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
void ResetExternalProfile();

// This is a deliberately narrow experiment: layer 3 cannot interact with
// layers 1/2, including soft-body contact. The global reference has exactly
// the same authored collision rules. A general all-collide scene is refused.
namespace {

constexpr JPH::ObjectLayer CoupledRigidLayer = 1;
constexpr JPH::ObjectLayer SoftLayer = 2;
constexpr JPH::ObjectLayer IsolatedRigidLayer = 3;
constexpr JPH::ObjectLayer IsolatedStaticLayer = 4;
constexpr float FrameDelta = 1.0f / 60.0f;

enum class Partition { Coupled, Isolated };

struct Layers final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 5; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer Layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(Layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Selective"; }
#endif
};

struct Pairs final : JPH::ObjectLayerPairFilter {
    const bool Separate;
    explicit Pairs(bool InSeparate = true) : Separate(InSeparate) {}
    bool ShouldCollide(JPH::ObjectLayer A, JPH::ObjectLayer B) const override {
        const bool AIsolated = A == IsolatedRigidLayer || A == IsolatedStaticLayer;
        const bool BIsolated = B == IsolatedRigidLayer || B == IsolatedStaticLayer;
        if (Separate && AIsolated != BIsolated)
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

struct Contacts final : JPH::SoftBodyContactListener {
    std::atomic<unsigned long long> Added{0};
    void OnSoftBodyContactAdded(const JPH::Body &, const JPH::SoftBodyManifold &) override {
        Added.fetch_add(1, std::memory_order_relaxed);
    }
};

struct RigidContacts final : JPH::ContactListener {
    std::atomic<unsigned long long> Added{0};
    std::atomic<unsigned long long> Persisted{0};

    static bool IsIsolated(JPH::ObjectLayer Layer) {
        return Layer == IsolatedRigidLayer || Layer == IsolatedStaticLayer;
    }

    void OnContactAdded(const JPH::Body &A, const JPH::Body &B,
                        const JPH::ContactManifold &, JPH::ContactSettings &) override {
        if (IsIsolated(A.GetObjectLayer()) && IsIsolated(B.GetObjectLayer()))
            Added.fetch_add(1, std::memory_order_relaxed);
    }

    void OnContactPersisted(const JPH::Body &A, const JPH::Body &B,
                            const JPH::ContactManifold &, JPH::ContactSettings &) override {
        if (IsIsolated(A.GetObjectLayer()) && IsIsolated(B.GetObjectLayer()))
            Persisted.fetch_add(1, std::memory_order_relaxed);
    }
};

struct World final {
    Layers LayerMap;
    Pairs PairFilter;
    BroadPairs BroadFilter;
    ExecutionResources &Resources;
    bool Dense;
    Contacts ContactCount;
    RigidContacts RigidContactCount;
    JPH::PhysicsSystem Physics;
    std::vector<JPH::BodyID> Bodies;
    std::vector<JPH::Ref<JPH::TwoBodyConstraint>> Joints;

    World(ExecutionResources &InResources, bool Separate, bool InDense) : PairFilter(Separate),
        BroadFilter(PairFilter), Resources(InResources), Dense(InDense) {
        Physics.Init(8192, 0, 16384, 8192, LayerMap, BroadFilter, PairFilter);
        Physics.SetGravity(Dense ? JPH::Vec3(0.0f, -9.81f, 0.0f) : JPH::Vec3::sZero());
        Physics.SetSoftBodyContactListener(&ContactCount);
        Physics.SetContactListener(&RigidContactCount);
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
        Settings.mGravityFactor = Dense && Layer == IsolatedRigidLayer ? 1.0f : 0.0f;
        Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        Settings.mMassPropertiesOverride.mMass =
            0.7f * (4.0f / 3.0f) * 3.14159265358979323846f * Radius * Radius * Radius;
        const JPH::BodyID Id = Physics.GetBodyInterface().CreateAndAddBody(Settings, JPH::EActivation::Activate);
        if (Id.IsInvalid()) throw std::runtime_error("[Physics:Selective] Body admission failed");
        Physics.GetBodyInterface().SetLinearVelocity(Id, Velocity);
        Bodies.push_back(Id);
        return Id;
    }

    void AddFloor() {
        JPH::BodyCreationSettings Settings(new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
            JPH::RVec3(83.2, -0.5, 0.0), JPH::Quat::sIdentity(),
            JPH::EMotionType::Static, IsolatedStaticLayer);
        const JPH::BodyID Id = Physics.GetBodyInterface().CreateAndAddBody(
            Settings, JPH::EActivation::DontActivate);
        if (Id.IsInvalid()) throw std::runtime_error("[Physics:Selective] Floor admission failed");
        Bodies.push_back(Id);
    }

    JPH::BodyID AddSoft(const JPH::Ref<JPH::SoftBodySharedSettings> &Shared) {
        JPH::SoftBodyCreationSettings Settings(Shared, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), SoftLayer);
        Settings.mNumIterations = 12;
        PhysicsJoltEvaluation::ApplyVelocityReconstruction(Settings);
        Settings.mAllowSleeping = false;
        Settings.mLinearDamping = 2.4f;
        Settings.mGravityFactor = 0.0f;
        Settings.mVertexRadius = 0.05f;
        Settings.mFacesDoubleSided = true;
        JPH::BodyID Id = Physics.GetBodyInterface().CreateAndAddSoftBody(Settings, JPH::EActivation::Activate);
        if (Id.IsInvalid()) throw std::runtime_error("[Physics:Selective] Soft admission failed");
        Bodies.push_back(Id);
        return Id;
    }

    void Join(JPH::BodyID A, JPH::BodyID B, JPH::RVec3 PointA, JPH::RVec3 PointB) {
        const auto &Interface = Physics.GetBodyInterface();
        const auto IsIsolated = [](JPH::ObjectLayer Layer) {
            return Layer == IsolatedRigidLayer || Layer == IsolatedStaticLayer;
        };
        const bool AIsolated = IsIsolated(Interface.GetObjectLayer(A));
        const bool BIsolated = IsIsolated(Interface.GetObjectLayer(B));
        if (AIsolated != BIsolated)
            throw std::runtime_error("[Physics:Selective] Cross-partition joint refused before step");
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
    std::string_view Layout = "sparse";
    int Frames = 120;
    int Workers = 1;
    int IsolatedBodies = 32;
#ifdef PHYSICS_GANTRIA_JOLT
    int VelocityReconstruction = 1;
#else
    int VelocityReconstruction = 0;
#endif
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
        else if (Key == "--layout") Result.Layout = Value;
        else if (Key == "--frames") Result.Frames = ParseInteger(Value);
        else if (Key == "--workers") Result.Workers = ParseInteger(Value);
        else if (Key == "--isolated-bodies") Result.IsolatedBodies = ParseInteger(Value);
        else if (Key == "--velocity-reconstruction")
            Result.VelocityReconstruction = PhysicsJoltEvaluation::ParseVelocityReconstruction(Value);
        else throw std::runtime_error("[Physics:Selective] Unknown option");
    }
    if (Result.Frames < 1 || Result.Frames > 600 || Result.Workers < 1 || Result.Workers > 8 ||
        Result.IsolatedBodies < 1 || Result.IsolatedBodies > 512 ||
        (Result.Layout != "sparse" && Result.Layout != "dense"))
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
    bool Dense;
    std::unique_ptr<World> Coupled;
    std::unique_ptr<World> Isolated;
    JPH::BodyID Soft;
    std::array<JPH::BodyID, 2> CoupledRigids;
    std::vector<JPH::BodyID> IsolatedRigids;
    JPH::Ref<JPH::SoftBodySharedSettings> Shared;

    Scene(const Options &Config, bool Selective) : Resources(Config.Workers), Dense(Config.Layout == "dense") {
        Coupled = std::make_unique<World>(Resources, true, Dense);
        if (Selective) Isolated = std::make_unique<World>(Resources, true, Dense);
        Shared = PhysicsJoltEvaluation::Cloth(13, 4.0f, false, false);
        PhysicsJoltEvaluation::ValidateVelocityConstraints(*Shared, Config.VelocityReconstruction);
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
                JPH::RVec3(OriginX + Column * 0.91, Dense ? 0.55 + Row * 0.91 : Row * 0.91, 0.0),
                IsolatedRigidLayer,
                JPH::Vec3(TravelSpeed + (Dense ? 0.0f : ((Column & 1) ? -0.3f : 0.3f)),
                    0.0f, 0.0f)));
        }
        if (Dense) IsolatedWorld.AddFloor();
    }
};

double X(JPH::RVec3 Point) { return static_cast<double>(Point.GetX()); }
double SafeNumber(double Value) { return std::isfinite(Value) ? Value : 0.0; }

double Percentile(std::vector<double> Values, double Fraction) {
    std::sort(Values.begin(), Values.end());
    const size_t Index = static_cast<size_t>(std::ceil(Fraction * Values.size())) - 1;
    return Values[std::min(Index, Values.size() - 1)];
}

void Mix(std::uint64_t &Hash, float Value) {
    Hash ^= std::bit_cast<std::uint32_t>(Value);
    Hash *= 1099511628211ULL;
}

void Mix(std::uint64_t &Hash, JPH::Vec3 Value) {
    Mix(Hash, Value.GetX());
    Mix(Hash, Value.GetY());
    Mix(Hash, Value.GetZ());
}

void Mix(std::uint64_t &Hash, JPH::Quat Value) {
    Mix(Hash, Value.GetX());
    Mix(Hash, Value.GetY());
    Mix(Hash, Value.GetZ());
    Mix(Hash, Value.GetW());
}

bool Finite(JPH::Vec3 Value) {
    return std::isfinite(static_cast<double>(Value.GetX())) &&
        std::isfinite(static_cast<double>(Value.GetY())) &&
        std::isfinite(static_cast<double>(Value.GetZ()));
}

bool Finite(JPH::Quat Value) {
    return std::isfinite(static_cast<double>(Value.GetX())) &&
        std::isfinite(static_cast<double>(Value.GetY())) &&
        std::isfinite(static_cast<double>(Value.GetZ())) &&
        std::isfinite(static_cast<double>(Value.GetW()));
}

void CaptureCoupledState(const Scene &Worlds, std::uint64_t &Hash,
                         bool &FiniteEveryFrame, double &MaxStretch) {
    const auto &Bodies = Worlds.Coupled->Physics.GetBodyInterface();
    for (JPH::BodyID Id : Worlds.CoupledRigids) {
        const JPH::RVec3 Position = Bodies.GetPosition(Id);
        FiniteEveryFrame &= std::isfinite(X(Position)) &&
            std::isfinite(static_cast<double>(Position.GetY())) &&
            std::isfinite(static_cast<double>(Position.GetZ()));
        Mix(Hash, static_cast<float>(Position.GetX()));
        Mix(Hash, static_cast<float>(Position.GetY()));
        Mix(Hash, static_cast<float>(Position.GetZ()));
        const JPH::Quat Rotation = Bodies.GetRotation(Id);
        FiniteEveryFrame &= Finite(Rotation);
        Mix(Hash, Rotation);
        const JPH::Vec3 Velocity = Bodies.GetLinearVelocity(Id);
        FiniteEveryFrame &= Finite(Velocity);
        Mix(Hash, Velocity);
        const JPH::Vec3 AngularVelocity = Bodies.GetAngularVelocity(Id);
        FiniteEveryFrame &= Finite(AngularVelocity);
        Mix(Hash, AngularVelocity);
    }
    const JPH::RVec3 SoftPosition = Bodies.GetPosition(Worlds.Soft);
    FiniteEveryFrame &= std::isfinite(X(SoftPosition)) &&
        std::isfinite(static_cast<double>(SoftPosition.GetY())) &&
        std::isfinite(static_cast<double>(SoftPosition.GetZ()));
    Mix(Hash, static_cast<float>(SoftPosition.GetX()));
    Mix(Hash, static_cast<float>(SoftPosition.GetY()));
    Mix(Hash, static_cast<float>(SoftPosition.GetZ()));
    const JPH::Quat SoftRotation = Bodies.GetRotation(Worlds.Soft);
    FiniteEveryFrame &= Finite(SoftRotation);
    Mix(Hash, SoftRotation);
    JPH::BodyLockRead SoftLock(Worlds.Coupled->Physics.GetBodyLockInterface(), Worlds.Soft);
    if (!SoftLock.Succeeded()) throw std::runtime_error("[Physics:Selective] Soft body lost");
    const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(SoftLock.GetBody().GetMotionProperties());
    for (const auto &Vertex : Motion->GetVertices()) {
        FiniteEveryFrame &= Finite(Vertex.mPosition) && Finite(Vertex.mVelocity);
        Mix(Hash, Vertex.mPosition);
        Mix(Hash, Vertex.mVelocity);
    }
    const double Stretch = PhysicsJoltEvaluation::MaxEdgeStretch(*Worlds.Shared, *Motion);
    if (std::isfinite(Stretch)) MaxStretch = std::max(MaxStretch, Stretch);
    else FiniteEveryFrame = false;
}

struct PhysicalSummary final {
    bool FiniteEveryFrame = true;
    double MaxSpeed = 0.0;
    double MaxPenetration = 0.0;
    double MaxFloorPenetration = 0.0;
};

void InspectIsolated(const Scene &Worlds, PhysicalSummary &Summary) {
    const auto &Bodies = (Worlds.Isolated ? Worlds.Isolated : Worlds.Coupled)->Physics.GetBodyInterface();
    std::vector<JPH::RVec3> Positions;
    Positions.reserve(Worlds.IsolatedRigids.size());
    for (JPH::BodyID Id : Worlds.IsolatedRigids) {
        const JPH::RVec3 Position = Bodies.GetPosition(Id);
        const JPH::Vec3 Velocity = Bodies.GetLinearVelocity(Id);
        Summary.FiniteEveryFrame &= std::isfinite(X(Position)) &&
            std::isfinite(static_cast<double>(Position.GetY())) &&
            std::isfinite(static_cast<double>(Position.GetZ())) &&
            std::isfinite(static_cast<double>(Velocity.GetX())) &&
            std::isfinite(static_cast<double>(Velocity.GetY())) &&
            std::isfinite(static_cast<double>(Velocity.GetZ()));
        const double Speed = static_cast<double>(Velocity.Length());
        if (std::isfinite(Speed)) Summary.MaxSpeed = std::max(Summary.MaxSpeed, Speed);
        if (Worlds.Dense && std::isfinite(static_cast<double>(Position.GetY())))
            Summary.MaxFloorPenetration = std::max(Summary.MaxFloorPenetration,
                std::max(0.0, 0.45 - static_cast<double>(Position.GetY())));
        Positions.push_back(Position);
    }
    for (size_t A = 0; A < Positions.size(); ++A)
        for (size_t B = A + 1; B < Positions.size(); ++B) {
            const double DistanceSquared = static_cast<double>((Positions[A] - Positions[B]).LengthSq());
            if (std::isfinite(DistanceSquared) && DistanceSquared < 0.9 * 0.9)
                Summary.MaxPenetration = std::max(Summary.MaxPenetration,
                    0.9 - std::sqrt(std::max(0.0, DistanceSquared)));
            else if (!std::isfinite(DistanceSquared)) Summary.FiniteEveryFrame = false;
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
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        const Options Config = Parse(Count, Arguments);
        PhysicsJoltEvaluation::ConfigureVelocityReconstruction(Config.VelocityReconstruction);
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
        const bool Selective = Config.Mode != "global" && Config.Mode != "refusal-joint";
        Certificate State;
        State.CrossPairEligible = Pairs(true).ShouldCollide(CoupledRigidLayer, IsolatedRigidLayer);
        State.CrossSoftInfluence = Pairs(true).ShouldCollide(SoftLayer, IsolatedRigidLayer);
        State.Require();
        const auto ResourcesBefore = PhysicsEvaluation::CaptureResources();
        PhysicsJoltEvaluation::JoltRuntime Runtime;
        const auto SceneStart = PhysicsJoltEvaluation::Clock::now();
        Scene Worlds(Config, Selective);
        if (Config.Mode == "refusal-joint") {
            try {
                Worlds.Coupled->Join(Worlds.CoupledRigids[0], Worlds.IsolatedRigids[0],
                    JPH::RVec3::sZero(), JPH::RVec3::sZero());
            } catch (const std::runtime_error &Error) {
                if (std::string_view(Error.what()).find("Cross-partition joint refused") == std::string_view::npos)
                    throw;
                std::cout << "{\"RefusedBeforeUpdate\":true,\"Reason\":\"CrossJoint\"}\n";
                return 0;
            }
            throw std::runtime_error("[Physics:Selective] Cross joint accepted");
        }
        if (ExpectRefusal) {
            try { CheckSpatialMargin(Worlds, Config.Frames * FrameDelta); }
            catch (const std::runtime_error &) {
                std::cout << "{\"RefusedBeforeUpdate\":true,\"Reason\":\"SpatialMargin\"}\n";
                return 0;
            }
            throw std::runtime_error("[Physics:Selective] Unsafe scene accepted");
        }
        const double SceneSetupMs = PhysicsJoltEvaluation::Milliseconds(SceneStart);
        const auto InitialQualityStart = PhysicsJoltEvaluation::Clock::now();
        PhysicalSummary InitialQuality;
        InspectIsolated(Worlds, InitialQuality);
        const double InitialQualityMs = PhysicsJoltEvaluation::Milliseconds(InitialQualityStart);
        const bool InitialQualityPass = InitialQuality.FiniteEveryFrame &&
            InitialQuality.MaxPenetration <= 0.25 && InitialQuality.MaxFloorPenetration <= 0.25;
        if (!InitialQualityPass)
            throw std::runtime_error("[Physics:Selective] Initial rigid geometry exceeds quality gate");
        ResetExternalProfile();
        double CoupledMs = 0.0;
        double IsolatedMs = 0.0;
        double GuardMs = 0.0;
        double QualityMs = 0.0;
        PhysicalSummary IsolatedQuality;
        int RigidContactFrames = 0;
        bool CoupledFiniteEveryFrame = true;
        double MaxEdgeStretchAcrossFrames = 0.0;
        std::vector<double> FrameUpdateMs;
        std::vector<double> FrameGuardMs;
        FrameUpdateMs.reserve(Config.Frames);
        FrameGuardMs.reserve(Config.Frames);
        std::uint64_t CoupledTrajectoryHash = 1469598103934665603ULL;
        const World &IsolatedWorld = Selective ? *Worlds.Isolated : *Worlds.Coupled;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            const auto GuardStart = PhysicsJoltEvaluation::Clock::now();
            CheckSpatialMargin(Worlds, (Config.Frames - Frame) * FrameDelta);
            const double ThisGuardMs = PhysicsJoltEvaluation::Milliseconds(GuardStart);
            GuardMs += ThisGuardMs;
            FrameGuardMs.push_back(ThisGuardMs);
            double ThisUpdateMs = 0.0;
            const auto RigidContactsBefore =
                IsolatedWorld.RigidContactCount.Added.load(std::memory_order_relaxed) +
                IsolatedWorld.RigidContactCount.Persisted.load(std::memory_order_relaxed);
            for (int Update = 0; Update < 2; ++Update)
            {
                const double ThisStepMs = Worlds.Coupled->Step(FrameDelta / 2.0f, 8);
                CoupledMs += ThisStepMs;
                ThisUpdateMs += ThisStepMs;
            }
            if (Selective) {
                const double ThisStepMs = Worlds.Isolated->Step(FrameDelta, 4);
                IsolatedMs += ThisStepMs;
                ThisUpdateMs += ThisStepMs;
            }
            const auto RigidContactsAfter =
                IsolatedWorld.RigidContactCount.Added.load(std::memory_order_relaxed) +
                IsolatedWorld.RigidContactCount.Persisted.load(std::memory_order_relaxed);
            if (RigidContactsAfter > RigidContactsBefore) ++RigidContactFrames;
            const auto QualityStart = PhysicsJoltEvaluation::Clock::now();
            FrameUpdateMs.push_back(ThisUpdateMs);
            CaptureCoupledState(Worlds, CoupledTrajectoryHash,
                CoupledFiniteEveryFrame, MaxEdgeStretchAcrossFrames);
            InspectIsolated(Worlds, IsolatedQuality);
            QualityMs += PhysicsJoltEvaluation::Milliseconds(QualityStart);
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
        const bool IsolatedQualityPass = IsolatedQuality.FiniteEveryFrame &&
            IsolatedQuality.MaxSpeed <= 100.0 && IsolatedQuality.MaxPenetration <= 0.25 &&
            IsolatedQuality.MaxFloorPenetration <= 0.25;
        const bool DenseContactPass = !Worlds.Dense ||
            RigidContactFrames >= std::max(1, Config.Frames / 4);
        const auto SoftContactCallbacks = Worlds.Coupled->ContactCount.Added.load(std::memory_order_relaxed);
        const bool CoupledStretchPass = CoupledFiniteEveryFrame && MaxEdgeStretchAcrossFrames <= 0.1;
        const bool CoupledContactObserved = SoftContactCallbacks > 0;
        const bool BenchmarkQualityPass = IsolatedQualityPass && CoupledStretchPass &&
            CoupledContactObserved && DenseContactPass;
        const auto ResourcesAfter = PhysicsEvaluation::CaptureResources();
        std::cout << std::setprecision(17) << "{\"Mode\":\"" << Config.Mode << "\",\"Frames\":"
            << Config.Frames << ",\"Workers\":" << Config.Workers
            << ",\"IsolatedBodies\":" << Config.IsolatedBodies
            << ",\"Layout\":\"" << Config.Layout << '"'
            << ",\"VelocityReconstruction\":\""
            << PhysicsJoltEvaluation::VelocityReconstructionName(Config.VelocityReconstruction) << '"'
            << ",\"SceneSetupMs\":" << SceneSetupMs
            << ",\"InitialQualityMs\":" << InitialQualityMs
            << ",\"GuardMs\":" << GuardMs
            << ",\"QualityMs\":" << QualityMs
            << ",\"CoupledWorldMs\":" << CoupledMs
            << ",\"IsolatedWorldMs\":" << IsolatedMs
            << ",\"TotalUpdateMs\":" << CoupledMs + IsolatedMs
            << ",\"UpdateP50Ms\":" << Percentile(FrameUpdateMs, 0.50)
            << ",\"UpdateP95Ms\":" << Percentile(FrameUpdateMs, 0.95)
            << ",\"GuardP95Ms\":" << Percentile(FrameGuardMs, 0.95)
            << ",\"CoupledRigidX\":" << SafeNumber(X(CoupledBodies.GetPosition(Worlds.CoupledRigids[0])))
            << ",\"IsolatedRigidX\":" << SafeNumber(X(IsolatedBodies.GetPosition(Worlds.IsolatedRigids[0])))
            << ",\"MaxIsolatedSpeedAcrossFrames\":" << IsolatedQuality.MaxSpeed
            << ",\"MaxIsolatedPenetrationAcrossFrames\":" << IsolatedQuality.MaxPenetration
            << ",\"MaxFloorPenetrationAcrossFrames\":" << IsolatedQuality.MaxFloorPenetration
            << ",\"RigidContactFrames\":" << RigidContactFrames
            << ",\"DenseContactPass\":" << (DenseContactPass ? "true" : "false")
            << ",\"IsolatedRigidContactAdded\":"
            << IsolatedWorld.RigidContactCount.Added.load(std::memory_order_relaxed)
            << ",\"IsolatedRigidContactPersisted\":"
            << IsolatedWorld.RigidContactCount.Persisted.load(std::memory_order_relaxed)
            << ",\"InitialPairPenetration\":" << InitialQuality.MaxPenetration
            << ",\"InitialFloorPenetration\":" << InitialQuality.MaxFloorPenetration
            << ",\"InitialQualityPass\":" << (InitialQualityPass ? "true" : "false")
            << ",\"IsolatedFiniteEveryFrame\":" << (IsolatedQuality.FiniteEveryFrame ? "true" : "false")
            << ",\"IsolatedQualityPass\":" << (IsolatedQualityPass ? "true" : "false")
            << ",\"CoupledFiniteEveryFrame\":" << (CoupledFiniteEveryFrame ? "true" : "false")
            << ",\"CoupledStretchPass\":" << (CoupledStretchPass ? "true" : "false")
            << ",\"SoftContactCallbacks\":" << SoftContactCallbacks
            << ",\"CoupledContactObserved\":" << (CoupledContactObserved ? "true" : "false")
            << ",\"BenchmarkQualityPass\":" << (BenchmarkQualityPass ? "true" : "false")
            << ",\"RequiresFullInternalCouplingQualification\":true"
            << ",\"WorkingSetBeforeBytes\":" << ResourcesBefore.WorkingSetBytes
            << ",\"WorkingSetAfterBytes\":" << ResourcesAfter.WorkingSetBytes
            << ",\"PeakWorkingSetProcessLifetimeBytes\":" << ResourcesAfter.PeakWorkingSetBytes
            << ",\"WorkingSetAvailable\":" << (ResourcesAfter.WorkingSetAvailable ? "true" : "false")
            << ",\"MaxEdgeStretch\":" << SafeNumber(MaxEdgeStretch)
            << ",\"MaxEdgeStretchAcrossFrames\":" << MaxEdgeStretchAcrossFrames
            << ",\"CoupledTrajectoryHash\":" << CoupledTrajectoryHash
            << ",\"SoftPositionChecksum\":" << SafeNumber(SoftPositionChecksum) << "}\n";
        DumpExternalProfile();
        return BenchmarkQualityPass ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cerr << Error.what() << '\n';
        return 2;
    }
}

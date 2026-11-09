#include "ContactExperimentSeams.hpp"
#include "../ExperimentSupport.hpp"
#include "../Provider/UnifiedJoltProvider.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gargantuan;
using Json = nlohmann::json;
constexpr float DeltaTime = 1.0f / 60.0f;
void HashPosition(std::uint64_t &Hash, glm::vec3 Position) {
    for (float Value : {Position.x, Position.y, Position.z}) {
        std::uint32_t Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        Hash ^= Bits;
        Hash *= 1099511628211ull;
    }
}

glm::vec3 Vec(const Json &Value) {
    if (!Value.is_array() || Value.size() != 3) throw std::invalid_argument("Expected xyz triple");
    const glm::vec3 Result(Value[0].get<float>(), Value[1].get<float>(), Value[2].get<float>());
    if (!std::isfinite(Result.x) || !std::isfinite(Result.y) || !std::isfinite(Result.z))
        throw std::invalid_argument("Nonfinite xyz triple");
    return Result;
}
Json Triplet(glm::vec3 Value) { return Json::array({Value.x, Value.y, Value.z}); }
Json QuatValues(glm::quat Value) { return Json::array({Value.w, Value.x, Value.y, Value.z}); }

struct SphereCase {
    std::string Id;
    std::string Motion;
    std::string KinematicUpdate;
    std::string TrajectoryKind;
    glm::vec3 Start{0.0f};
    glm::vec3 Velocity{0.0f};
    float Radius = 1.0f;
    float Mass = 0.0f;
    float Duration = 0.66f;
    float Frequency = 1.0f;
    PhysicsBodyId RigidId;
    PhysicsBodyDesc Description;
    [[nodiscard]] glm::vec3 Target(float Time) const {
        if (TrajectoryKind == "linear_clamped") return Start + Velocity * std::min(Time, Duration);
        if (TrajectoryKind == "linear") return Start + Velocity * Time;
        if (TrajectoryKind == "sine")
            return Start + Velocity * (std::sin(2.0f * 3.14159265358979323846f * Frequency * Time) / Frequency);
        return Start;
    }
};

void Require(bool Condition, const char *Message) {
    if (!Condition) throw std::runtime_error(Message);
}

void AddEdge(Json &Edges, const std::vector<glm::vec3> &Rest, unsigned A, unsigned B, const char *Kind) {
    Edges.push_back({{"A", A}, {"B", B},
        {"RestLengthM", glm::length(Rest[B] - Rest[A])}, {"Kind", Kind}});
}

Json MakeFixture(const Json &Config, const SoftBodyDefinition &Definition,
                 const SoftBodyState &Initial, const std::vector<SphereCase> &Spheres,
                 glm::vec3 Gravity, glm::vec3 InitialVelocity, float AuthoredDamping, int Frames) {
    const auto &Rest = *Initial.Positions;
    Json Fixture = {{"Schema", "Gargantuan.ContactFixture.V1"},
        {"CaseId", Config.at("CaseId")}, {"Frames", Frames},
        {"DeltaTimeSeconds", DeltaTime}, {"RestPositions", Json::array()},
        {"InitialVelocities", Json::array()}, {"VertexMassKg", Json::array()},
        {"Triangles", Json::array()}, {"StructuralEdges", Json::array()},
        {"Pins", Json::array()}, {"ThicknessM", Definition.Material.Thickness},
        {"GravityMPerS2", Triplet(Gravity)}, {"Colliders", Json::array()},
        {"Gates", Config.value("Gates", Json{{"MaximumAxialStrain", 0.1},
            {"MaximumPinErrorM", 0.0001}, {"MaximumSpherePenetrationM", 0.001}})}};
    std::vector<bool> Pinned(Rest.size(), false);
    for (const auto &Pin : Definition.Attachments) {
        Pinned[Pin.Vertex] = true;
        Fixture["Pins"].push_back({{"Vertex", Pin.Vertex}, {"Target", Triplet(Pin.Position)}});
    }
    for (std::size_t Index = 0; Index < Rest.size(); ++Index) {
        Fixture["RestPositions"].push_back(Triplet(Rest[Index]));
        Fixture["InitialVelocities"].push_back(Triplet(Pinned[Index] ? glm::vec3(0.0f) : InitialVelocity));
        Fixture["VertexMassKg"].push_back(Pinned[Index] ? 0.0f : Definition.Material.ParticleMass);
    }
    const auto &Indices = *Initial.Indices;
    for (std::size_t Index = 0; Index < Indices.size(); Index += 3)
        Fixture["Triangles"].push_back(Json::array({Indices[Index], Indices[Index + 1], Indices[Index + 2]}));
    const auto N = Definition.ResolutionX;
    const auto Id = [N](unsigned X, unsigned Y) { return Y * N + X; };
    for (unsigned Y = 0; Y < N; ++Y) for (unsigned X = 0; X < N; ++X) {
        const auto A = Id(X, Y);
        if (X + 1 < N) AddEdge(Fixture["StructuralEdges"], Rest, A, Id(X + 1, Y), "axial");
        if (Y + 1 < N) AddEdge(Fixture["StructuralEdges"], Rest, A, Id(X, Y + 1), "axial");
        if (X + 1 < N && Y + 1 < N) {
            AddEdge(Fixture["StructuralEdges"], Rest, A, Id(X + 1, Y + 1), "shear");
            AddEdge(Fixture["StructuralEdges"], Rest, Id(X + 1, Y), Id(X, Y + 1), "shear");
        }
        if (X + 2 < N) AddEdge(Fixture["StructuralEdges"], Rest, A, Id(X + 2, Y), "bend");
        if (Y + 2 < N) AddEdge(Fixture["StructuralEdges"], Rest, A, Id(X, Y + 2), "bend");
    }
    for (const auto &Sphere : Spheres)
        Fixture["Colliders"].push_back({{"Id", Sphere.Id}, {"Kind", "sphere"},
            {"RadiusM", Sphere.Radius},
            {"MassKg", Sphere.Motion == "dynamic" ? Json(Sphere.Mass) : Json(nullptr)},
            {"Motion", Sphere.Motion},
            {"Trajectory", {{"Kind", Sphere.Motion == "dynamic" ? "free_dynamic" : Sphere.TrajectoryKind},
                {"Start", Triplet(Sphere.Start)}, {"Velocity", Triplet(Sphere.Velocity)},
                {"DurationSeconds", Sphere.Duration}, {"FrequencyHz", Sphere.Frequency}}}});
    for (std::size_t Index = 0; Index < Spheres.size(); ++Index)
        if (Spheres[Index].Motion == "kinematic")
            Fixture["Colliders"][Index]["Trajectory"]["KinematicUpdate"] = Spheres[Index].KinematicUpdate;
    Fixture["Authored"] = {{"SizeM", Triplet(Definition.Size)},
        {"RotationWxyz", Config.value("RotationWxyz", Json::array({1,0,0,0}))},
        {"Material", {{"ParticleMassKg", Definition.Material.ParticleMass},
            {"StretchCompliance", Definition.Material.StretchCompliance},
            {"BendCompliance", Definition.Material.BendCompliance},
            {"AuthoredFrameDamping", AuthoredDamping},
            {"AppliedStepDamping", Definition.Material.Damping},
            {"Friction", Definition.Material.Friction}}},
        {"XpbdSubsteps", Config.value("XpbdSubsteps", 32)},
        {"InitialVelocityMPerS", Triplet(InitialVelocity)},
        {"RotateAuthoredInputs", Config.value("RotateAuthoredInputs", false)},
        {"StaticPoseProjectionWork", "INCOMPLETE"}};
    return Fixture;
}

int Run(const std::filesystem::path &ConfigPath, const std::filesystem::path &Output) {
    std::ifstream Input(ConfigPath);
    Require(bool(Input), "Cannot read contact case JSON");
    const Json Config = Json::parse(Input);
    const std::string BackendName = Config.at("Backend").get<std::string>();
    Require(BackendName == "jolt" || BackendName == "xpbd", "Backend must be jolt or xpbd");
    const bool CaptureTrace = Config.value("CaptureTrace", true);
    const char *AccountingEnvironment = std::getenv("GANTRIA_CONTACT_ACCOUNTING");
    Require(!AccountingEnvironment ||
        (std::strcmp(AccountingEnvironment, "0") == 0 ||
         std::strcmp(AccountingEnvironment, "1") == 0),
        "GANTRIA_CONTACT_ACCOUNTING must be 0 or 1");
    const bool CaptureAccounting = BackendName == "xpbd" && CaptureTrace &&
        (!AccountingEnvironment || std::strcmp(AccountingEnvironment, "1") == 0);
    const int N = Config.at("Resolution").get<int>();
    const int Frames = Config.at("Frames").get<int>();
    const int Substeps = Config.value("XpbdSubsteps", 32);
    const int Workers = Config.value("Workers", 1);
    Require((N == 9 || N == 17 || N == 32 || N == 128) && Frames > 0 && Frames <= 600 &&
        Substeps > 0 && Substeps <= 64 && Workers >= 1 && Workers <= 8,
        "Contact case exceeds bounded size");
    Require(Config.at("CaseId").is_string() && !Config.at("CaseId").get<std::string>().empty(),
        "Missing CaseId");
    glm::vec3 Gravity = Vec(Config.value("GravityMPerS2", Json::array({0,-9.81,0})));
    const glm::vec3 Origin = Vec(Config.value("OriginM", Json::array({0,3,0})));
    const glm::vec3 Size = Vec(Config.value("SizeM", Json::array({8,8,0.5})));
    glm::vec3 InitialVelocity = Vec(Config.value("InitialVelocityMPerS",
        Json::array({Config.value("InitialSpeedXMPerS", 2.0f), 0.0f, 0.0f})));
    Require(Size.x > 0 && Size.y > 0 && Size.z > 0, "Invalid cloth size");
    const Json RotationValue = Config.value("RotationWxyz", Json::array({1,0,0,0}));
    Require(RotationValue.is_array() && RotationValue.size() == 4, "RotationWxyz needs four numbers");
    const glm::quat Rotation(RotationValue[0].get<float>(), RotationValue[1].get<float>(),
        RotationValue[2].get<float>(), RotationValue[3].get<float>());
    PhysicsEvaluation::SetExperimentalXpbdClothRotation(Rotation);
    PhysicsEvaluation::SetExperimentalJoltClothRotation(Rotation);
    PhysicsEvaluation::SetExperimentalXpbdAccountingEnabled(
        BackendName == "xpbd" && CaptureAccounting);
    const bool RotateAuthoredInputs = Config.value("RotateAuthoredInputs", false);
    if (RotateAuthoredInputs) {
        Gravity = Rotation * Gravity;
        InitialVelocity = Rotation * InitialVelocity;
    }

    SoftBodyDefinition Definition;
    Definition.Kind = SoftBodyKind::Cloth;
    Definition.Position = Origin;
    Definition.Size = Size;
    Definition.ResolutionX = Definition.ResolutionY = static_cast<unsigned>(N);
    Definition.Quality = BackendName == "xpbd" ? SoftBodyQuality::High : SoftBodyQuality::Automatic;
    const Json Material = Config.value("Material", Json::object());
    Definition.Material.ParticleMass = Material.value("ParticleMassKg", Definition.Material.ParticleMass);
    Definition.Material.StretchCompliance = Material.value("StretchCompliance", Definition.Material.StretchCompliance);
    Definition.Material.BendCompliance = Material.value("BendCompliance", Definition.Material.BendCompliance);
    Definition.Material.Friction = Material.value("Friction", Definition.Material.Friction);
    Definition.Material.Thickness = Material.value("ThicknessM", Definition.Material.Thickness);
    const float AuthoredDamping = Material.value("Damping", Definition.Material.Damping);
    Definition.Material.Damping = BackendName == "xpbd" ?
        1.0f - std::pow(1.0f - AuthoredDamping, 1.0f / float(Substeps)) : AuthoredDamping;
    Require(Definition.Material.ParticleMass > 0 && AuthoredDamping >= 0 && AuthoredDamping < 1 &&
        Definition.Material.Thickness >= 0, "Invalid material parameters");
    const Json Pins = Config.value("Pins", Json("top"));
    std::vector<unsigned> PinIndices;
    if (Pins == "top") for (int X = 0; X < N; ++X) PinIndices.push_back(unsigned(X));
    else if (Pins == "corners") PinIndices = {0u, unsigned(N - 1),
        unsigned(N * (N - 1)), unsigned(N * N - 1)};
    else if (Pins == "left") for (int Y = 0; Y < N; ++Y) PinIndices.push_back(unsigned(Y * N));
    else if (Pins == "free") {}
    else if (Pins.is_array()) for (const auto &Pin : Pins) PinIndices.push_back(Pin.get<unsigned>());
    else Require(false, "Pins must be top, left, corners, free, or index array");
    std::sort(PinIndices.begin(), PinIndices.end());
    Require(PinIndices.size() <= 256 &&
        std::adjacent_find(PinIndices.begin(), PinIndices.end()) == PinIndices.end(), "Invalid pin count or duplicate");
    for (unsigned Index : PinIndices) {
        Require(Index < unsigned(N * N), "Pin index out of range");
        const float U = float(Index % N) / float(N - 1) - 0.5f;
        const float V = float(Index / N) / float(N - 1) - 0.5f;
        const glm::vec3 Offset(U * Size.x, -V * Size.y, 0.0f);
        Definition.Attachments.push_back({Index, Origin + Rotation * Offset});
    }

    std::vector<SphereCase> Spheres;
    for (const auto &Item : Config.value("Spheres", Json::array())) {
        Require(Spheres.size() < 8, "Too many spheres");
        SphereCase Sphere;
        Sphere.Id = Item.at("Id").get<std::string>();
        Sphere.Motion = Item.value("Motion", std::string("static"));
        Sphere.KinematicUpdate = Item.value("KinematicUpdate",
            BackendName == "xpbd" ? std::string("pose") : std::string("move"));
        Sphere.Radius = Item.value("RadiusM", 1.0f);
        Sphere.Mass = Item.value("MassKg", 1.0f);
        Sphere.Start = Vec(Item.at("CenterM"));
        const Json Trajectory = Item.value("Trajectory", Json::object());
        Sphere.TrajectoryKind = Trajectory.value("Kind", std::string("hold"));
        Sphere.Velocity = Vec(Trajectory.value("VelocityMPerS", Json::array({0,0,0})));
        Sphere.Duration = Trajectory.value("DurationSeconds", 0.66f);
        Sphere.Frequency = Trajectory.value("FrequencyHz", 1.0f);
        if (RotateAuthoredInputs) {
            Sphere.Start = Origin + Rotation * (Sphere.Start - Origin);
            Sphere.Velocity = Rotation * Sphere.Velocity;
        }
        Require(!Sphere.Id.empty() && Sphere.Radius > 0 && Sphere.Mass >= 0.001f &&
            Sphere.Duration > 0 && Sphere.Frequency > 0 &&
            (Sphere.Motion == "static" || Sphere.Motion == "kinematic" || Sphere.Motion == "dynamic") &&
            (Sphere.KinematicUpdate == "move" || Sphere.KinematicUpdate == "pose") &&
            (Sphere.TrajectoryKind == "hold" || Sphere.TrajectoryKind == "linear" ||
             Sphere.TrajectoryKind == "linear_clamped" || Sphere.TrajectoryKind == "sine"),
            "Invalid sphere case");
        Require(BackendName == "jolt" || Sphere.Motion != "dynamic",
            "XPBD public collider snapshots cannot provide reciprocal dynamic coupling");
        Require(BackendName == "jolt" || Sphere.Motion != "kinematic" ||
            Sphere.KinematicUpdate == "pose",
            "XPBD collider snapshots cannot provide native MoveKinematic semantics");
        Spheres.push_back(Sphere);
    }

    const auto BeforeAdmission = PhysicsEvaluation::CaptureResources();
    SoftBodyWorldLimits Limits;
    Limits.MaximumWorkers = Workers;
    PhysicsWorldConfig WorldConfig;
    WorldConfig.Gravity = Gravity;
    std::unique_ptr<ISoftBodyBackend> Xpbd;
    PhysicsEvaluation::UnifiedJoltProvider Jolt;
    if (BackendName == "xpbd") Xpbd = CreateSoftBodyBackend(Limits);
    else Jolt = PhysicsEvaluation::CreateUnifiedJoltProvider(WorldConfig, Limits, Workers);
    ISoftBodyBackend *Soft = BackendName == "xpbd" ? Xpbd.get() : Jolt.Soft.get();
    Require(Soft && Soft->IsValid(), "Soft provider creation failed");
    const SoftBodyId Cloth = Soft->CreateBody(Definition);
    Require(Cloth.IsValid(), "Cloth admission failed");
    auto Initial = Soft->GetBodyState(Cloth);
    Require(Initial && Initial->Positions && Initial->Indices &&
        Initial->Positions->size() == std::size_t(N * N), "Initial cloth topology missing");
    const std::uint64_t TopologyRevision = Initial->TopologyRevision;
    std::uint64_t VertexRevision = Initial->VertexRevision;
    for (auto &Sphere : Spheres) if (BackendName == "jolt") {
        Sphere.Description.Shape.Kind = PhysicsShapeKind::Ball;
        Sphere.Description.Shape.Size = glm::vec3(2.0f * Sphere.Radius);
        Sphere.Description.Transform.Position = Sphere.Start;
        Sphere.Description.Anchored = Sphere.Motion == "static";
        Sphere.Description.Kinematic = Sphere.Motion == "kinematic";
        Sphere.Description.SleepEnabled = false;
        if (Sphere.Motion == "dynamic")
            Sphere.Description.Density = Sphere.Mass /
                ((4.0f / 3.0f) * 3.14159265358979323846f *
                    Sphere.Radius * Sphere.Radius * Sphere.Radius);
        Sphere.RigidId = Jolt.Rigid->CreateBody(Sphere.Description);
        Require(Sphere.RigidId.IsValid(), "Rigid sphere admission failed");
        if (Sphere.Motion == "dynamic")
            Require(Jolt.Rigid->SetLinearVelocity(Sphere.RigidId, Sphere.Velocity).Succeeded(),
                "Dynamic sphere initial velocity rejected");
    }
    std::filesystem::create_directories(Output);
    if (CaptureTrace) {
        std::ofstream File(Output / "ContactFixtureV1.json");
        Require(bool(File), "Fixture output failed");
        File << MakeFixture(Config, Definition, *Initial, Spheres, Gravity,
            InitialVelocity, AuthoredDamping, Frames).dump(1);
        Require(bool(File), "Fixture write failed");
    }
    Initial.reset();
    const auto Movable = std::size_t(N * N) - Definition.Attachments.size();
    Require(Soft->ApplyImpulse(Cloth, float(Movable) *
        Definition.Material.ParticleMass * InitialVelocity).Succeeded(), "Initial impulse rejected");
    std::ofstream Trace;
    if (CaptureTrace) {
        Trace.open(Output / "ContactTraceV1.jsonl");
        Require(bool(Trace), "Trace output failed");
    }
    std::vector<double> PhysicsMs;
    PhysicsMs.reserve(Frames);
    std::uint64_t StateHash = 14695981039346656037ull;
    double XpbdWorkerUtilizationSum = 0.0;
    std::uint64_t XpbdSubstepCount = 0, XpbdJobsDispatched = 0;
    std::size_t XpbdWorkersActual = 0;
    for (int Frame = 0; Frame < Frames; ++Frame) {
        const auto PhysicsStart = std::chrono::steady_clock::now();
        const float EndTime = float(Frame + 1) * DeltaTime;
        if (BackendName == "jolt") {
            for (auto &Sphere : Spheres) if (Sphere.Motion != "dynamic") {
                const glm::vec3 Center = Sphere.Target(EndTime);
                if (Sphere.Motion == "kinematic" && Sphere.KinematicUpdate == "move")
                    Require(PhysicsEvaluation::MoveExperimentalJoltKinematic(*Jolt.Rigid,
                        Sphere.RigidId, Center, DeltaTime), "Native kinematic move failed");
                else {
                    Sphere.Description.Transform.Position = Center;
                    Require(Jolt.Rigid->UpdateBody(Sphere.RigidId, Sphere.Description).Succeeded(),
                        "Static pose update failed");
                }
            }
            (void)Jolt.Rigid->Step({.DeltaTime = DeltaTime, .SubStepCount = 16});
        }
        const int Steps = BackendName == "xpbd" ? Substeps : 1;
        PhysicsEvaluation::XpbdContactProjection FrameContact;
        SoftBodyState FrameState;
        bool StaticPoseWorkComplete = true;
        for (int Sub = 0; Sub < Steps; ++Sub) {
            SoftBodyStepConfig Step;
            Step.DeltaTime = DeltaTime / float(Steps);
            Step.Gravity = Gravity;
            std::vector<glm::vec3> ColliderDeltas;
            if (BackendName == "xpbd") for (const auto &Sphere : Spheres) {
                SoftBodyCollider Collider;
                Collider.Shape.Kind = PhysicsShapeKind::Ball;
                Collider.Shape.Size = glm::vec3(2.0f * Sphere.Radius);
                const float StartTime = (float(Frame) + float(Sub) / Steps) * DeltaTime;
                const float EndSubstepTime = (float(Frame) + float(Sub + 1) / Steps) * DeltaTime;
                Collider.Transform.Position = Sphere.Target(EndSubstepTime);
                Step.Colliders.push_back(Collider);
                if (Sphere.Motion == "static" &&
                    Sphere.Target(StartTime) != Sphere.Target(EndSubstepTime))
                    StaticPoseWorkComplete = false;
                ColliderDeltas.push_back(Sphere.Motion == "kinematic" ?
                    Sphere.Target(EndSubstepTime) - Sphere.Target(StartTime) : glm::vec3(0.0f));
            }
            if (BackendName == "xpbd")
                PhysicsEvaluation::SetExperimentalXpbdColliderDeltas(std::move(ColliderDeltas));
            const auto Result = Soft->Step(Step);
            Require(!Result.BodiesTruncated && !Result.VerticesTruncated &&
                !Result.CollidersTruncated && Result.Profile.BacklogDrops == 0 &&
                Result.Profile.StaleResults == 0 && Result.States.size() == 1 &&
                Result.Profile.SimulatedBodies == 1 &&
                Result.Profile.SimulatedVertices == std::size_t(N * N) &&
                Result.Profile.FrozenBodies == 0 &&
                Result.States.front().Body == Cloth && Result.States.front().Simulated &&
                Result.States.front().Positions &&
                Result.States.front().Positions->size() == std::size_t(N * N) &&
                Result.States.front().TopologyRevision == TopologyRevision &&
                Result.States.front().VertexRevision > VertexRevision,
                "Soft step lost work");
            VertexRevision = Result.States.front().VertexRevision;
            FrameState = Result.States.front();
            if (BackendName == "xpbd") {
                XpbdWorkerUtilizationSum += Result.Profile.WorkerUtilization;
                XpbdJobsDispatched += Result.Profile.JobsDispatched;
                XpbdWorkersActual = std::max(XpbdWorkersActual, Result.Profile.WorkerCount);
                ++XpbdSubstepCount;
            }
            if (BackendName == "xpbd" && CaptureAccounting) {
                const auto Projection = PhysicsEvaluation::GetExperimentalXpbdContactProjection(*Soft, Cloth);
                Require(Projection.has_value(), "XPBD projection diagnostic missing");
                FrameContact.VelocityDerivedMomentumDeltaNs += Projection->VelocityDerivedMomentumDeltaNs;
                FrameContact.NormalImpulseOnClothNs += Projection->NormalImpulseOnClothNs;
                FrameContact.FrictionImpulseOnClothNs += Projection->FrictionImpulseOnClothNs;
                FrameContact.PrescribedColliderReactionImpulseNs +=
                    Projection->PrescribedColliderReactionImpulseNs;
                if (Sub == 0) FrameContact.MomentumStartKgMPerS = Projection->MomentumStartKgMPerS;
                FrameContact.MomentumEndKgMPerS = Projection->MomentumEndKgMPerS;
                FrameContact.GravityForceImpulseNs += Projection->GravityForceImpulseNs;
                FrameContact.UniformImpulseNs += Projection->UniformImpulseNs;
                FrameContact.PointImpulseNs += Projection->PointImpulseNs;
                FrameContact.DampingImpulseNs += Projection->DampingImpulseNs;
                FrameContact.PinConstraintImpulseOnClothNs +=
                    Projection->PinConstraintImpulseOnClothNs;
                FrameContact.UnpinnedStructuralNumericalImpulseNs +=
                    Projection->UnpinnedStructuralNumericalImpulseNs;
                FrameContact.MomentumResidualNs += Projection->MomentumResidualNs;
                FrameContact.ContactKineticDeltaJ += Projection->ContactKineticDeltaJ;
                FrameContact.PrescribedSurfaceWorkJ += Projection->PrescribedSurfaceWorkJ;
                FrameContact.Corrections += Projection->Corrections;
                if (Sub == 0) FrameContact.KineticStartJ = Projection->KineticStartJ;
                FrameContact.KineticEndJ = Projection->KineticEndJ;
                FrameContact.AppliedDynamicsKineticDeltaJ += Projection->AppliedDynamicsKineticDeltaJ;
                FrameContact.GravityForceKineticDeltaJ += Projection->GravityForceKineticDeltaJ;
                FrameContact.UniformImpulseKineticDeltaJ += Projection->UniformImpulseKineticDeltaJ;
                FrameContact.PointImpulseKineticDeltaJ += Projection->PointImpulseKineticDeltaJ;
                FrameContact.DampingKineticDeltaJ += Projection->DampingKineticDeltaJ;
                FrameContact.StructuralProjectionKineticDeltaJ +=
                    Projection->StructuralProjectionKineticDeltaJ;
                FrameContact.PinProjectionKineticDeltaJ += Projection->PinProjectionKineticDeltaJ;
                FrameContact.LedgerResidualJ += Projection->LedgerResidualJ;
            }
        }
        PhysicsMs.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - PhysicsStart).count());
        for (const auto &Position : *FrameState.Positions) {
            Require(std::isfinite(Position.x) && std::isfinite(Position.y) &&
                std::isfinite(Position.z), "Nonfinite poststep position");
            HashPosition(StateHash, Position);
        }
        if (!CaptureTrace) continue;
        const auto Velocities = BackendName == "xpbd" ?
            PhysicsEvaluation::GetExperimentalXpbdVelocities(*Soft, Cloth) :
            PhysicsEvaluation::GetJoltSoftBodyVelocities(*Jolt.Rigid, *Soft, Cloth);
        Require(Velocities && Velocities->size() == FrameState.Positions->size(),
            "Poststep physical velocities missing");
        Json Record = {{"Frame", Frame}, {"TimeSeconds", EndTime},
            {"Positions", Json::array()}, {"Velocities", Json::array()},
            {"ColliderStates", Json::array()}};
        if (BackendName == "xpbd" && CaptureAccounting) {
            const auto DTriplet = [](glm::dvec3 Value) {
                return Json::array({Value.x, Value.y, Value.z});
            };
            Record["Stages"] = {{"ContactProjectionVelocityDerivedMomentumDeltaNs",
                DTriplet(FrameContact.VelocityDerivedMomentumDeltaNs)},
                {"ContactProjectionCorrections", FrameContact.Corrections},
                {"Interpretation", "exact solver projection contribution to reconstructed velocity"}};
            Record["External"] = {{"XpbdContactAccounting", {
                {"NormalImpulseOnClothNs", DTriplet(FrameContact.NormalImpulseOnClothNs)},
                {"FrictionImpulseOnClothNs", DTriplet(FrameContact.FrictionImpulseOnClothNs)},
                {"PrescribedColliderReactionImpulseNs",
                    DTriplet(FrameContact.PrescribedColliderReactionImpulseNs)},
                {"MomentumStartKgMPerS", DTriplet(FrameContact.MomentumStartKgMPerS)},
                {"MomentumEndKgMPerS", DTriplet(FrameContact.MomentumEndKgMPerS)},
                {"GravityForceImpulseNs", DTriplet(FrameContact.GravityForceImpulseNs)},
                {"UniformImpulseNs", DTriplet(FrameContact.UniformImpulseNs)},
                {"PointImpulseNs", DTriplet(FrameContact.PointImpulseNs)},
                {"DampingImpulseNs", DTriplet(FrameContact.DampingImpulseNs)},
                {"PinConstraintImpulseOnClothNs",
                    DTriplet(FrameContact.PinConstraintImpulseOnClothNs)},
                {"UnpinnedStructuralNumericalImpulseNs",
                    DTriplet(FrameContact.UnpinnedStructuralNumericalImpulseNs)},
                {"MomentumResidualNs", DTriplet(FrameContact.MomentumResidualNs)},
                {"ContactKineticDeltaJ", FrameContact.ContactKineticDeltaJ},
                {"PrescribedSurfaceWorkJ", FrameContact.PrescribedSurfaceWorkJ},
                {"ContactEnergyResidualJ", FrameContact.ContactKineticDeltaJ -
                    FrameContact.PrescribedSurfaceWorkJ},
                {"KineticStartJ", FrameContact.KineticStartJ},
                {"KineticEndJ", FrameContact.KineticEndJ},
                {"AppliedDynamicsKineticDeltaJ", FrameContact.AppliedDynamicsKineticDeltaJ},
                {"GravityForceKineticDeltaJ", FrameContact.GravityForceKineticDeltaJ},
                {"UniformImpulseKineticDeltaJ", FrameContact.UniformImpulseKineticDeltaJ},
                {"PointImpulseKineticDeltaJ", FrameContact.PointImpulseKineticDeltaJ},
                {"DampingKineticDeltaJ", FrameContact.DampingKineticDeltaJ},
                {"StructuralProjectionKineticDeltaJ",
                    FrameContact.StructuralProjectionKineticDeltaJ},
                {"PinProjectionKineticDeltaJ", FrameContact.PinProjectionKineticDeltaJ},
                {"LedgerResidualJ", FrameContact.LedgerResidualJ},
                {"SubstepSeconds", DeltaTime / float(Substeps)},
                {"PrescribedSurfaceVelocitySource", "center(end)-center(start) per substep"},
                {"Corrections", FrameContact.Corrections},
                {"StaticPoseWorkComplete", StaticPoseWorkComplete},
                {"Method", "source-contact-projection-and-previous-position-deltas"},
                {"Scope", "one-way prescribed sphere; no dynamic rigid momentum claim"}}}};
        }
        for (std::size_t Index = 0; Index < Velocities->size(); ++Index) {
            for (float Value : {(*FrameState.Positions)[Index].x, (*FrameState.Positions)[Index].y,
                                (*FrameState.Positions)[Index].z}) {
                Require(std::isfinite(Value), "Nonfinite poststep position");
                Record["Positions"].push_back(Value);
            }
            for (float Value : {(*Velocities)[Index].x, (*Velocities)[Index].y,
                                (*Velocities)[Index].z}) {
                Require(std::isfinite(Value), "Nonfinite poststep velocity");
                Record["Velocities"].push_back(Value);
            }
        }
        for (const auto &Sphere : Spheres) {
            glm::vec3 Center = Sphere.Target(EndTime);
            glm::vec3 LinearVelocity(0.0f), AngularVelocity(0.0f);
            glm::quat Orientation(1,0,0,0);
            if (BackendName == "jolt") {
                const auto RigidState = Jolt.Rigid->GetBodyState(Sphere.RigidId);
                Require(RigidState.has_value(), "Actual rigid state missing");
                Center = RigidState->Transform.Position;
                LinearVelocity = RigidState->LinearVelocity;
                AngularVelocity = RigidState->AngularVelocity;
                Orientation = RigidState->Transform.ToQuaternion();
            } else if (Sphere.Motion == "kinematic")
                LinearVelocity = (Sphere.Target(EndTime) - Sphere.Target(float(Frame) * DeltaTime)) / DeltaTime;
            Record["ColliderStates"].push_back({{"Id", Sphere.Id}, {"Center", Triplet(Center)},
                {"OrientationWxyz", QuatValues(Orientation)},
                {"LinearVelocity", Triplet(LinearVelocity)},
                {"AngularVelocity", Triplet(AngularVelocity)},
                {"VelocityMethod", BackendName == "jolt" ? "native-state" :
                    Sphere.Motion == "kinematic" ? "prescribed-frame-pose-difference" :
                    "static-pose-snapshot"}});
        }
        Trace << Record.dump() << '\n';
        Require(bool(Trace), "Trace write failed");
    }
    if (CaptureTrace) {
        Trace.close();
        Require(!Trace.fail(), "Trace close failed");
    }
    const auto AfterRun = PhysicsEvaluation::CaptureResources();
    Xpbd.reset();
    Jolt.Soft.reset();
    Jolt.Rigid.reset();
    const auto AfterRetire = PhysicsEvaluation::CaptureResources();
    std::cout << "[Physics:ContactQualification] case=" << Config.at("CaseId")
        << " backend=" << BackendName << " frames=" << Frames << " vertices=" << N*N
        << " result=" << (CaptureTrace ? "TRACE_WRITTEN" : "TIMING_ONLY")
        << " accounting=" << int(CaptureAccounting)
        << " state_hash=" << StateHash << '\n';
    std::sort(PhysicsMs.begin(), PhysicsMs.end());
    const auto Percentile = [&](double Fraction) { return PhysicsMs[std::size_t(
        std::ceil(Fraction * PhysicsMs.size()) - 1)]; };
    std::cout << "[Physics:ContactQualification] "
        << (CaptureTrace ? "diagnostic" : "clean") << "_physics_ms_p50=" << Percentile(0.50)
        << " p95=" << Percentile(0.95) << " p99=" << Percentile(0.99)
        << " excludes_json_and_hash=1 quality_capture_not_ranked=" << int(CaptureTrace)
        << " xpbd_workers_actual=" << XpbdWorkersActual
        << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
        << " xpbd_worker_utilization_mean=" <<
            (XpbdSubstepCount ? XpbdWorkerUtilizationSum / double(XpbdSubstepCount) : 0.0)
        << '\n';
    std::cout << "[Physics:ContactQualification] resources " << Json({
        {"AdmissionAndRun", PhysicsEvaluation::ResourceDelta(BeforeAdmission, AfterRun)},
        {"Retirement", PhysicsEvaluation::ResourceDelta(AfterRun, AfterRetire)},
        {"WholeProcess", PhysicsEvaluation::ResourceDelta(BeforeAdmission, AfterRetire)},
        {"StateHash", StateHash}, {"Accounting", CaptureAccounting},
        {"CaptureTrace", CaptureTrace}}).dump() << '\n';
    return 0;
}
} // namespace

int main(int Count, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        if (Count == 2 && std::string(Arguments[1]) == "--self-test") {
            const glm::vec3 Normal(1,0,0);
            const auto Resolve = [&](glm::vec3 Cloth, glm::vec3 Surface, float Friction) {
                return PhysicsEvaluation::ResolveExperimentalXpbdRelativeMovement(
                    Cloth, Surface, Normal, Friction);
            };
            Require(glm::length(Resolve({0.2f,0,0}, {0.1f,0,0}, 0.5f) -
                glm::vec3(0.2f,0,0)) < 1e-6f, "Separating relative normal velocity was removed");
            Require(glm::length(Resolve({0,0,0}, {0.1f,0,0}, 0.5f) -
                glm::vec3(0.1f,0,0)) < 1e-6f, "Approaching relative normal velocity survived");
            Require(glm::length(Resolve({0,0.2f,0}, {0,0.1f,0}, 0.5f) -
                glm::vec3(0,0.15f,0)) < 1e-6f, "Tangential relative friction is incorrect");
            std::cout << "[Physics:ContactQualification] relative_response_self_test=PASS\n";
            return 0;
        }
        if (Count != 3) throw std::invalid_argument("Usage: PhysicsJoltContactQualification Case.json OutputDirectory");
        return Run(Arguments[1], Arguments[2]);
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:ContactQualification] " << Error.what() << '\n';
        return 1;
    }
}

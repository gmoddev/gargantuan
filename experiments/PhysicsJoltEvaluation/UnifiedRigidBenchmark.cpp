#include "RigidJoltBackend.hpp"
#include "ExperimentSupport.hpp"

#include "gargantuan/physics/PhysicsBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace PhysicsEvaluation {
namespace {
    using namespace gargantuan;
    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::json;

    struct Options {
        std::string Backend = "box3d";
        std::string Case = "all";
        int Workers = 1;
        bool Quick = false;
        int WorldUpdates = 1;
        int CollisionSteps = 4;
    };
    int SelectedWorldUpdates = 1;
    int SelectedCollisionSteps = 4;

    void Require(bool Condition, std::string_view Message) {
        if (!Condition) throw std::runtime_error("[Physics:JoltRigid] " + std::string(Message));
    }
    double Milliseconds(Clock::duration Duration) {
        return std::chrono::duration<double, std::milli>(Duration).count();
    }
    Json Timings(const std::vector<double> &Values) {
        if (Values.empty()) return nullptr;
        auto Sorted = Values;
        std::ranges::sort(Sorted);
        auto At = [&](double Fraction) {
            const auto Index = static_cast<std::size_t>(std::ceil(Fraction * Sorted.size())) - 1;
            return Sorted[std::min(Index, Sorted.size() - 1)];
        };
        return {{"P50Ms", At(0.50)}, {"P95Ms", At(0.95)}, {"P99Ms", At(0.99)},
            {"MaximumMs", Sorted.back()}, {"RawMs", Values}};
    }
    PhysicsBodyDesc Body(glm::vec3 Position, glm::vec3 Size, bool Anchored,
                         PhysicsShapeKind Kind = PhysicsShapeKind::Box) {
        PhysicsBodyDesc Result;
        Result.Transform = CFrame(Position);
        Result.Shape.Kind = Kind;
        Result.Shape.Size = Size;
        Result.Anchored = Anchored;
        Result.SleepEnabled = true;
        return Result;
    }
    PhysicsBodyDesc JointBody(glm::vec3 Position, glm::vec3 Size, bool Anchored) {
        auto Result = Body(Position, Size, Anchored);
        Result.CanCollide = false;
        Result.CanTouch = false;
        return Result;
    }
    std::unique_ptr<IPhysicsBackend> GetBackend(const Options &Selection, glm::vec3 Gravity) {
        const PhysicsWorldConfig Config{.Gravity = Gravity};
        auto Backend = Selection.Backend == "jolt"
            ? CreateRigidJoltBackend(Config, Selection.Workers) : CreatePhysicsBackend(Config);
        Require(Backend && Backend->IsValid(), "rigid backend unavailable");
        return Backend;
    }
    PhysicsBodyId Add(IPhysicsBackend &World, const PhysicsBodyDesc &Description) {
        const auto Id = World.CreateBody(Description);
        Require(Id.IsValid(), "body admission failed");
        return Id;
    }
    void Retire(IPhysicsBackend &World, const std::vector<PhysicsBodyId> &Bodies) {
        for (auto Id : Bodies) {
            Require(World.DestroyBody(Id).Succeeded(), "body retirement failed");
            Require(!World.IsBodyValid(Id), "stale body ID remained valid");
        }
    }
    void FiniteState(const IPhysicsBackend &World, PhysicsBodyId Id) {
        const auto State = World.GetBodyState(Id);
        Require(State.has_value(), "body state unavailable");
        const auto &P = State->Transform.Position;
        Require(std::isfinite(P.x) && std::isfinite(P.y) && std::isfinite(P.z), "nonfinite body position");
        const auto &V = State->LinearVelocity;
        Require(std::isfinite(V.x) && std::isfinite(V.y) && std::isfinite(V.z), "nonfinite body velocity");
    }
    void UnitNormal(const PhysicsRaycastHit &Hit) {
        const auto &N = Hit.Normal;
        const float Length = glm::length(N);
        Require(std::isfinite(N.x) && std::isfinite(N.y) && std::isfinite(N.z) &&
            std::isfinite(Length) && std::abs(Length - 1.0f) < 0.01f,
            "query returned nonfinite or nonunit hit normal");
    }
    void Step(IPhysicsBackend &World, std::vector<double> &Samples,
              std::optional<PhysicsBodyId> RequiredMotion = std::nullopt) {
        double TotalStepMs = 0.0;
        for (int Update = 0; Update < SelectedWorldUpdates; ++Update) {
            const auto Started = Clock::now();
            const auto Result = World.Step({.DeltaTime = (1.0f / 60.0f) / SelectedWorldUpdates,
                .SubStepCount = SelectedCollisionSteps});
            TotalStepMs += Milliseconds(Clock::now() - Started);
            Require(!Result.EventsTruncated, "physics step truncated contact history");
            std::set<PhysicsBodyId> MotionIds;
            for (const auto &Motion : Result.Motions) {
                Require(World.IsBodyValid(Motion.Body), "step returned an invalid motion body");
                Require(MotionIds.insert(Motion.Body).second, "step returned duplicate body motion");
                const auto &Position = Motion.Transform.Position;
                Require(std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z),
                    "step returned nonfinite motion position");
                for (int Column = 0; Column < 3; ++Column)
                    for (int Row = 0; Row < 3; ++Row)
                        Require(std::isfinite(Motion.Transform.Rotation[Column][Row]),
                            "step returned nonfinite motion rotation");
            }
            for (const auto &Contact : Result.Contacts)
                Require(World.IsBodyValid(Contact.BodyA) && World.IsBodyValid(Contact.BodyB) &&
                    Contact.BodyA != Contact.BodyB, "step returned an invalid contact pair");
            if (RequiredMotion)
                Require(MotionIds.contains(*RequiredMotion), "step omitted required moving body");
        }
        Samples.push_back(TotalStepMs);
    }
    Json RunBoxScene(const Options &Selection, std::string_view Name) {
        const auto Before = CaptureResources();
        const auto InitStart = Clock::now();
        const bool StaticOnly = Name == "static-world";
        const bool MostlyStatic = Name == "mostly-static";
        const bool Dense = Name == "dense-contacts";
        auto World = GetBackend(Selection, {0.0f, -196.2f, 0.0f});
        std::vector<PhysicsBodyId> Bodies;
        Bodies.push_back(Add(*World, Body({0.0f, -0.5f, 0.0f}, {200.0f, 1.0f, 200.0f}, true)));
        const int StaticCount = StaticOnly ? (Selection.Quick ? 100 : 400)
            : MostlyStatic ? (Selection.Quick ? 100 : 1000) : 0;
        const int DynamicCount = StaticOnly ? 0 : MostlyStatic ? 10 : (Selection.Quick ? 32 : 128);
        for (int Index = 0; Index < StaticCount; ++Index) {
            const float X = float(Index % 40) * 3.0f - 58.0f;
            const float Z = float(Index / 40) * 3.0f - 30.0f;
            Bodies.push_back(Add(*World, Body({X, 0.5f, Z}, {1, 1, 1}, true)));
        }
        std::vector<PhysicsBodyId> Dynamic;
        for (int Index = 0; Index < DynamicCount; ++Index) {
            const float Gap = Dense ? 0.8f : 1.02f;
            const float X = float(Index % 8) * Gap - 3.5f;
            const float Z = float((Index / 8) % 8) * Gap - 3.5f;
            const float Y = 1.5f + float(Index / 64) * Gap;
            const auto Id = Add(*World, Body({X, Y, Z}, {1, 1, 1}, false));
            Dynamic.push_back(Id); Bodies.push_back(Id);
        }
        const auto AfterInit = CaptureResources();
        const double InitMs = Milliseconds(Clock::now() - InitStart);
        const int Warmup = Selection.Quick ? 2 : 30;
        const int Frames = Selection.Quick ? 8 : 120;
        std::vector<double> Measured;
        for (int Index = 0; Index < Warmup + Frames; ++Index) {
            std::vector<double> One;
            Step(*World, One, Index == 0 && !Dynamic.empty()
                ? std::optional<PhysicsBodyId>(Dynamic.front()) : std::nullopt);
            if (Index >= Warmup) Measured.push_back(One.front());
        }
        float MinimumY = std::numeric_limits<float>::infinity();
        std::vector<glm::vec3> FinalPositions;
        FinalPositions.reserve(Dynamic.size());
        for (auto Id : Dynamic) {
            FiniteState(*World, Id);
            const auto State = World->GetBodyState(Id);
            MinimumY = std::min(MinimumY, State->Transform.Position.y);
            FinalPositions.push_back(State->Transform.Position);
            Require(std::abs(State->Mass - 0.7f) < 0.01f, "dynamic box mass differs from neutral density and geometry");
        }
        Require(Dynamic.empty() || MinimumY > 0.35f, "dynamic box penetrated floor");
        float MaximumPairPenetration = 0.0f;
        for (std::size_t First = 0; First < FinalPositions.size(); ++First)
            for (std::size_t Second = First + 1; Second < FinalPositions.size(); ++Second) {
                const auto Delta = glm::abs(FinalPositions[First] - FinalPositions[Second]);
                if (Delta.x < 1.0f && Delta.y < 1.0f && Delta.z < 1.0f)
                    MaximumPairPenetration = std::max(MaximumPairPenetration,
                        std::min({1.0f - Delta.x, 1.0f - Delta.y, 1.0f - Delta.z}));
            }
        Require(MaximumPairPenetration < 0.25f, "stack contains materially interpenetrating bodies");
        const PhysicsRaycastRequest Ray{.Origin = {0.0f, 20.0f, 0.0f}, .Direction = {0.0f, -40.0f, 0.0f}};
        std::vector<double> Query;
        for (int Index = 0; Index < (Selection.Quick ? 8 : 128); ++Index) {
            const auto Started = Clock::now();
            const auto Hit = World->Raycast(Ray);
            Query.push_back(Milliseconds(Clock::now() - Started));
            Require(Hit.Succeeded() && Hit.HasHit(), "downward ray missed scene");
        }
        const auto AfterRun = CaptureResources();
        const auto RetireStart = Clock::now();
        Retire(*World, Bodies);
        World.reset();
        const double TeardownMs = Milliseconds(Clock::now() - RetireStart);
        const auto AfterRetire = CaptureResources();
        return {{"Case", std::string(Name)}, {"Status", "measured"}, {"Bodies", Bodies.size()},
            {"DynamicBodies", DynamicCount}, {"StaticBodies", StaticCount + 1},
            {"WarmupFrames", Warmup}, {"MeasuredFrames", Frames},
            {"InitMs", InitMs}, {"Step", Timings(Measured)}, {"Raycast", Timings(Query)},
            {"TeardownMs", TeardownMs}, {"MinimumDynamicCenterY", Dynamic.empty() ? Json(nullptr) : Json(MinimumY)},
            {"MaximumPairPenetration", MaximumPairPenetration},
            {"Quality", {{"Status", "pass"}, {"Checks", {"finite states", "mass parity",
                "floor separation", "pair penetration", "ray hits", "identity retirement"}}}},
            {"ResourcesInit", ResourceDelta(Before, AfterInit)},
            {"ResourcesRun", ResourceDelta(AfterInit, AfterRun)},
            {"ResourcesRetire", ResourceDelta(AfterRun, AfterRetire)}};
    }
    Json RunChurn(const Options &Selection) {
        auto World = GetBackend(Selection, {0, 0, 0});
        const int Cycles = Selection.Quick ? 2 : 30;
        std::vector<double> Create, StepTimes, Destroy;
        const auto Before = CaptureResources();
        for (int Cycle = 0; Cycle < Cycles; ++Cycle) {
            std::vector<PhysicsBodyId> Bodies;
            const auto Started = Clock::now();
            for (int Index = 0; Index < 32; ++Index)
                Bodies.push_back(Add(*World, Body({float(Index * 3), 5, 0}, {1, 1, 1}, false)));
            Create.push_back(Milliseconds(Clock::now() - Started));
            Step(*World, StepTimes);
            const auto RetireStart = Clock::now();
            Retire(*World, Bodies);
            Destroy.push_back(Milliseconds(Clock::now() - RetireStart));
        }
        const auto After = CaptureResources();
        return {{"Case", "body-churn"}, {"Status", "measured"}, {"Cycles", Cycles},
            {"BodiesPerCycle", 32}, {"Create", Timings(Create)}, {"Step", Timings(StepTimes)},
            {"Destroy", Timings(Destroy)},
            {"Quality", {{"Status", "pass"}, {"Checks", {"all bodies admitted", "stale IDs rejected each cycle"}}}},
            {"Resources", ResourceDelta(Before, After)}};
    }
    Json RunSleep(const Options &Selection) {
        auto World = GetBackend(Selection, {0, -10, 0});
        const auto Floor = Add(*World, Body({0, -0.5f, 0}, {10, 1, 10}, true));
        const auto Ball = Add(*World, Body({0, 2, 0}, {1, 1, 1}, false, PhysicsShapeKind::Ball));
        std::vector<double> Samples;
        for (int Index = 0; Index < (Selection.Quick ? 60 : 360); ++Index) Step(*World, Samples);
        FiniteState(*World, Ball);
        const bool Slept = !World->GetBodyState(Ball)->Awake;
        Require(World->SetAwake(Ball, true).Succeeded(), "wake request failed");
        const bool Woke = World->GetBodyState(Ball)->Awake;
        Retire(*World, {Ball, Floor});
        return {{"Case", "sleep-wake"}, {"Status", "measured"}, {"Step", Timings(Samples)},
            {"SleptNaturally", Slept}, {"Woke", Woke},
            {"Quality", {{"Status", Slept && Woke ? "pass" : "fail"},
                {"Checks", {"finite body", "natural sleep", "explicit wake", "retirement"}}}}};
    }
    std::shared_ptr<const PhysicsTriangleMesh> GetGridMesh(int Cells) {
        auto Positions = std::make_shared<std::vector<glm::vec3>>();
        auto Indices = std::make_shared<std::vector<std::uint32_t>>();
        for (int Z = 0; Z <= Cells; ++Z)
            for (int X = 0; X <= Cells; ++X)
                Positions->push_back({2.0f * X / Cells - 1, 0, 2.0f * Z / Cells - 1});
        for (int Z = 0; Z < Cells; ++Z)
            for (int X = 0; X < Cells; ++X) {
                const auto A = std::uint32_t(Z * (Cells + 1) + X);
                const auto B = A + std::uint32_t(Cells + 1);
                Indices->insert(Indices->end(), {A, B, B + 1, A, B + 1, A + 1});
            }
        return std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
            .ResourceId = "asset://physics-jolt-rigid-grid", .ContentRevision = 1,
            .Positions = Positions, .Indices = Indices});
    }
    Json RunMesh(const Options &Selection) {
        auto World = GetBackend(Selection, {0, -10, 0});
        auto Mesh = GetGridMesh(Selection.Quick ? 8 : 16);
        auto FloorDescription = Body({0, 0, 0}, {20, 1, 20}, true, PhysicsShapeKind::StaticTriangleMesh);
        FloorDescription.Shape.Mesh = Mesh;
        const auto Before = CaptureResources();
        const auto Started = Clock::now();
        const auto Floor = Add(*World, FloorDescription);
        const double MeshCreateMs = Milliseconds(Clock::now() - Started);
        const auto Ball = Add(*World, Body({0, 3, 0}, {1, 1, 1}, false, PhysicsShapeKind::Ball));
        std::vector<double> Samples;
        for (int Index = 0; Index < (Selection.Quick ? 30 : 180); ++Index)
            Step(*World, Samples, Index == 0 ? std::optional<PhysicsBodyId>(Ball) : std::nullopt);
        const auto Final = World->GetBodyState(Ball);
        Require(Final && std::isfinite(Final->Transform.Position.y) && Final->Transform.Position.y > 0.35f,
            "dynamic ball missed mesh support");
        const auto Hit = World->Raycast({.Origin = {0, 5, 0}, .Direction = {0, -10, 0}});
        Require(Hit.Succeeded() && Hit.HasHit(), "ray missed mesh support");
        Retire(*World, {Ball, Floor});
        const auto After = CaptureResources();
        return {{"Case", "mesh-contact"}, {"Status", "measured"},
            {"Triangles", Mesh->Indices->size() / 3}, {"MeshCreateMs", MeshCreateMs},
            {"Step", Timings(Samples)}, {"FinalBallY", Final->Transform.Position.y},
            {"Quality", {{"Status", "pass"}, {"Checks", {"mesh support", "ray support", "retirement"}}}},
            {"Resources", ResourceDelta(Before, After)}};
    }
    Json RunQueries(const Options &Selection) {
        auto World = GetBackend(Selection, {0, 0, 0});
        const auto A = Add(*World, Body({5, 0, 0}, {2, 2, 2}, true));
        const auto B = Add(*World, Body({5, 0, 0}, {2, 2, 2}, true));
        std::vector<double> Rays;
        Json ShapeRows = Json::array();
        const int Count = Selection.Quick ? 8 : 128;
        for (int Index = 0; Index < Count; ++Index) {
            const auto Started = Clock::now();
            const auto Ray = World->Raycast({.Origin = {0, 0, 0}, .Direction = {10, 0, 0}});
            Rays.push_back(Milliseconds(Clock::now() - Started));
            Require(Ray.Succeeded() && Ray.HasHit(), "ray missed query target");
            for (const auto &Hit : Ray.Candidates) UnitNormal(Hit);
        }
        for (const auto Kind : {PhysicsQueryShapeKind::Sphere, PhysicsQueryShapeKind::Capsule,
                PhysicsQueryShapeKind::Box}) {
            std::vector<double> Casts, Overlaps;
            for (int Index = 0; Index < Count; ++Index) {
                PhysicsShapeCastRequest Cast;
                Cast.Shape.Kind = Kind;
                Cast.Shape.Radius = 0.5f;
                Cast.Shape.Height = 2.0f;
                Cast.Shape.Size = {1, 2, 1};
                Cast.Direction = {10, 0, 0};
                auto Started = Clock::now();
                const auto CastResult = World->ShapeCast(Cast);
                Casts.push_back(Milliseconds(Clock::now() - Started));
                Require(CastResult.Succeeded() && CastResult.HasHit(), "shape cast missed query target");
                const auto Nearest = std::min_element(CastResult.Candidates.begin(), CastResult.Candidates.end(),
                    [](const auto &Left, const auto &Right) { return Left.Distance < Right.Distance; });
                for (const auto &Hit : CastResult.Candidates) UnitNormal(Hit);
                Require(std::abs(Nearest->Distance - 3.5f) < 0.15f,
                    "shape cast hit distance differs from geometry");
                PhysicsOverlapRequest Overlap;
                Overlap.Shape.Kind = Kind;
                Overlap.Shape.Transform = CFrame(5, 0, 0);
                Overlap.Shape.Radius = 0.5f;
                Overlap.Shape.Height = 2.0f;
                Overlap.Shape.Size = {1, 2, 1};
                Started = Clock::now();
                const auto Result = World->Overlap(Overlap);
                Overlaps.push_back(Milliseconds(Clock::now() - Started));
                Require(Result.Succeeded() && Result.Bodies.size() == 2 &&
                    Result.Bodies[0] == A && Result.Bodies[1] == B, "overlap ordering or count failed");
            }
            ShapeRows.push_back({{"Shape", Kind == PhysicsQueryShapeKind::Sphere ? "Sphere"
                : Kind == PhysicsQueryShapeKind::Capsule ? "Capsule" : "Box"},
                {"Cast", Timings(Casts)}, {"Overlap", Timings(Overlaps)}});
        }
        Retire(*World, {A, B});
        return {{"Case", "advanced-queries"}, {"Status", "measured"},
            {"Iterations", Count}, {"Raycast", Timings(Rays)}, {"Shapes", ShapeRows},
            {"Quality", {{"Status", "pass"}, {"Checks", {"ray hit", "three cast distances",
                "three overlap orderings", "retirement"}}}}};
    }
    Json RunMeshQuery(const Options &Selection) {
        auto World = GetBackend(Selection, {0, 0, 0});
        auto Mesh = GetGridMesh(Selection.Quick ? 8 : 16);
        auto Description = Body({0, 0, 0}, {20, 1, 20}, true, PhysicsShapeKind::StaticTriangleMesh);
        Description.Shape.Mesh = Mesh;
        const auto Id = Add(*World, Description);
        const int Count = Selection.Quick ? 8 : 128;
        std::vector<double> Rays, Casts, Overlaps;
        for (int Index = 0; Index < Count; ++Index) {
            auto Started = Clock::now();
            const auto Ray = World->Raycast({.Origin = {0, 5, 0}, .Direction = {0, -10, 0}});
            Rays.push_back(Milliseconds(Clock::now() - Started));
            Require(Ray.Succeeded() && Ray.Candidates.size() == 1 && Ray.Candidates[0].Body == Id &&
                std::abs(Ray.Candidates[0].Distance - 5.0f) < 0.1f,
                "mesh ray did not deduplicate native triangle hits");
            UnitNormal(Ray.Candidates[0]);
            PhysicsShapeCastRequest Cast;
            Cast.Shape.Kind = PhysicsQueryShapeKind::Sphere;
            Cast.Shape.Transform = CFrame(0, 5, 0);
            Cast.Shape.Radius = 0.5f;
            Cast.Direction = {0, -10, 0};
            Started = Clock::now();
            const auto Hit = World->ShapeCast(Cast);
            Casts.push_back(Milliseconds(Clock::now() - Started));
            Require(Hit.Succeeded() && Hit.Candidates.size() == 1 && Hit.Candidates[0].Body == Id &&
                std::abs(Hit.Candidates[0].Distance - 4.5f) < 0.2f,
                "mesh cast did not deduplicate native triangle hits");
            UnitNormal(Hit.Candidates[0]);
            PhysicsOverlapRequest Overlap;
            Overlap.Shape.Kind = PhysicsQueryShapeKind::Sphere;
            Overlap.Shape.Transform = CFrame(0, 0.1f, 0);
            Overlap.Shape.Radius = 0.5f;
            Started = Clock::now();
            const auto Result = World->Overlap(Overlap);
            Overlaps.push_back(Milliseconds(Clock::now() - Started));
            Require(Result.Succeeded() && Result.Bodies.size() == 1 && Result.Bodies[0] == Id &&
                !Result.Truncated, "mesh overlap did not deduplicate native subshape hits");
        }
        Retire(*World, {Id});
        return {{"Case", "mesh-query-dedup"}, {"Status", "measured"},
            {"Triangles", Mesh->Indices->size() / 3}, {"Iterations", Count},
            {"Raycast", Timings(Rays)}, {"SphereCast", Timings(Casts)},
            {"SphereOverlap", Timings(Overlaps)},
            {"Quality", {{"Status", "pass"}, {"Checks", {"one semantic body per native mesh ray/cast/overlap",
                "finite query geometry", "no truncation", "retirement"}}}}};
    }
    Json RunFilter(const Options &Selection) {
        auto World = GetBackend(Selection, {0, 0, 0});
        auto Target = Body({5, 0, 0}, {2, 2, 2}, true);
        Target.CollisionGroup = 2;
        Target.CollisionMask = std::uint16_t{1} << 1;
        const auto Id = Add(*World, Target);
        PhysicsRaycastRequest Ray{.Origin = {0, 0, 0}, .Direction = {10, 0, 0}};
        Ray.Filter.CollisionGroup = 1;
        Ray.Filter.CollisionMask = std::uint16_t{1} << 2;
        std::vector<double> Query, Update;
        const int Count = Selection.Quick ? 4 : 128;
        for (int Index = 0; Index < Count; ++Index) {
            Target.CollisionGroup = Index % 2 == 0 ? 2 : 3;
            const auto Started = Clock::now();
            Require(World->UpdateBody(Id, Target).Succeeded(), "filter update failed");
            Update.push_back(Milliseconds(Clock::now() - Started));
            const auto QueryStart = Clock::now();
            const auto Hit = World->Raycast(Ray);
            Query.push_back(Milliseconds(Clock::now() - QueryStart));
            Require(Hit.Succeeded() && Hit.HasHit() == (Target.CollisionGroup == 2),
                "mutual collision mask failed to gate ray");
        }
        Retire(*World, {Id});
        return {{"Case", "filter-updates"}, {"Status", "measured"}, {"Iterations", Count},
            {"Update", Timings(Update)}, {"Raycast", Timings(Query)},
            {"Quality", {{"Status", "pass"}, {"Checks", {"mutual mask toggled ray visibility", "retirement"}}}}};
    }
    Json RunCcd(const Options &Selection, bool Enabled) {
        auto World = GetBackend(Selection, {0, 0, 0});
        const int Trials = Selection.Quick ? 2 : 32;
        int Stopped = 0;
        std::vector<double> Samples, Create, Destroy;
        for (int Index = 0; Index < Trials; ++Index) {
            auto Wall = Body({0, 0, 0}, {0.1f, 4, 4}, false);
            auto Ball = Body({-3, 0, 0}, {0.5f, 0.5f, 0.5f}, false, PhysicsShapeKind::Ball);
            Ball.MovingBodyCCD = Enabled;
            const auto Started = Clock::now();
            const auto WallId = Add(*World, Wall);
            const auto BallId = Add(*World, Ball);
            Create.push_back(Milliseconds(Clock::now() - Started));
            Require(World->SetLinearVelocity(WallId, {-2, 0, 0}).Succeeded(), "CCD wall velocity rejected");
            Require(World->SetLinearVelocity(BallId, {360, 0, 0}).Succeeded(), "CCD projectile velocity rejected");
            Step(*World, Samples, BallId);
            const auto State = World->GetBodyState(BallId);
            Require(State && std::isfinite(State->Transform.Position.x), "nonfinite CCD result");
            if (State->Transform.Position.x < 1.0f) ++Stopped;
            const auto RetireStart = Clock::now();
            Retire(*World, {BallId, WallId});
            Destroy.push_back(Milliseconds(Clock::now() - RetireStart));
        }
        return {{"Case", Enabled ? "moving-ccd-on" : "moving-ccd-off"}, {"Status", "measured"},
            {"Trials", Trials}, {"ProjectileSpeed", 360.0}, {"TargetSpeed", -2.0},
            {"Create", Timings(Create)}, {"Step", Timings(Samples)}, {"Destroy", Timings(Destroy)},
            {"StoppedBeforeX1", Stopped},
            {"Quality", {{"Status", !Enabled || Stopped == Trials ? "pass" : "fail"},
                {"Checks", Enabled ? Json::array({"finite pose", "all moving-target projectiles stopped", "retirement"})
                    : Json::array({"finite pose", "observed stop count", "retirement"})}}}};
    }
    Json RunJoints(const Options &Selection, std::string_view Name) {
        auto World = GetBackend(Selection, {0, 0, 0});
        std::vector<PhysicsBodyId> Bodies;
        std::vector<PhysicsConstraintId> Joints;
        const bool Motor = Name == "joint-motors";
        if (Motor) {
            Bodies.push_back(Add(*World, JointBody({0, 0, 0}, {1, 1, 1}, true)));
            Bodies.push_back(Add(*World, JointBody({2, 0, 0}, {1, 1, 1}, false)));
            Bodies.push_back(Add(*World, JointBody({10, 0, 0}, {1, 1, 1}, true)));
            Bodies.push_back(Add(*World, JointBody({10, 0, 0}, {1, 1, 1}, false)));
            PhysicsConstraintDesc Hinge;
            Hinge.Kind = PhysicsConstraintKind::Hinge;
            Hinge.BodyA = Bodies[0]; Hinge.BodyB = Bodies[1];
            Hinge.LocalFrameA = CFrame(0.5f, 0, 0); Hinge.LocalFrameB = CFrame(-1.5f, 0, 0);
            Hinge.EnableLimit = true; Hinge.LowerLimit = -0.4f; Hinge.UpperLimit = 0.4f;
            Hinge.EnableMotor = true; Hinge.MotorSpeed = 2.0f; Hinge.MaximumMotorForce = 1000.0f;
            Joints.push_back(World->CreateConstraint(Hinge));
            PhysicsConstraintDesc Slider;
            Slider.Kind = PhysicsConstraintKind::Slider;
            Slider.BodyA = Bodies[2]; Slider.BodyB = Bodies[3];
            Slider.EnableLimit = true; Slider.LowerLimit = -1; Slider.UpperLimit = 1;
            Slider.EnableMotor = true; Slider.MotorSpeed = 3; Slider.MaximumMotorForce = 1000;
            Joints.push_back(World->CreateConstraint(Slider));
        } else if (Name == "spring-distance") {
            Bodies.push_back(Add(*World, JointBody({0, 0, 0}, {0.5f, 0.5f, 0.5f}, true)));
            Bodies.push_back(Add(*World, JointBody({0, -2, 0}, {0.5f, 0.5f, 0.5f}, false)));
            PhysicsConstraintDesc Spring;
            Spring.Kind = PhysicsConstraintKind::Distance;
            Spring.BodyA = Bodies[0]; Spring.BodyB = Bodies[1];
            Spring.RestLength = 2.0f; Spring.SpringFrequency = 2.0f; Spring.SpringDamping = 0.7f;
            Joints.push_back(World->CreateConstraint(Spring));
            Require(World->ApplyLinearImpulse(Bodies[1], {0, -2, 0}).Succeeded(), "spring impulse failed");
        } else {
            Bodies.push_back(Add(*World, JointBody({0, 5, 0}, {0.5f, 0.5f, 0.5f}, true)));
            for (int Index = 1; Index <= 8; ++Index) {
                Bodies.push_back(Add(*World, JointBody({0, 5.0f - Index, 0}, {0.5f, 0.5f, 0.5f}, false)));
                PhysicsConstraintDesc Joint;
                Joint.Kind = PhysicsConstraintKind::BallSocket;
                Joint.BodyA = Bodies[Index - 1]; Joint.BodyB = Bodies[Index];
                Joint.LocalFrameA = CFrame(0, -0.5f, 0);
                Joint.LocalFrameB = CFrame(0, 0.5f, 0);
                Joints.push_back(World->CreateConstraint(Joint));
            }
        }
        for (auto Id : Joints) Require(Id.IsValid(), "joint admission failed");
        if (Name == "articulated-chain")
            Require(World->ApplyLinearImpulse(Bodies.back(), {2, 0, 0}).Succeeded(),
                "articulated chain drive impulse failed");
        std::vector<double> Samples;
        const int Frames = Selection.Quick ? 20 : 120;
        float MaximumLinkDrift = 0.0f;
        float MaximumLinkCenterDistance = 0.0f;
        float MaximumHingeAbsoluteAngle = 0.0f;
        float MinimumSliderX = std::numeric_limits<float>::infinity();
        float MaximumSliderX = -std::numeric_limits<float>::infinity();
        float MinimumSpringLength = std::numeric_limits<float>::infinity();
        float MaximumSpringLength = 0.0f;
        float FinalHingeAngle = 0.0f;
        float FinalSliderX = 0.0f;
        float FinalSpringLength = 0.0f;
        for (int Index = 0; Index < Frames; ++Index) {
            Step(*World, Samples, Index == 0 ? std::optional<PhysicsBodyId>(Bodies[1]) : std::nullopt);
            for (auto Id : Bodies) FiniteState(*World, Id);
            if (Motor) {
                const auto Door = World->GetBodyState(Bodies[1])->Transform;
                FinalHingeAngle = std::atan2(Door.Rotation[0][1], Door.Rotation[0][0]);
                FinalSliderX = World->GetBodyState(Bodies[3])->Transform.Position.x;
                MaximumHingeAbsoluteAngle = std::max(MaximumHingeAbsoluteAngle, std::abs(FinalHingeAngle));
                MinimumSliderX = std::min(MinimumSliderX, FinalSliderX);
                MaximumSliderX = std::max(MaximumSliderX, FinalSliderX);
            } else if (Name == "spring-distance") {
                FinalSpringLength = glm::length(World->GetBodyState(Bodies[1])->Transform.Position -
                    World->GetBodyState(Bodies[0])->Transform.Position);
                MinimumSpringLength = std::min(MinimumSpringLength, FinalSpringLength);
                MaximumSpringLength = std::max(MaximumSpringLength, FinalSpringLength);
            } else {
                for (std::size_t Link = 1; Link < Bodies.size(); ++Link) {
                    const auto A = World->GetBodyState(Bodies[Link - 1])->Transform;
                    const auto B = World->GetBodyState(Bodies[Link])->Transform;
                    const auto AttachmentA = A.Position + A.Rotation * glm::vec3(0, -0.5f, 0);
                    const auto AttachmentB = B.Position + B.Rotation * glm::vec3(0, 0.5f, 0);
                    const auto LinkDrift = glm::length(AttachmentA - AttachmentB);
                    Require(std::isfinite(LinkDrift), "articulated attachment drift was nonfinite");
                    MaximumLinkDrift = std::max(MaximumLinkDrift, LinkDrift);
                    MaximumLinkCenterDistance = std::max(MaximumLinkCenterDistance,
                        glm::length(A.Position - B.Position));
                }
            }
        }
        bool Stable = true;
        if (Motor) Stable = std::abs(FinalHingeAngle) > 0.05f &&
            MaximumHingeAbsoluteAngle <= 0.45f &&
            MinimumSliderX >= 8.95f && MaximumSliderX <= 11.05f && FinalSliderX > 10.1f;
        else if (Name == "spring-distance") Stable = std::isfinite(FinalSpringLength) &&
            MinimumSpringLength > 1.0f && MaximumSpringLength < 3.0f;
        else Stable = MaximumLinkDrift < 0.2f;
        for (auto Id : Joints) Require(World->DestroyConstraint(Id).Succeeded(), "joint retirement failed");
        Retire(*World, Bodies);
        return {{"Case", std::string(Name)}, {"Status", "measured"}, {"Bodies", Bodies.size()},
            {"Constraints", Joints.size()}, {"Step", Timings(Samples)},
            {"MaximumLinkDrift", Name == "articulated-chain" ? Json(MaximumLinkDrift) : Json(nullptr)},
            {"MaximumLinkCenterDistance", Name == "articulated-chain" ? Json(MaximumLinkCenterDistance) : Json(nullptr)},
            {"FinalHingeAngle", Motor ? Json(FinalHingeAngle) : Json(nullptr)},
            {"MaximumHingeAbsoluteAngle", Motor ? Json(MaximumHingeAbsoluteAngle) : Json(nullptr)},
            {"FinalSliderX", Motor ? Json(FinalSliderX) : Json(nullptr)},
            {"MinimumSliderX", Motor ? Json(MinimumSliderX) : Json(nullptr)},
            {"MaximumSliderX", Motor ? Json(MaximumSliderX) : Json(nullptr)},
            {"FinalSpringLength", Name == "spring-distance" ? Json(FinalSpringLength) : Json(nullptr)},
            {"MinimumSpringLength", Name == "spring-distance" ? Json(MinimumSpringLength) : Json(nullptr)},
            {"MaximumSpringLength", Name == "spring-distance" ? Json(MaximumSpringLength) : Json(nullptr)},
            {"Quality", {{"Status", Stable ? "pass" : "fail"},
                {"Checks", {"finite states each step", "authored limits plus 0.05 tolerance",
                    "transient spring/link drift", "constraint and body retirement"}}}}};
    }
    Json RunOne(const Options &Selection, std::string_view Name) {
        if (Name == "static-world" || Name == "stack" || Name == "dense-contacts" || Name == "mostly-static")
            return RunBoxScene(Selection, Name);
        if (Name == "body-churn") return RunChurn(Selection);
        if (Name == "sleep-wake") return RunSleep(Selection);
        if (Name == "mesh-contact") return RunMesh(Selection);
        if (Name == "advanced-queries") return RunQueries(Selection);
        if (Name == "mesh-query-dedup") return RunMeshQuery(Selection);
        if (Name == "filter-updates") return RunFilter(Selection);
        if (Name == "moving-ccd-off") return RunCcd(Selection, false);
        if (Name == "moving-ccd-on") return RunCcd(Selection, true);
        if (Name == "joint-motors" || Name == "spring-distance" || Name == "articulated-chain")
            return RunJoints(Selection, Name);
        return {{"Case", std::string(Name)}, {"Status", "unsupported"},
            {"Reason", "No bounded experimental fixture or neutral adapter coverage"},
            {"Step", nullptr}, {"Quality", {{"Status", "unsupported"}}}};
    }
    Options Parse(int Count, char **Arguments) {
        Options Result;
        for (int Index = 1; Index < Count; ++Index) {
            const std::string_view Value(Arguments[Index]);
            if (Value == "--quick") Result.Quick = true;
            else if (Value.starts_with("--backend=")) Result.Backend = Value.substr(10);
            else if (Value.starts_with("--case=")) Result.Case = Value.substr(7);
            else if (Value.starts_with("--workers=")) Result.Workers = std::stoi(std::string(Value.substr(10)));
            else if (Value.starts_with("--world-updates=")) Result.WorldUpdates =
                std::stoi(std::string(Value.substr(16)));
            else if (Value.starts_with("--collision-steps=")) Result.CollisionSteps =
                std::stoi(std::string(Value.substr(18)));
            else throw std::invalid_argument("[Physics:JoltRigid] unknown argument: " + std::string(Value));
        }
        Require(Result.Backend == "box3d" || Result.Backend == "jolt", "backend must be box3d or jolt");
        Require(Result.Workers == 1 || Result.Workers == 2 || Result.Workers == 8,
            "workers must be a total thread budget of 1, 2, or 8");
        Require(Result.WorldUpdates >= 1 && Result.WorldUpdates <= 96,
            "world updates must be between 1 and 96");
        Require(Result.CollisionSteps >= 1 && Result.CollisionSteps <= 8,
            "collision steps must be between 1 and 8");
        return Result;
    }
}
}

int main(int ArgumentCount, char **Arguments) {
    using namespace PhysicsEvaluation;
    SuppressErrorDialogs();
    InstallPhysicsAllocators();
    try {
        const auto Selection = Parse(ArgumentCount, Arguments);
        SelectedWorldUpdates = Selection.WorldUpdates;
        SelectedCollisionSteps = Selection.CollisionSteps;
        const std::vector<std::string_view> Cases = {"static-world", "stack", "dense-contacts",
            "mostly-static", "body-churn", "sleep-wake", "mesh-contact", "advanced-queries",
            "mesh-query-dedup",
            "filter-updates", "moving-ccd-off", "moving-ccd-on", "joint-motors",
            "spring-distance", "articulated-chain", "concurrent-admission"};
        if (Selection.Case != "all" && std::ranges::find(Cases, Selection.Case) == Cases.end())
            throw std::invalid_argument("[Physics:JoltRigid] unknown case");
        std::cout << Json{{"Type", "PhysicsJoltRigidBenchmark"}, {"Backend", Selection.Backend},
            {"WorkerBudgetTotal", Selection.Workers},
            {"JoltBackgroundWorkers", Selection.Backend == "jolt" ? Selection.Workers - 1 : 0},
            {"Box3DSolverWorkers", Selection.Backend == "box3d" ? Json(1) : Json(nullptr)},
            {"JoltFixedTempAllocatorReserveBytes", Selection.Backend == "jolt" ? 32 * 1024 * 1024 : 0},
            {"TempArenaSuballocationsMeasured", false},
            {"FixedDeltaSeconds", 1.0 / 60.0}, {"CollisionSubsteps", Selection.CollisionSteps},
            {"WorldUpdatesPerFrame", Selection.WorldUpdates},
            {"TotalCollisionStepsPerFrame", Selection.WorldUpdates * Selection.CollisionSteps},
            {"WorldUpdateDeltaSeconds", (1.0f / 60.0f) / Selection.WorldUpdates},
            {"TimingScope", "Each frame sample sums World.Step call durations over all world updates; step-result validation is outside the sample"},
            {"WarmupFrames", Selection.Quick ? 2 : 30},
            {"QualityRule", "Only passing rows may be considered for performance comparison"},
            {"Scope", "neutral IPhysicsBackend; no WorldRoot, asset import, or public API migration"}}.dump() << '\n';
        bool Failed = false;
        for (auto Name : Cases) {
            if (Selection.Case != "all" && Selection.Case != Name) continue;
            try {
                const auto ResourcesBefore = CaptureResources();
                auto Row = RunOne(Selection, Name);
                const auto ResourcesAfter = CaptureResources();
                Row["Backend"] = Selection.Backend;
                Row["WorkerBudgetTotal"] = Selection.Workers;
                Row["WorldUpdatesPerFrame"] = Selection.WorldUpdates;
                Row["CollisionSubstepsPerUpdate"] = Selection.CollisionSteps;
                Row["TotalCollisionStepsPerFrame"] = Selection.WorldUpdates * Selection.CollisionSteps;
                Row["JoltFixedTempAllocatorReserveBytes"] = Selection.Backend == "jolt" ? 32 * 1024 * 1024 : 0;
                Row["TotalResources"] = ResourceDelta(ResourcesBefore, ResourcesAfter);
                Failed |= Row["Quality"]["Status"] == "fail";
                std::cout << Row.dump() << '\n';
            } catch (const std::exception &Error) {
                Failed = true;
                std::cout << Json{{"Case", std::string(Name)}, {"Backend", Selection.Backend},
                    {"Status", "failed"}, {"Quality", {{"Status", "fail"}, {"Reason", Error.what()}}},
                    {"TimingClaimValid", false}}.dump() << '\n';
            }
        }
        return Failed ? 1 : 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltRigid] " << Error.what() << '\n';
        return 2;
    }
}

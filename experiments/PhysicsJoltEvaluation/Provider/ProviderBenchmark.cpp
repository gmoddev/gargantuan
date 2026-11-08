#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using namespace gargantuan;
    using PhysicsEvaluation::CreateUnifiedJoltProvider;
    using Clock = std::chrono::steady_clock;

    struct Scene {
        std::string Name;
        bool Rubber = false;
        std::uint32_t Resolution = 0;
        std::uint32_t Count = 1;
    };
    struct BodyRecord {
        SoftBodyId Id;
        SoftBodyDefinition Definition;
        std::vector<glm::vec3> Rest;
        std::uint64_t TopologyRevision = 0;
        std::uint64_t LastVertexRevision = 0;
    };
    struct Quality {
        double MeanY = 0;
        double CenterY = 0;
        double MaximumPinError = 0;
        double MeanEdgeStrain = 0;
        double MaximumEdgeStrain = 0;
        double MeanVolumeRatio = 0;
        double MeanTetraVolumeRatio = 0;
        double MaximumTetraVolumeError = 0;
        double MeanDiagonalStrain = 0;
        std::size_t EdgeCount = 0;
        std::size_t DiagonalCount = 0;
    };
    Scene GetScene(const std::string &Name) {
        if (Name == "cloth4k") return {Name, false, 64, 1};
        if (Name == "cloth16k") return {Name, false, 128, 1};
        if (Name == "cloth4x4k") return {Name, false, 64, 4};
        if (Name == "rubber4k") return {Name, true, 16, 1};
        if (Name == "rubber16k") return {Name, true, 25, 1};
        throw std::invalid_argument("Unknown scene");
    }
    SoftBodyDefinition MakeDefinition(const Scene &SceneValue, std::uint32_t Index) {
        SoftBodyDefinition Definition;
        Definition.Kind = SceneValue.Rubber ? SoftBodyKind::Rubber : SoftBodyKind::Cloth;
        Definition.Position = {20.0f * Index, 3.0f, 0.0f};
        Definition.Size = SceneValue.Rubber ? glm::vec3(2.0f) : glm::vec3(8.0f, 8.0f, 0.5f);
        Definition.ResolutionX = SceneValue.Resolution;
        Definition.ResolutionY = SceneValue.Resolution;
        if (SceneValue.Rubber) Definition.ResolutionZ = SceneValue.Resolution;
        else {
            for (std::uint32_t X = 0; X < SceneValue.Resolution; ++X) {
                const float Horizontal = float(X) / float(SceneValue.Resolution - 1) - 0.5f;
                Definition.Attachments.push_back({X,
                    Definition.Position + glm::vec3(Definition.Size.x * Horizontal,
                        Definition.Size.y * 0.5f, 0.0f)});
            }
        }
        return Definition;
    }
    std::size_t VertexIndex(const SoftBodyDefinition &Definition,
                            std::uint32_t X, std::uint32_t Y, std::uint32_t Z) {
        return (std::size_t(Z) * Definition.ResolutionY + Y) * Definition.ResolutionX + X;
    }
    double BoundsVolume(const std::vector<glm::vec3> &Positions) {
        glm::vec3 Minimum(std::numeric_limits<float>::max());
        glm::vec3 Maximum(-std::numeric_limits<float>::max());
        for (const auto &Position : Positions) {
            Minimum = glm::min(Minimum, Position);
            Maximum = glm::max(Maximum, Position);
        }
        const auto Span = Maximum - Minimum;
        return double(Span.x) * Span.y * Span.z;
    }
    Quality Inspect(const std::vector<BodyRecord> &Bodies,
                    ISoftBodyBackend &Backend) {
        Quality Result;
        double SumY = 0, SumCenterY = 0, SumEdgeStrain = 0, SumVolumeRatio = 0;
        double SumTetraRatio = 0, SumDiagonalStrain = 0;
        std::size_t Vertices = 0, RubberBodies = 0;
        for (const auto &Record : Bodies) {
            const auto State = Backend.GetBodyState(Record.Id);
            if (!State || !State->Positions || State->Positions->size() != Record.Rest.size())
                throw std::runtime_error("Missing final soft body state");
            const auto &Positions = *State->Positions;
            for (const auto &Position : Positions) {
                if (!std::isfinite(Position.x) || !std::isfinite(Position.y) ||
                    !std::isfinite(Position.z))
                    throw std::runtime_error("Nonfinite soft position");
                SumY += Position.y;
            }
            Vertices += Positions.size();
            const auto &Definition = Record.Definition;
            const auto ZCount = Definition.Kind == SoftBodyKind::Cloth ? 1u : Definition.ResolutionZ;
            const auto Center = VertexIndex(Definition, Definition.ResolutionX / 2,
                Definition.ResolutionY / 2, ZCount / 2);
            SumCenterY += Positions[Center].y;
            for (const auto &Pin : Definition.Attachments)
                Result.MaximumPinError = std::max(Result.MaximumPinError,
                    double(glm::length(Positions[Pin.Vertex] - Pin.Position)));
            const auto Edge = [&](std::size_t A, std::size_t B) {
                const double RestLength = glm::length(Record.Rest[B] - Record.Rest[A]);
                const double CurrentLength = glm::length(Positions[B] - Positions[A]);
                const double Strain = std::abs(CurrentLength / RestLength - 1.0);
                SumEdgeStrain += Strain;
                Result.MaximumEdgeStrain = std::max(Result.MaximumEdgeStrain, Strain);
                ++Result.EdgeCount;
            };
            for (std::uint32_t Z = 0; Z < ZCount; ++Z)
                for (std::uint32_t Y = 0; Y < Definition.ResolutionY; ++Y)
                    for (std::uint32_t X = 0; X < Definition.ResolutionX; ++X) {
                        const auto A = VertexIndex(Definition, X, Y, Z);
                        if (X + 1 < Definition.ResolutionX)
                            Edge(A, VertexIndex(Definition, X + 1, Y, Z));
                        if (Y + 1 < Definition.ResolutionY)
                            Edge(A, VertexIndex(Definition, X, Y + 1, Z));
                        if (Z + 1 < ZCount)
                            Edge(A, VertexIndex(Definition, X, Y, Z + 1));
                    }
            if (Definition.Kind == SoftBodyKind::Rubber) {
                SumVolumeRatio += BoundsVolume(Positions) / BoundsVolume(Record.Rest);
                double RestTetraVolume = 0, CurrentTetraVolume = 0;
                const auto TetraVolume = [](const std::vector<glm::vec3> &Vertices,
                                            std::size_t A, std::size_t B,
                                            std::size_t C, std::size_t D) {
                    const glm::dvec3 First = glm::dvec3(Vertices[B] - Vertices[A]);
                    const glm::dvec3 Second = glm::dvec3(Vertices[C] - Vertices[A]);
                    const glm::dvec3 Third = glm::dvec3(Vertices[D] - Vertices[A]);
                    return std::abs(glm::dot(First, glm::cross(Second, Third))) / 6.0;
                };
                const auto Tetra = [&](std::size_t A, std::size_t B,
                                       std::size_t C, std::size_t D) {
                    const double RestVolume = TetraVolume(Record.Rest, A, B, C, D);
                    const double CurrentVolume = TetraVolume(Positions, A, B, C, D);
                    RestTetraVolume += RestVolume;
                    CurrentTetraVolume += CurrentVolume;
                    Result.MaximumTetraVolumeError = std::max(
                        Result.MaximumTetraVolumeError,
                        std::abs(CurrentVolume / RestVolume - 1.0));
                };
                const auto Diagonal = [&](std::size_t A, std::size_t B) {
                    const double RestLength = glm::length(Record.Rest[B] - Record.Rest[A]);
                    const double CurrentLength = glm::length(Positions[B] - Positions[A]);
                    SumDiagonalStrain += std::abs(CurrentLength / RestLength - 1.0);
                    ++Result.DiagonalCount;
                };
                for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
                    for (std::uint32_t Y = 0; Y + 1 < Definition.ResolutionY; ++Y)
                        for (std::uint32_t X = 0; X + 1 < Definition.ResolutionX; ++X) {
                            const auto A = VertexIndex(Definition, X, Y, Z);
                            const auto B = VertexIndex(Definition, X + 1, Y, Z);
                            const auto C = VertexIndex(Definition, X, Y + 1, Z);
                            const auto D = VertexIndex(Definition, X + 1, Y + 1, Z);
                            const auto E = VertexIndex(Definition, X, Y, Z + 1);
                            const auto F = VertexIndex(Definition, X + 1, Y, Z + 1);
                            const auto G = VertexIndex(Definition, X, Y + 1, Z + 1);
                            const auto H = VertexIndex(Definition, X + 1, Y + 1, Z + 1);
                            Tetra(A, B, C, E);
                            Tetra(B, D, C, H);
                            Tetra(B, C, E, H);
                            Tetra(B, E, F, H);
                            Tetra(C, G, E, H);
                            Diagonal(A, H);
                            Diagonal(B, G);
                            Diagonal(C, F);
                            Diagonal(D, E);
                        }
                SumTetraRatio += CurrentTetraVolume / RestTetraVolume;
                ++RubberBodies;
            }
        }
        Result.MeanY = SumY / Vertices;
        Result.CenterY = SumCenterY / Bodies.size();
        Result.MeanEdgeStrain = SumEdgeStrain / Result.EdgeCount;
        Result.MeanVolumeRatio = RubberBodies ? SumVolumeRatio / RubberBodies : 0;
        Result.MeanTetraVolumeRatio = RubberBodies ? SumTetraRatio / RubberBodies : 0;
        Result.MeanDiagonalStrain = Result.DiagonalCount ?
            SumDiagonalStrain / Result.DiagonalCount : 0;
        return Result;
    }
    double Milliseconds(Clock::duration Duration) {
        return std::chrono::duration<double, std::milli>(Duration).count();
    }
    std::string ReceiptToken(std::string Value) {
        for (char &Character : Value)
            if (!((Character >= 'a' && Character <= 'z') ||
                  (Character >= 'A' && Character <= 'Z') ||
                  (Character >= '0' && Character <= '9') || Character == '-' ||
                  Character == '_')) Character = '_';
        return Value;
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        if (ArgumentCount != 6 && ArgumentCount != 7)
            throw std::invalid_argument("Usage: ProviderBenchmark jolt|xpbd scene none|static-sphere frames[1..30] workers[1..8] [shared-run-pair]");
        const std::string BackendName = Arguments[1];
        if (BackendName != "jolt" && BackendName != "xpbd")
            throw std::invalid_argument("Backend must be jolt or xpbd");
        const auto SceneValue = GetScene(Arguments[2]);
        const std::string Coupling = Arguments[3];
        if (Coupling != "none" && Coupling != "static-sphere")
            throw std::invalid_argument("Coupling must be none or static-sphere");
        const int Frames = std::stoi(Arguments[4]);
        const int Workers = std::stoi(Arguments[5]);
        if (Frames < 1 || Frames > 30 || Workers < 1 || Workers > 8)
            throw std::invalid_argument("Frame or worker budget out of bounds");
        const char *HostEnvironment = std::getenv("COMPUTERNAME");
        if (!HostEnvironment || !*HostEnvironment) HostEnvironment = std::getenv("HOSTNAME");
        const auto Host = ReceiptToken(HostEnvironment && *HostEnvironment ?
            HostEnvironment : "unknown");
#ifdef _WIN32
        constexpr const char *OperatingSystem = "windows";
#else
        constexpr const char *OperatingSystem = "linux";
#endif
        const auto BuildStamp = ReceiptToken(std::string(__DATE__) + "_" + __TIME__);
        const auto RunPair = ArgumentCount == 7 ? ReceiptToken(Arguments[6]) : "unpaired";
        if (RunPair.size() > 64 || RunPair.empty())
            throw std::invalid_argument("Run-pair identifier must be 1..64 characters");

        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = Workers;
        std::unique_ptr<ISoftBodyBackend> Xpbd;
        PhysicsEvaluation::UnifiedJoltProvider Jolt;
        PhysicsWorldConfig WorldConfig;
        WorldConfig.Gravity = {0, -9.81f, 0};
        if (BackendName == "xpbd") Xpbd = CreateSoftBodyBackend(Limits);
        else Jolt = CreateUnifiedJoltProvider(WorldConfig, Limits, Workers);
        auto *Soft = BackendName == "xpbd" ? Xpbd.get() : Jolt.Soft.get();
        if (!Soft || !Soft->IsValid()) throw std::runtime_error("Soft provider creation failed");
        std::vector<BodyRecord> Bodies;
        std::size_t Constraints = 0;
        for (std::uint32_t Index = 0; Index < SceneValue.Count; ++Index) {
            auto Definition = MakeDefinition(SceneValue, Index);
            const auto Id = Soft->CreateBody(Definition);
            if (!Id.IsValid()) throw std::runtime_error("Soft scene admission failed");
            const auto State = Soft->GetBodyState(Id);
            if (!State || !State->Positions) throw std::runtime_error("Initial soft state missing");
            const auto Inspection = Soft->GetBodyInspection(Id);
            if (!Inspection) throw std::runtime_error("Initial soft inspection missing");
            Constraints += Inspection->ConstraintCount;
            Bodies.push_back({Id, std::move(Definition), *State->Positions,
                State->TopologyRevision, State->VertexRevision});
        }
        std::vector<SoftBodyCollider> Colliders;
        double InitialMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        if (Coupling == "static-sphere") {
            for (std::uint32_t Index = 0; Index < SceneValue.Count; ++Index) {
                SoftBodyCollider Collider;
                Collider.Shape.Kind = PhysicsShapeKind::Ball;
                Collider.Shape.Size = {2, 2, 2};
                Collider.Transform.Position = {20.0f * Index +
                    (SceneValue.Rubber ? 2.2f : 5.2f), 3.0f, 0};
                Colliders.push_back(Collider);
                for (const auto &Position : Bodies[Index].Rest)
                    InitialMinimumSurfaceDistance = std::min(InitialMinimumSurfaceDistance,
                        double(glm::length(Position - Collider.Transform.Position)) - 1.0);
                if (BackendName == "jolt") {
                    PhysicsBodyDesc Description;
                    Description.Shape = Collider.Shape;
                    Description.Transform = Collider.Transform;
                    Description.Anchored = true;
                    if (!Jolt.Rigid->CreateBody(Description).IsValid())
                        throw std::runtime_error("Static rigid collider admission failed");
                }
            }
            if (InitialMinimumSurfaceDistance <= 0.0)
                throw std::runtime_error("Coupled scene begins in collider penetration");
        }
        for (const auto &Record : Bodies) {
            const auto Count = Record.Rest.size() - Record.Definition.Attachments.size();
            const auto Impulse = glm::vec3(float(Count) *
                Record.Definition.Material.ParticleMass * 2.0f, 0, 0);
            if (!Soft->ApplyImpulse(Record.Id, Impulse).Succeeded())
                throw std::runtime_error("Uniform impulse admission failed");
        }
        std::vector<double> TotalTimes;
        std::vector<double> WorldTimes;
        std::vector<double> TailTimes;
        std::vector<double> XpbdUtilizations;
        std::size_t ActualXpbdWorkers = 0;
        std::size_t XpbdJobsDispatched = 0;
        double MinimumPoststepSurfaceDistance = std::numeric_limits<double>::infinity();
        double FinalMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        TotalTimes.reserve(Frames);
        std::cout << "[Physics:ProviderBenchmark] backend=" << BackendName
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " workers_requested=" << Workers
            << " host=" << Host << " os=" << OperatingSystem
            << " build_stamp=" << BuildStamp << " run_pair=" << RunPair
            << " vertices=" << Bodies.size() * Bodies.front().Rest.size() << '\n' << std::flush;
        for (int Frame = 0; Frame < Frames; ++Frame) {
            const auto Start = Clock::now();
            if (BackendName == "jolt")
                (void)Jolt.Rigid->Step({.DeltaTime = SoftBodyStepInterval, .SubStepCount = 16});
            const auto AfterRigid = Clock::now();
            SoftBodyStepConfig StepConfig;
            StepConfig.DeltaTime = SoftBodyStepInterval;
            StepConfig.Gravity = WorldConfig.Gravity;
            if (BackendName == "xpbd") StepConfig.Colliders = Colliders;
            const auto Result = Soft->Step(StepConfig);
            if (Result.States.size() != Bodies.size() ||
                Result.Profile.SimulatedBodies != Bodies.size() ||
                Result.Profile.SimulatedVertices != Bodies.size() * Bodies.front().Rest.size() ||
                Result.Profile.FrozenBodies || Result.Profile.BacklogDrops ||
                Result.Profile.StaleResults || Result.BodiesTruncated ||
                Result.VerticesTruncated || Result.CollidersTruncated ||
                std::any_of(Result.States.begin(), Result.States.end(),
                    [](const auto &State) { return !State.Simulated; }))
                throw std::runtime_error("Soft benchmark step lost a body or truncated work");
            for (auto &Record : Bodies) {
                const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                    [&](const auto &State) { return State.Body == Record.Id; });
                if (Found == Result.States.end() || !Found->Positions ||
                    Found->Positions->size() != Record.Rest.size() ||
                    Found->TopologyRevision != Record.TopologyRevision ||
                    Found->VertexRevision <= Record.LastVertexRevision)
                    throw std::runtime_error("Soft benchmark published stale identity or revision");
                Record.LastVertexRevision = Found->VertexRevision;
            }
            if (BackendName == "xpbd") {
                ActualXpbdWorkers = Result.Profile.WorkerCount;
                XpbdJobsDispatched = Result.Profile.JobsDispatched;
                XpbdUtilizations.push_back(Result.Profile.WorkerUtilization);
            }
            const auto Finish = Clock::now();
            const double WorldTime = Milliseconds(AfterRigid - Start);
            const double TailTime = Milliseconds(Finish - AfterRigid);
            WorldTimes.push_back(WorldTime);
            TailTimes.push_back(TailTime);
            TotalTimes.push_back(WorldTime + TailTime);
            std::cout << "[Physics:ProviderBenchmark] frame=" << Frame
                << " jolt_world_ms=" << WorldTime
                << " jolt_extract_ms=" << (BackendName == "jolt" ? TailTime : 0.0)
                << " xpbd_step_ms=" << (BackendName == "xpbd" ? TailTime : 0.0)
                << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
                << " xpbd_worker_utilization=" <<
                    (BackendName == "xpbd" ? XpbdUtilizations.back() : 0.0)
                << " total_ms=" << TotalTimes.back() << '\n' << std::flush;
            if (Coupling == "static-sphere") {
                FinalMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
                for (std::size_t Index = 0; Index < Bodies.size(); ++Index) {
                    const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                        [&](const auto &State) { return State.Body == Bodies[Index].Id; });
                    if (Found == Result.States.end() || !Found->Positions)
                        throw std::runtime_error("Coupled soft state missing after step");
                    for (const auto &Position : *Found->Positions)
                        FinalMinimumSurfaceDistance = std::min(FinalMinimumSurfaceDistance,
                            double(glm::length(Position - Colliders[Index].Transform.Position)) - 1.0);
                }
                MinimumPoststepSurfaceDistance = std::min(MinimumPoststepSurfaceDistance,
                    FinalMinimumSurfaceDistance);
            }
        }
        auto Sorted = TotalTimes;
        std::sort(Sorted.begin(), Sorted.end());
        const auto Mean = [](const std::vector<double> &Values) {
            return std::accumulate(Values.begin(), Values.end(), 0.0) / Values.size();
        };
        const double Median = 0.5 * (Sorted[(Sorted.size() - 1) / 2] +
            Sorted[Sorted.size() / 2]);
        const auto QualityValue = Inspect(Bodies, *Soft);
        std::cout << "[Physics:ProviderBenchmark] summary backend=" << BackendName
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " workers_requested=" << Workers
            << " host=" << Host << " os=" << OperatingSystem
            << " build_stamp=" << BuildStamp << " run_pair=" << RunPair
            << " constraints=" << Constraints
            << " xpbd_workers_actual=" << ActualXpbdWorkers
            << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
            << " xpbd_mean_worker_utilization=" <<
                (BackendName == "xpbd" ? Mean(XpbdUtilizations) : 0.0)
            << " mean_total_ms=" << Mean(TotalTimes)
            << " median_total_ms=" << Median
            << " p95_total_ms=" << Sorted[std::size_t(std::ceil(0.95 * Sorted.size())) - 1]
            << " mean_jolt_world_ms=" << Mean(WorldTimes)
            << " mean_jolt_extract_ms=" <<
                (BackendName == "jolt" ? Mean(TailTimes) : 0.0)
            << " mean_xpbd_step_ms=" <<
                (BackendName == "xpbd" ? Mean(TailTimes) : 0.0)
            << " mean_y=" << QualityValue.MeanY
            << " center_y=" << QualityValue.CenterY
            << " max_pin_error=" << QualityValue.MaximumPinError
            << " mean_edge_strain=" << QualityValue.MeanEdgeStrain
            << " max_edge_strain=" << QualityValue.MaximumEdgeStrain
            << " mean_volume_ratio=" << QualityValue.MeanVolumeRatio
            << " mean_tetra_volume_ratio=" << QualityValue.MeanTetraVolumeRatio
            << " max_tetra_volume_error=" << QualityValue.MaximumTetraVolumeError
            << " mean_diagonal_strain=" << QualityValue.MeanDiagonalStrain
            << " initial_min_surface_distance=" <<
                (Coupling == "static-sphere" ? InitialMinimumSurfaceDistance : 0.0)
            << " minimum_poststep_surface_distance=" <<
                (Coupling == "static-sphere" ? MinimumPoststepSurfaceDistance : 0.0)
            << " final_min_surface_distance=" <<
                (Coupling == "static-sphere" ? FinalMinimumSurfaceDistance : 0.0)
            << " max_poststep_penetration=" <<
                (Coupling == "static-sphere" ?
                    std::max(0.0, -MinimumPoststepSurfaceDistance) : 0.0)
            << " observed_contact_band=" <<
                (Coupling == "static-sphere" && MinimumPoststepSurfaceDistance <= 0.15 ? 1 : 0)
            << " edge_count=" << QualityValue.EdgeCount << '\n' << std::flush;
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:ProviderBenchmark] " << Error.what() << '\n';
        return 1;
    }
}

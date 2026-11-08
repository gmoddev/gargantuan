#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/RubberBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace gargantuan {
    struct WorldRootTestAccess {
        static void Inject(WorldRoot &World, PhysicsEvaluation::UnifiedJoltProvider Provider) {
            World.Physics = PhysicsWorld(std::move(Provider.Rigid));
            World.Deformables = SoftBodyWorld(std::move(Provider.Soft));
        }
        static void SetXpbdWorkerLimit(WorldRoot &World, int Workers) {
            SoftBodyWorldLimits Limits;
            Limits.MaximumWorkers = Workers;
            World.Deformables = SoftBodyWorld(Limits);
        }
        static std::optional<SoftBodyState> State(const WorldRoot &World, ObjectId Object) {
            const auto Found = World.DeformableStates.find(Object);
            return Found == World.DeformableStates.end() ? std::nullopt :
                std::optional<SoftBodyState>(Found->second);
        }
        static bool Retired(const WorldRoot &World) {
            return World.ShuttingDownPhysics && World.DeformableIds.empty() &&
                World.PartBodies.empty() && World.DeformableStates.empty();
        }
    };
}

namespace {
    using namespace gargantuan;
    using Clock = std::chrono::steady_clock;

    struct Scene {
        std::string Name;
        bool Rubber = false;
        std::uint32_t Resolution = 0;
        std::uint32_t Count = 1;
    };
    struct BodyRecord {
        std::shared_ptr<DeformableBody> Instance;
        std::vector<glm::vec3> Rest;
        std::vector<SoftBodyAttachmentDesc> Pins;
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
    };
    Scene GetScene(const std::string &Name) {
        if (Name == "cloth4k") return {Name, false, 64, 1};
        if (Name == "cloth16k") return {Name, false, 128, 1};
        if (Name == "cloth4x4k") return {Name, false, 64, 4};
        if (Name == "rubber4k") return {Name, true, 16, 1};
        if (Name == "rubber16k") return {Name, true, 25, 1};
        throw std::invalid_argument("Unknown scene");
    }
    double Milliseconds(Clock::duration Duration) {
        return std::chrono::duration<double, std::milli>(Duration).count();
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
    double MinimumSphereDistance(const std::vector<glm::vec3> &Positions,
                                 glm::vec3 Center) {
        double Minimum = std::numeric_limits<double>::infinity();
        for (const auto &Position : Positions)
            Minimum = std::min(Minimum, double(glm::length(Position - Center)) - 1.0);
        return Minimum;
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        if (ArgumentCount != 6)
            throw std::invalid_argument("Usage: EngineBenchmark jolt|xpbd scene none|static-sphere frames[1..30] workers[1..8]");
        const std::string Backend = Arguments[1];
        if (Backend != "jolt" && Backend != "xpbd")
            throw std::invalid_argument("Backend must be jolt or xpbd");
        const auto SceneValue = GetScene(Arguments[2]);
        const std::string Coupling = Arguments[3];
        if (Coupling != "none" && Coupling != "static-sphere")
            throw std::invalid_argument("Coupling must be none or static-sphere");
        const int Frames = std::stoi(Arguments[4]);
        const int WorkerBudget = std::stoi(Arguments[5]);
        if (Frames < 1 || Frames > 30 || WorkerBudget < 1 || WorkerBudget > 8)
            throw std::invalid_argument("Frame or worker budget out of bounds");

        const auto SetupStart = Clock::now();
        gargantuan::BootstrapNativeRuntimeSchema();
        auto Game = std::make_shared<DataModel>();
        auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
        if (!World) throw std::runtime_error("DataModel Workspace missing");
        if (Backend == "jolt") {
            PhysicsWorldConfig Config;
            Config.Gravity = {0, -9.81f, 0};
            WorldRootTestAccess::Inject(*World,
                PhysicsEvaluation::CreateUnifiedJoltProvider(Config, {}, WorkerBudget));
        } else WorldRootTestAccess::SetXpbdWorkerLimit(*World, WorkerBudget);
        World->SetGravity(9.81f);
        std::vector<BodyRecord> Bodies;
        std::vector<glm::vec3> SphereCenters;
        std::size_t Vertices = 0, Constraints = 0;
        for (std::uint32_t Index = 0; Index < SceneValue.Count; ++Index) {
            const glm::vec3 Origin{20.0f * Index, 3.0f, 0.0f};
            std::shared_ptr<DeformableBody> Instance;
            if (SceneValue.Rubber) {
                auto Rubber = std::make_shared<RubberBody>();
                Rubber->SetSize({2, 2, 2});
                Rubber->SetResolutionX(SceneValue.Resolution);
                Rubber->SetResolutionY(SceneValue.Resolution);
                Rubber->SetResolutionZ(SceneValue.Resolution);
                Instance = Rubber;
            } else {
                auto ClothBody = std::make_shared<Cloth>();
                ClothBody->SetSize({8, 8, 0.5f});
                ClothBody->SetResolutionX(SceneValue.Resolution);
                ClothBody->SetResolutionY(SceneValue.Resolution);
                Instance = ClothBody;
            }
            Instance->SetName("BenchSoft" + std::to_string(Index));
            Instance->SetPosition(Origin);
            BodyRecord Record;
            Record.Instance = Instance;
            if (!SceneValue.Rubber) {
                for (std::uint32_t X = 0; X < SceneValue.Resolution; ++X) {
                    const glm::vec3 PinPosition = Origin + glm::vec3(
                        8.0f * (float(X) / float(SceneValue.Resolution - 1) - 0.5f), 4, 0);
                    auto Pin = std::make_shared<SoftBodyAttachment>();
                    Pin->SetVertexIndex(X);
                    Pin->SetPosition(PinPosition);
                    Pin->SetParent(Instance);
                    Record.Pins.push_back({X, PinPosition});
                }
            }
            Instance->SetParent(World);
            Bodies.push_back(std::move(Record));
            if (Coupling == "static-sphere") {
                const auto Center = Origin + glm::vec3(
                    SceneValue.Rubber ? 2.2f : 5.2f, 0, 0);
                SphereCenters.push_back(Center);
                auto Sphere = std::make_shared<Part>();
                Sphere->SetAnchored(true);
                Sphere->SetShape(Enums::PartType::Ball);
                Sphere->SetSize({2, 2, 2});
                Sphere->SetCFrame(CFrame(Center.x, Center.y, Center.z));
                Sphere->SetParent(World);
            }
        }
        World->StepPhysics(0.0f, std::nullopt);
        double InitialMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        for (std::size_t Index = 0; Index < Bodies.size(); ++Index) {
            auto &Record = Bodies[Index];
            const auto Inspection = World->GetDeformableInspection(Record.Instance->GetObjectId());
            const auto State = WorldRootTestAccess::State(*World, Record.Instance->GetObjectId());
            if (!Inspection || !State || !State->Positions ||
                Inspection->VertexCount != (SceneValue.Rubber ?
                    std::size_t(SceneValue.Resolution) * SceneValue.Resolution * SceneValue.Resolution :
                    std::size_t(SceneValue.Resolution) * SceneValue.Resolution) ||
                Inspection->AttachmentCount != Record.Pins.size() ||
                Inspection->ConstraintCount == 0 ||
                State->Positions->size() != Inspection->VertexCount)
                throw std::runtime_error("Authored Engine soft body or pins failed admission");
            Vertices += Inspection->VertexCount;
            Constraints += Inspection->ConstraintCount;
            Record.Rest = *State->Positions;
            Record.TopologyRevision = State->TopologyRevision;
            Record.LastVertexRevision = State->VertexRevision;
            if (Coupling == "static-sphere")
                InitialMinimumSurfaceDistance = std::min(InitialMinimumSurfaceDistance,
                    MinimumSphereDistance(Record.Rest, SphereCenters[Index]));
            const auto Movable = Record.Rest.size() - Record.Pins.size();
            Record.Instance->ApplyImpulse({float(Movable) * 0.1f * 2.0f, 0, 0});
        }
        if (Coupling == "static-sphere" && InitialMinimumSurfaceDistance <= 0)
            throw std::runtime_error("Authored coupled scene begins in penetration");
        const double SetupMs = Milliseconds(Clock::now() - SetupStart);

        std::vector<double> FrameTimes;
        FrameTimes.reserve(Frames);
        double MinimumPoststepSurfaceDistance = std::numeric_limits<double>::infinity();
        double FinalMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        double PeakPinError = 0, PeakEdgeStrain = 0;
        Quality FinalQuality;
        std::size_t XpbdWorkersActual = 0, XpbdJobsDispatched = 0;
        std::cout << "[Physics:EngineBenchmark] backend=" << Backend
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " workers_requested=" << WorkerBudget
            << " vertices=" << Vertices << " constraints=" << Constraints << '\n' << std::flush;
        for (int Frame = 0; Frame < Frames; ++Frame) {
            const auto Start = Clock::now();
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
            const auto Finish = Clock::now();
            const double FrameTime = Milliseconds(Finish - Start);
            FrameTimes.push_back(FrameTime);
            const auto &Profile = World->GetLastSoftBodyProfile();
            if (Profile.SimulatedBodies != Bodies.size() || Profile.SimulatedVertices != Vertices ||
                Profile.FrozenBodies || Profile.BacklogDrops || Profile.StaleResults)
                throw std::runtime_error("WorldRoot frame froze or dropped soft work");
            if (Backend == "xpbd") {
                XpbdWorkersActual = Profile.WorkerCount;
                XpbdJobsDispatched = Profile.JobsDispatched;
            }
            FinalMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
            Quality FrameQuality;
            double SumY = 0, SumCenterY = 0, SumEdgeStrain = 0, SumVolumeRatio = 0;
            std::size_t EdgeCount = 0;
            for (std::size_t Index = 0; Index < Bodies.size(); ++Index) {
                auto &Record = Bodies[Index];
                const auto State = WorldRootTestAccess::State(*World,
                    Record.Instance->GetObjectId());
                if (!State || !State->Positions || !State->Simulated ||
                    State->Positions->size() != Record.Rest.size() ||
                    State->TopologyRevision != Record.TopologyRevision ||
                    State->VertexRevision <= Record.LastVertexRevision)
                    throw std::runtime_error("WorldRoot published stale soft body state");
                Record.LastVertexRevision = State->VertexRevision;
                const auto &Positions = *State->Positions;
                for (const auto &Position : Positions) {
                    if (!std::isfinite(Position.x) || !std::isfinite(Position.y) ||
                        !std::isfinite(Position.z))
                        throw std::runtime_error("WorldRoot published nonfinite soft vertex");
                    SumY += Position.y;
                }
                const auto Resolution = SceneValue.Resolution;
                const auto ZCount = SceneValue.Rubber ? Resolution : 1u;
                const auto VertexIndex = [Resolution](std::uint32_t X, std::uint32_t Y,
                                                       std::uint32_t Z) {
                    return (std::size_t(Z) * Resolution + Y) * Resolution + X;
                };
                SumCenterY += Positions[VertexIndex(Resolution / 2, Resolution / 2,
                    ZCount / 2)].y;
                for (const auto &Pin : Record.Pins)
                    FrameQuality.MaximumPinError = std::max(FrameQuality.MaximumPinError,
                        double(glm::length(Positions[Pin.Vertex] - Pin.Position)));
                const auto Edge = [&](std::size_t A, std::size_t B) {
                    const double RestLength = glm::length(Record.Rest[B] - Record.Rest[A]);
                    const double CurrentLength = glm::length(Positions[B] - Positions[A]);
                    if (!(RestLength > 0) || !std::isfinite(CurrentLength))
                        throw std::runtime_error("WorldRoot quality edge is invalid");
                    const double Strain = std::abs(CurrentLength / RestLength - 1.0);
                    SumEdgeStrain += Strain;
                    FrameQuality.MaximumEdgeStrain = std::max(
                        FrameQuality.MaximumEdgeStrain, Strain);
                    ++EdgeCount;
                };
                for (std::uint32_t Z = 0; Z < ZCount; ++Z)
                    for (std::uint32_t Y = 0; Y < Resolution; ++Y)
                        for (std::uint32_t X = 0; X < Resolution; ++X) {
                            const auto A = VertexIndex(X, Y, Z);
                            if (X + 1 < Resolution) Edge(A, VertexIndex(X + 1, Y, Z));
                            if (Y + 1 < Resolution) Edge(A, VertexIndex(X, Y + 1, Z));
                            if (Z + 1 < ZCount) Edge(A, VertexIndex(X, Y, Z + 1));
                        }
                if (SceneValue.Rubber) {
                    const double RestVolume = BoundsVolume(Record.Rest);
                    const double CurrentVolume = BoundsVolume(Positions);
                    if (!(RestVolume > 0) || !std::isfinite(CurrentVolume))
                        throw std::runtime_error("WorldRoot quality volume is invalid");
                    SumVolumeRatio += CurrentVolume / RestVolume;
                }
                if (Coupling == "static-sphere")
                    FinalMinimumSurfaceDistance = std::min(FinalMinimumSurfaceDistance,
                        MinimumSphereDistance(*State->Positions, SphereCenters[Index]));
            }
            FrameQuality.MeanY = SumY / Vertices;
            FrameQuality.CenterY = SumCenterY / Bodies.size();
            FrameQuality.MeanEdgeStrain = SumEdgeStrain / EdgeCount;
            FrameQuality.MeanVolumeRatio = SceneValue.Rubber ?
                SumVolumeRatio / Bodies.size() : 0;
            PeakPinError = std::max(PeakPinError, FrameQuality.MaximumPinError);
            PeakEdgeStrain = std::max(PeakEdgeStrain, FrameQuality.MaximumEdgeStrain);
            FinalQuality = FrameQuality;
            if (Coupling == "static-sphere")
                MinimumPoststepSurfaceDistance = std::min(MinimumPoststepSurfaceDistance,
                    FinalMinimumSurfaceDistance);
            std::cout << "[Physics:EngineBenchmark] frame=" << Frame
                << " physics_step_wall_ms=" << FrameTime
                << " xpbd_jobs_dispatched=" << XpbdJobsDispatched << '\n' << std::flush;
        }
        auto Sorted = FrameTimes;
        std::sort(Sorted.begin(), Sorted.end());
        const auto Mean = std::accumulate(FrameTimes.begin(), FrameTimes.end(), 0.0) / FrameTimes.size();
        const auto Median = 0.5 * (Sorted[(Sorted.size() - 1) / 2] + Sorted[Sorted.size() / 2]);
        const auto TeardownStart = Clock::now();
        Game->Destroy();
        if (!WorldRootTestAccess::Retired(*World))
            throw std::runtime_error("WorldRoot semantic teardown retained physics identities");
        const double TeardownMs = Milliseconds(Clock::now() - TeardownStart);
        std::cout << "[Physics:EngineBenchmark] summary backend=" << Backend
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " workers_requested=" << WorkerBudget
            << " xpbd_workers_actual=" << XpbdWorkersActual
            << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
            << " vertices=" << Vertices << " constraints=" << Constraints
            << " setup_ms=" << SetupMs << " semantic_destroy_ms=" << TeardownMs
            << " mean_physics_step_wall_ms=" << Mean
            << " median_physics_step_wall_ms=" << Median
            << " p95_physics_step_wall_ms=" <<
                Sorted[std::size_t(std::ceil(0.95 * Sorted.size())) - 1]
            << " initial_min_surface_distance=" <<
                (Coupling == "static-sphere" ? InitialMinimumSurfaceDistance : 0.0)
            << " minimum_poststep_surface_distance=" <<
                (Coupling == "static-sphere" ? MinimumPoststepSurfaceDistance : 0.0)
            << " final_min_surface_distance=" <<
                (Coupling == "static-sphere" ? FinalMinimumSurfaceDistance : 0.0)
            << " observed_contact_band=" <<
                (Coupling == "static-sphere" && MinimumPoststepSurfaceDistance <= 0.15 ? 1 : 0)
            << " mean_y=" << FinalQuality.MeanY
            << " center_y=" << FinalQuality.CenterY
            << " max_pin_error=" << FinalQuality.MaximumPinError
            << " peak_pin_error=" << PeakPinError
            << " mean_edge_strain=" << FinalQuality.MeanEdgeStrain
            << " max_edge_strain=" << FinalQuality.MaximumEdgeStrain
            << " peak_edge_strain=" << PeakEdgeStrain
            << " mean_volume_ratio=" << FinalQuality.MeanVolumeRatio
            << '\n' << std::flush;
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:EngineBenchmark] " << Error.what() << '\n';
        return 1;
    }
}

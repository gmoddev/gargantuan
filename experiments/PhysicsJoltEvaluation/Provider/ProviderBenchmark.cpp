#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"
#include "../ContactProbe.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"
#ifdef PHYSICS_EVALUATION_CONTACT_VELOCITIES
#include "../ContactXpbd/ContactExperimentSeams.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sstream>
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
        if (Name == "cloth1k") return {Name, false, 32, 1};
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
        Definition.Material.StretchCompliance *= PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_COMPLIANCE", 1.0f);
        Definition.Material.Friction = PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_FRICTION", Definition.Material.Friction);
        Definition.Material.Thickness = PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_THICKNESS", Definition.Material.Thickness);
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
    void HashPosition(std::uint64_t &Hash, const glm::vec3 &Position) {
        for (const float Value : {Position.x, Position.y, Position.z}) {
            std::uint32_t Bits = 0;
            std::memcpy(&Bits, &Value, sizeof(Bits));
            Hash ^= Bits;
            Hash *= 1099511628211ull;
        }
    }
    void WriteTraceU32(std::ostream &Stream, std::uint32_t Value) {
        for (int Byte = 0; Byte < 4; ++Byte)
            Stream.put(char((Value >> (8 * Byte)) & 0xff));
    }
    void WriteTraceFloat(std::ostream &Stream, float Value) {
        std::uint32_t Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        WriteTraceU32(Stream, Bits);
    }
    nlohmann::json CalibrateXpbdRetention(int Substeps, float SubstepDamping,
                                          float FrameDamping) {
        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = 1;
        auto Backend = CreateSoftBodyBackend(Limits);
        if (!Backend || !Backend->IsValid())
            throw std::runtime_error("XPBD reference calibration backend failed admission");
        auto Definition = MakeDefinition({"free-reference-control", false, 8, 1}, 0);
        Definition.Attachments.clear();
        Definition.Quality = SoftBodyQuality::High;
        Definition.Material.Damping = SubstepDamping;
        const auto Body = Backend->CreateBody(Definition);
        if (!Body.IsValid()) throw std::runtime_error("XPBD free calibration body failed admission");
        const auto CenterX = [&]() {
            const auto State = Backend->GetBodyState(Body);
            if (!State || !State->Positions || State->Positions->size() != 64)
                throw std::runtime_error("XPBD free calibration state missing");
            double Sum = 0.0;
            for (const auto &Position : *State->Positions) {
                if (!std::isfinite(Position.x))
                    throw std::runtime_error("XPBD free calibration position nonfinite");
                Sum += Position.x;
            }
            return Sum / State->Positions->size();
        };
        const double InitialX = CenterX();
        const float TotalMass = 64.0f * Definition.Material.ParticleMass;
        if (!Backend->ApplyImpulse(Body, {TotalMass, 0, 0}).Succeeded())
            throw std::runtime_error("XPBD free calibration impulse failed");
        SoftBodyStepConfig Config;
        Config.DeltaTime = SoftBodyStepInterval / float(Substeps);
        Config.Gravity = {0, 0, 0};
        double FrameX[2]{};
        for (int Frame = 0; Frame < 2; ++Frame) {
            for (int Step = 0; Step < Substeps; ++Step) {
                const auto Result = Backend->Step(Config);
                if (Result.Profile.SimulatedBodies != 1 ||
                    Result.Profile.SimulatedVertices != 64 ||
                    Result.Profile.FrozenBodies || Result.Profile.BacklogDrops ||
                    Result.Profile.StaleResults || Result.BodiesTruncated ||
                    Result.VerticesTruncated || Result.CollidersTruncated)
                    throw std::runtime_error("XPBD free calibration dropped work");
            }
            FrameX[Frame] = CenterX();
        }
        const double FirstDisplacement = FrameX[0] - InitialX;
        const double SecondDisplacement = FrameX[1] - FrameX[0];
        const double ObservedRetention = SecondDisplacement / FirstDisplacement;
        const double ExpectedRetention = 1.0 - double(FrameDamping);
        const bool Pass = FirstDisplacement > 0.0 &&
            std::isfinite(ObservedRetention) &&
            std::abs(ObservedRetention - ExpectedRetention) <= 0.002;
        if (!Backend->DestroyBody(Body).Succeeded())
            throw std::runtime_error("XPBD free calibration body retirement failed");
        Backend.reset();
        return { {"Pass", Pass}, {"Substeps", Substeps},
            {"SubstepDamping", SubstepDamping},
            {"ExpectedFrameRetention", ExpectedRetention},
            {"ObservedFrameRetention", ObservedRetention},
            {"Tolerance", 0.002}, {"Scope", "Unpinned free cloth; zero gravity; one impulse; two frame displacements"} };
    }
    std::string ReceiptToken(std::string Value) {
        for (char &Character : Value)
            if (!((Character >= 'a' && Character <= 'z') ||
                  (Character >= 'A' && Character <= 'Z') ||
                  (Character >= '0' && Character <= '9') || Character == '-' ||
                  Character == '_')) Character = '_';
        return Value;
    }
    std::string BinaryFingerprint(const char *Path) {
        std::ifstream Stream(Path, std::ios::binary);
        if (!Stream) return "unknown";
        std::uint64_t Hash = 14695981039346656037ull;
        char Buffer[8192];
        while (Stream.read(Buffer, sizeof(Buffer)) || Stream.gcount() > 0)
            for (std::streamsize Index = 0; Index < Stream.gcount(); ++Index) {
                Hash ^= static_cast<unsigned char>(Buffer[Index]);
                Hash *= 1099511628211ull;
            }
        if (!Stream.eof()) return "unknown";
        std::ostringstream Result;
        Result << std::hex << std::setfill('0') << std::setw(16) << Hash;
        return Result.str();
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        if (ArgumentCount < 6 || ArgumentCount > 10)
            throw std::invalid_argument("Usage: ProviderBenchmark jolt|xpbd scene none|static-sphere|driven-sphere frames[1..30] workers[1..8] [shared-run-pair [warmup-frames[0..30] [xpbd-substeps [quality-trace-path]]]]");
        const std::string BackendName = Arguments[1];
        if (BackendName != "jolt" && BackendName != "xpbd")
            throw std::invalid_argument("Backend must be jolt or xpbd");
        const auto SceneValue = GetScene(Arguments[2]);
        const std::string Coupling = Arguments[3];
        if (Coupling != "none" && Coupling != "static-sphere" &&
            Coupling != "driven-sphere")
            throw std::invalid_argument("Coupling must be none, static-sphere, or driven-sphere");
        const bool DrivenSphere = Coupling == "driven-sphere";
        const bool ContactCaptureInstrumented = DrivenSphere && BackendName == "jolt";
        const float DriveDurationSeconds = PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_DURATION", 0.66f);
        constexpr float DriveDistanceMeters = 1.2f;
        const int Frames = std::stoi(Arguments[4]);
        const int Workers = std::stoi(Arguments[5]);
#ifdef PHYSICS_EVALUATION_LONG_RUN
        constexpr int MaximumFrames = 600;
        constexpr bool LongHorizonQualification = true;
#else
        constexpr int MaximumFrames = 30;
        constexpr bool LongHorizonQualification = false;
#endif
        if (Frames < 1 || Frames > MaximumFrames || Workers < 1 || Workers > 8)
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
        const auto BinaryId = BinaryFingerprint(Arguments[0]);
        const auto RunPair = ArgumentCount >= 7 ? ReceiptToken(Arguments[6]) : "unpaired";
        if (RunPair.size() > 64 || RunPair.empty())
            throw std::invalid_argument("Run-pair identifier must be 1..64 characters");
        const int WarmupFrames = ArgumentCount >= 8 ? std::stoi(Arguments[7]) : 0;
        if (WarmupFrames < 0 || WarmupFrames > 30)
            throw std::invalid_argument("Warmup frames must be between 0 and 30");
        const int RequestedXpbdSubsteps = ArgumentCount >= 9 ? std::stoi(Arguments[8]) : 1;
        const bool QualityTrace = ArgumentCount == 10;
        if (RequestedXpbdSubsteps != 1 && RequestedXpbdSubsteps != 4 &&
            RequestedXpbdSubsteps != 8 && RequestedXpbdSubsteps != 16 &&
            RequestedXpbdSubsteps != 32)
            throw std::invalid_argument("XPBD reference substeps must be 1, 4, 8, 16, or 32");
        const bool XpbdReference = BackendName == "xpbd" && RequestedXpbdSubsteps > 1;
        const int ActualXpbdSubsteps = BackendName == "xpbd" ? RequestedXpbdSubsteps : 0;
        const float FrameDamping = SoftBodyMaterialDesc{}.Damping;
        const float SubstepDamping = XpbdReference ?
            float(1.0 - std::pow(1.0 - double(FrameDamping),
                1.0 / double(RequestedXpbdSubsteps))) : FrameDamping;
        const char *XpbdQualityMode = BackendName != "xpbd" ? "not-applicable" :
            XpbdReference ? "high-reference" : "automatic-production";
        const char *XpbdDampingMode = BackendName != "xpbd" ? "not-applicable" :
            XpbdReference ? "per-substep-frame-retention" : "per-frame-production";
        const auto JoltProfile = PhysicsEvaluation::GetJoltSoftSolverProfile();
        const bool UsesJoltProfile = BackendName == "jolt";
#if defined(PHYSICS_EVALUATION_TRACK_ALLOCATIONS)
        constexpr bool AllocationTimingInstrumented = true;
#else
        constexpr bool AllocationTimingInstrumented = false;
#endif
#ifdef PHYSICS_EVALUATION_NATIVE_PROFILE
        constexpr bool NativeTimingInstrumented = true;
#else
        constexpr bool NativeTimingInstrumented = false;
#endif
        const bool TimingInstrumented = AllocationTimingInstrumented || NativeTimingInstrumented ||
            LongHorizonQualification ||
            ContactCaptureInstrumented || QualityTrace;

        const auto BeforeAdmission = PhysicsEvaluation::CaptureResources();
        const auto AdmissionStart = Clock::now();
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
            if (XpbdReference) {
                Definition.Quality = SoftBodyQuality::High;
                Definition.Material.Damping = SubstepDamping;
            }
            const auto Id = Soft->CreateBody(Definition);
            if (!Id.IsValid()) throw std::runtime_error("Soft scene admission failed");
            const auto State = Soft->GetBodyState(Id);
            if (!State || !State->Positions) throw std::runtime_error("Initial soft state missing");
            const auto Inspection = Soft->GetBodyInspection(Id);
            if (!Inspection) throw std::runtime_error("Initial soft inspection missing");
            const std::size_t ExpectedVertices = SceneValue.Rubber ?
                std::size_t(SceneValue.Resolution) * SceneValue.Resolution * SceneValue.Resolution :
                std::size_t(SceneValue.Resolution) * SceneValue.Resolution;
            if (State->Positions->size() != ExpectedVertices ||
                Inspection->VertexCount != ExpectedVertices ||
                Inspection->AttachmentCount != Definition.Attachments.size() ||
                Inspection->ConstraintCount == 0)
                throw std::runtime_error("Soft scene topology or attachment admission mismatch");
            Constraints += Inspection->ConstraintCount;
            Bodies.push_back({Id, std::move(Definition), *State->Positions,
                State->TopologyRevision, State->VertexRevision});
        }
        std::vector<SoftBodyCollider> Colliders;
        std::vector<glm::vec3> InitialColliderCenters;
        std::vector<PhysicsBodyId> StaticBodies;
        std::vector<PhysicsBodyDesc> StaticDescriptions;
        double InitialMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        if (Coupling != "none") {
            for (std::uint32_t Index = 0; Index < SceneValue.Count; ++Index) {
                SoftBodyCollider Collider;
                Collider.Shape.Kind = PhysicsShapeKind::Ball;
                Collider.Shape.Size = {2, 2, 2};
                Collider.Transform.Position = {20.0f * Index +
                    (SceneValue.Rubber ? 2.2f : 5.2f), 3.0f, 0};
                Collider.Transform.Position.z = PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_SPHERE_Z", 0.0f);
                Colliders.push_back(Collider);
                InitialColliderCenters.push_back(Collider.Transform.Position);
                for (const auto &Position : Bodies[Index].Rest)
                    InitialMinimumSurfaceDistance = std::min(InitialMinimumSurfaceDistance,
                        double(glm::length(Position - Collider.Transform.Position)) - 1.0);
                if (BackendName == "jolt") {
                    PhysicsBodyDesc Description;
                    Description.Shape = Collider.Shape;
                    Description.Transform = Collider.Transform;
                    Description.Anchored = true;
                    if (std::getenv("GANTRIA_PROBE_KINEMATIC")) {
                        Description.Anchored = false;
                        Description.Kinematic = true;
                    }
                    const auto StaticBody = Jolt.Rigid->CreateBody(Description);
                    if (!StaticBody.IsValid())
                        throw std::runtime_error("Static rigid collider admission failed");
                    StaticBodies.push_back(StaticBody);
                    StaticDescriptions.push_back(Description);
                }
            }
            if (InitialMinimumSurfaceDistance <= 0.0)
                throw std::runtime_error("Coupled scene begins in collider penetration");
        }
        if (ContactCaptureInstrumented &&
            !PhysicsEvaluation::SetJoltSoftRigidContactCapture(*Jolt.Rigid, true))
            throw std::runtime_error("Jolt soft-rigid contact capture failed admission");
        for (const auto &Record : Bodies) {
            const auto Count = Record.Rest.size() - Record.Definition.Attachments.size();
            const auto Impulse = glm::vec3(float(Count) *
                Record.Definition.Material.ParticleMass * 2.0f * PhysicsEvaluation::ContactProbeFloat("GANTRIA_PROBE_IMPULSE", 1.0f), 0, 0);
            if (!Soft->ApplyImpulse(Record.Id, Impulse).Succeeded())
                throw std::runtime_error("Uniform impulse admission failed");
        }
        const double StartupAdmissionMs = Milliseconds(Clock::now() - AdmissionStart);
        const auto AfterAdmission = PhysicsEvaluation::CaptureResources();
        std::vector<double> TotalTimes;
        std::vector<double> WorldTimes;
        std::vector<double> TailTimes;
        std::vector<double> XpbdUtilizations;
        std::size_t ActualXpbdWorkers = 0;
        std::size_t XpbdJobsDispatched = 0;
        std::size_t XpbdJobsDispatchedTotal = 0;
        std::size_t MeasuredXpbdSubsteps = 0;
        double MinimumPoststepSurfaceDistance = std::numeric_limits<double>::infinity();
        double FinalMinimumSurfaceDistance = std::numeric_limits<double>::infinity();
        std::uint64_t SoftTrajectoryHash = 14695981039346656037ull;
        std::ofstream Trace;
#ifdef PHYSICS_EVALUATION_CONTACT_VELOCITIES
        std::ofstream JoltVelocityTrace;
        if (const char *VelocityPath = std::getenv("GANTRIA_CONTACT_VELOCITY_TRACE");
            VelocityPath && BackendName == "jolt") {
            if (!QualityTrace || !*VelocityPath)
                throw std::invalid_argument("[Physics:ContactProbe] velocity output needs quality trace and path");
            JoltVelocityTrace.open(VelocityPath, std::ios::binary | std::ios::trunc);
            if (!JoltVelocityTrace)
                throw std::runtime_error("[Physics:ContactProbe] Jolt velocity trace open failed");
        }
#endif
        if (QualityTrace) {
            Trace.open(Arguments[9], std::ios::binary | std::ios::trunc);
            if (!Trace) throw std::runtime_error("Quality trace file admission failed");
            Trace.write("PJTRACE1", 8);
            WriteTraceU32(Trace, 1);
            WriteTraceU32(Trace, std::uint32_t(Bodies.size()));
            WriteTraceU32(Trace, std::uint32_t(Bodies.front().Rest.size()));
            WriteTraceU32(Trace, std::uint32_t(WarmupFrames + Frames));
            WriteTraceU32(Trace, std::uint32_t(WarmupFrames));
            WriteTraceFloat(Trace, SoftBodyStepInterval);
            if (!Trace) throw std::runtime_error("Quality trace header write failed");
        }
        TotalTimes.reserve(Frames);
        std::cout << "[Physics:ProviderBenchmark] backend=" << BackendName
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " workers_requested=" << Workers
            << " host=" << Host << " os=" << OperatingSystem
            << " build_stamp=" << BuildStamp << " binary_id=" << BinaryId
            << " run_pair=" << RunPair
            << " warmup_frames=" << WarmupFrames
            << " timing_instrumented=" << int(TimingInstrumented)
            << " quality_trace=" << int(QualityTrace)
            << " contact_capture_instrumented=" << int(ContactCaptureInstrumented)
            << " drive_duration_seconds=" << (DrivenSphere ? DriveDurationSeconds : 0.0f)
            << " drive_distance_m=" << (DrivenSphere ? DriveDistanceMeters : 0.0f)
            << " xpbd_substeps_requested=" << RequestedXpbdSubsteps
            << " xpbd_substeps_actual=" << ActualXpbdSubsteps
            << " xpbd_reference=" << int(XpbdReference)
            << " xpbd_quality_mode=" << XpbdQualityMode
            << " xpbd_damping_mode=" << XpbdDampingMode
            << " xpbd_substep_damping=" << (BackendName == "xpbd" ? SubstepDamping : 0.0f)
            << " jolt_world_updates=" << (UsesJoltProfile ? JoltProfile.WorldUpdates : 0)
            << " jolt_collision_steps_per_update=" <<
                (UsesJoltProfile ? JoltProfile.CollisionStepsPerUpdate : 0)
            << " jolt_soft_iterations_per_collision_step=" <<
                (UsesJoltProfile ? JoltProfile.SoftIterationsPerCollisionStep : 0)
            << " jolt_constraint_sweeps_per_iteration=" <<
                (UsesJoltProfile ? JoltProfile.ConstraintSweepsPerIteration : 0)
            << " jolt_soft_integrations_per_frame=" <<
                (UsesJoltProfile ? JoltProfile.SoftIntegrationsPerFrame : 0)
            << " jolt_full_update_sleep_clock=" <<
                int(UsesJoltProfile && JoltProfile.FullUpdateSleepClock)
            << " jolt_large_cloth_group_batch=" << (UsesJoltProfile ? JoltProfile.LargeClothGroupBatch : 0)
            << " vertices=" << Bodies.size() * Bodies.front().Rest.size() << '\n' << std::flush;
        for (int SequenceFrame = 0; SequenceFrame < WarmupFrames + Frames; ++SequenceFrame) {
            const bool Measured = SequenceFrame >= WarmupFrames;
            const int Frame = SequenceFrame - WarmupFrames;
            const auto DriveOffset = [&](float TimeSeconds) {
                return DriveDistanceMeters * std::min(TimeSeconds / DriveDurationSeconds, 1.0f);
            };
            const auto Start = Clock::now();
            const int ProbeParts = PhysicsEvaluation::ContactProbeParts();
            for (int ProbePart = 1; BackendName == "jolt" && ProbePart <= ProbeParts; ++ProbePart) {
            if (DrivenSphere) {
                const float TimeSeconds = (float(SequenceFrame) + float(ProbePart) / ProbeParts) * SoftBodyStepInterval;
                for (std::size_t Index = 0; Index < StaticBodies.size(); ++Index) {
                    StaticDescriptions[Index].Transform.Position = InitialColliderCenters[Index] -
                        glm::vec3(DriveOffset(TimeSeconds), 0, 0);
                    if (!Jolt.Rigid->UpdateBody(StaticBodies[Index], StaticDescriptions[Index]).Succeeded())
                        throw std::runtime_error("Driven Jolt sphere update failed");
                    Colliders[Index].Transform.Position = StaticDescriptions[Index].Transform.Position;
                }
            }
            if (BackendName == "jolt")
                (void)Jolt.Rigid->Step({.DeltaTime = SoftBodyStepInterval / ProbeParts, .SubStepCount = 16});
            }
            const auto AfterRigid = Clock::now();
            SoftBodyStepConfig StepConfig;
            const int SoftStepsThisFrame = BackendName == "xpbd" ? ActualXpbdSubsteps : 1;
            StepConfig.DeltaTime = SoftBodyStepInterval / float(SoftStepsThisFrame);
            StepConfig.Gravity = WorldConfig.Gravity;
            if (BackendName == "xpbd") StepConfig.Colliders = Colliders;
            SoftBodyStepResult Result;
            std::size_t FrameXpbdJobs = 0;
            double FrameXpbdUtilization = 0.0;
            for (int Substep = 0; Substep < SoftStepsThisFrame; ++Substep) {
                if (DrivenSphere && BackendName == "xpbd") {
                    const float TimeSeconds =
                        (float(SequenceFrame) + float(Substep + 1) / SoftStepsThisFrame) *
                        SoftBodyStepInterval;
                    for (std::size_t Index = 0; Index < Colliders.size(); ++Index)
                        Colliders[Index].Transform.Position = InitialColliderCenters[Index] -
                            glm::vec3(DriveOffset(TimeSeconds), 0, 0);
                    StepConfig.Colliders = Colliders;
                }
                Result = Soft->Step(StepConfig);
                if (Result.States.size() != Bodies.size() ||
                    Result.Profile.SimulatedBodies != Bodies.size() ||
                    Result.Profile.SimulatedVertices != Bodies.size() * Bodies.front().Rest.size() ||
                    Result.Profile.FrozenBodies || Result.Profile.BacklogDrops ||
                    Result.Profile.StaleResults || Result.BodiesTruncated ||
                    Result.VerticesTruncated || Result.CollidersTruncated ||
                    std::any_of(Result.States.begin(), Result.States.end(),
                        [](const auto &State) { return !State.Simulated; }))
                    throw std::runtime_error("Soft benchmark substep lost a body or truncated work");
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
                for (const auto &State : Result.States)
                    for (const auto &Position : *State.Positions)
                        if (!std::isfinite(Position.x) || !std::isfinite(Position.y) ||
                            !std::isfinite(Position.z))
                            throw std::runtime_error("Nonfinite soft position during warmup or measured substep");
                if (BackendName == "xpbd") {
                    ActualXpbdWorkers = Result.Profile.WorkerCount;
                    FrameXpbdJobs += Result.Profile.JobsDispatched;
                    FrameXpbdUtilization += Result.Profile.WorkerUtilization;
                    if (Measured) {
                        XpbdUtilizations.push_back(Result.Profile.WorkerUtilization);
                        ++MeasuredXpbdSubsteps;
                    }
                }
            }
            XpbdJobsDispatched = FrameXpbdJobs;
            if (Measured) XpbdJobsDispatchedTotal += FrameXpbdJobs;
            const auto Finish = Clock::now();
            const double WorldTime = Milliseconds(AfterRigid - Start);
            const double TailTime = Milliseconds(Finish - AfterRigid);
            if (Measured) {
                WorldTimes.push_back(WorldTime);
                TailTimes.push_back(TailTime);
                TotalTimes.push_back(WorldTime + TailTime);
            }
            std::cout << "[Physics:ProviderBenchmark] "
                << (Measured ? "frame=" : "warmup_frame=")
                << (Measured ? Frame : SequenceFrame)
                << " jolt_world_ms=" << WorldTime
                << " jolt_extract_ms=" << (BackendName == "jolt" ? TailTime : 0.0)
                << " xpbd_step_ms=" << (BackendName == "xpbd" ? TailTime : 0.0)
                << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
                << " xpbd_worker_utilization=" <<
                    (BackendName == "xpbd" ? FrameXpbdUtilization / SoftStepsThisFrame : 0.0)
                << " total_ms=" << (WorldTime + TailTime) << '\n' << std::flush;
            for (const auto &Record : Bodies) {
                const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                    [&](const auto &State) { return State.Body == Record.Id; });
                for (const auto &Position : *Found->Positions)
                {
                    HashPosition(SoftTrajectoryHash, Position);
                    if (QualityTrace) {
                        WriteTraceFloat(Trace, Position.x);
                        WriteTraceFloat(Trace, Position.y);
                        WriteTraceFloat(Trace, Position.z);
                    }
                }
#ifdef PHYSICS_EVALUATION_CONTACT_VELOCITIES
                if (JoltVelocityTrace.is_open()) {
                    const auto Velocities = PhysicsEvaluation::GetJoltSoftBodyVelocities(
                        *Jolt.Rigid, *Jolt.Soft, Record.Id);
                    if (!Velocities || Velocities->size() != Record.Rest.size())
                        throw std::runtime_error("[Physics:ContactProbe] Jolt velocity state missing");
                    for (const auto &Velocity : *Velocities) {
                        if (!std::isfinite(Velocity.x) || !std::isfinite(Velocity.y) ||
                            !std::isfinite(Velocity.z))
                            throw std::runtime_error("[Physics:ContactProbe] nonfinite Jolt velocity");
                        WriteTraceFloat(JoltVelocityTrace, Velocity.x);
                        WriteTraceFloat(JoltVelocityTrace, Velocity.y);
                        WriteTraceFloat(JoltVelocityTrace, Velocity.z);
                    }
                }
#endif
            }
            if (QualityTrace && !Trace)
                throw std::runtime_error("Quality trace frame write failed");
            std::cout << "[Physics:ProviderBenchmark] state_frame=" << SequenceFrame
                << " soft_trajectory_hash=" << SoftTrajectoryHash << '\n' << std::flush;
            if (Coupling != "none") {
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
#ifdef PHYSICS_EVALUATION_CONTACT_VELOCITIES
        if (JoltVelocityTrace.is_open()) {
            JoltVelocityTrace.close();
            if (!JoltVelocityTrace)
                throw std::runtime_error("[Physics:ContactProbe] Jolt velocity trace close failed");
        }
#endif
        auto Sorted = TotalTimes;
        std::sort(Sorted.begin(), Sorted.end());
        const auto Mean = [](const std::vector<double> &Values) {
            return std::accumulate(Values.begin(), Values.end(), 0.0) / Values.size();
        };
        const double Median = 0.5 * (Sorted[(Sorted.size() - 1) / 2] +
            Sorted[Sorted.size() / 2]);
        const auto QualityValue = Inspect(Bodies, *Soft);
        std::uint64_t JoltSoftContactVertices = 0;
        if (ContactCaptureInstrumented)
            for (std::size_t Index = 0; Index < StaticBodies.size(); ++Index)
                JoltSoftContactVertices += PhysicsEvaluation::GetJoltSoftRigidContactVertexCount(
                    *Jolt.Rigid, *Jolt.Soft, Bodies[Index].Id, StaticBodies[Index]);
        const auto AfterRun = PhysicsEvaluation::CaptureResources();
        std::cout << "[Physics:ProviderBenchmark] summary backend=" << BackendName
            << " scene=" << SceneValue.Name << " coupling=" << Coupling
            << " frames=" << Frames << " warmup_frames=" << WarmupFrames
            << " workers_requested=" << Workers
            << " host=" << Host << " os=" << OperatingSystem
            << " build_stamp=" << BuildStamp << " binary_id=" << BinaryId
            << " run_pair=" << RunPair
            << " timing_instrumented=" << int(TimingInstrumented)
            << " quality_trace=" << int(QualityTrace)
            << " contact_capture_instrumented=" << int(ContactCaptureInstrumented)
            << " drive_duration_seconds=" << (DrivenSphere ? DriveDurationSeconds : 0.0f)
            << " drive_distance_m=" << (DrivenSphere ? DriveDistanceMeters : 0.0f)
            << " jolt_soft_contact_vertices=" << JoltSoftContactVertices
            << " soft_trajectory_hash=" << SoftTrajectoryHash
            << " startup_admission_ms=" << StartupAdmissionMs
            << " xpbd_substeps_requested=" << RequestedXpbdSubsteps
            << " xpbd_substeps_actual=" << ActualXpbdSubsteps
            << " xpbd_reference=" << int(XpbdReference)
            << " xpbd_quality_mode=" << XpbdQualityMode
            << " xpbd_damping_mode=" << XpbdDampingMode
            << " xpbd_substep_damping=" << (BackendName == "xpbd" ? SubstepDamping : 0.0f)
            << " jolt_world_updates=" << (UsesJoltProfile ? JoltProfile.WorldUpdates : 0)
            << " jolt_collision_steps_per_update=" <<
                (UsesJoltProfile ? JoltProfile.CollisionStepsPerUpdate : 0)
            << " jolt_soft_iterations_per_collision_step=" <<
                (UsesJoltProfile ? JoltProfile.SoftIterationsPerCollisionStep : 0)
            << " jolt_constraint_sweeps_per_iteration=" <<
                (UsesJoltProfile ? JoltProfile.ConstraintSweepsPerIteration : 0)
            << " jolt_soft_integrations_per_frame=" <<
                (UsesJoltProfile ? JoltProfile.SoftIntegrationsPerFrame : 0)
            << " jolt_full_update_sleep_clock=" <<
                int(UsesJoltProfile && JoltProfile.FullUpdateSleepClock)
            << " jolt_large_cloth_group_batch=" << (UsesJoltProfile ? JoltProfile.LargeClothGroupBatch : 0)
            << " constraints=" << Constraints
            << " xpbd_workers_actual=" << ActualXpbdWorkers
            << " xpbd_jobs_dispatched=" << XpbdJobsDispatched
            << " xpbd_jobs_dispatched_total=" << XpbdJobsDispatchedTotal
            << " xpbd_measured_substeps=" << MeasuredXpbdSubsteps
            << " xpbd_mean_worker_utilization=" <<
                (BackendName == "xpbd" ? Mean(XpbdUtilizations) : 0.0)
            << " mean_total_ms=" << Mean(TotalTimes)
            << " median_total_ms=" << Median
            << " p95_total_ms=" << Sorted[std::size_t(std::ceil(0.95 * Sorted.size())) - 1]
            << " p99_total_ms=" << Sorted[std::size_t(std::ceil(0.99 * Sorted.size())) - 1]
            << " max_total_ms=" << Sorted.back()
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
                (Coupling != "none" ? InitialMinimumSurfaceDistance : 0.0)
            << " minimum_poststep_surface_distance=" <<
                (Coupling != "none" ? MinimumPoststepSurfaceDistance : 0.0)
            << " final_min_surface_distance=" <<
                (Coupling != "none" ? FinalMinimumSurfaceDistance : 0.0)
            << " max_poststep_penetration=" <<
                (Coupling != "none" ?
                    std::max(0.0, -MinimumPoststepSurfaceDistance) : 0.0)
            << " observed_contact_band=" <<
                (Coupling != "none" && MinimumPoststepSurfaceDistance <= 0.15 ? 1 : 0)
            << " edge_count=" << QualityValue.EdgeCount << '\n' << std::flush;

        const auto TeardownStart = Clock::now();
        for (const auto &Record : Bodies)
            if (!Soft->DestroyBody(Record.Id).Succeeded())
                throw std::runtime_error("Soft body retirement failed");
        for (const auto Body : StaticBodies)
            if (!Jolt.Rigid->DestroyBody(Body).Succeeded())
                throw std::runtime_error("Static rigid collider retirement failed");
        std::vector<BodyRecord>().swap(Bodies);
        std::vector<SoftBodyCollider>().swap(Colliders);
        std::vector<PhysicsBodyId>().swap(StaticBodies);
        Xpbd.reset();
        Jolt.Soft.reset();
        Jolt.Rigid.reset();
        const double TeardownMs = Milliseconds(Clock::now() - TeardownStart);
        const auto AfterRetire = PhysicsEvaluation::CaptureResources();
        const nlohmann::json Lifecycle = {
            {"StartupAdmissionMs", StartupAdmissionMs}, {"TeardownMs", TeardownMs},
            {"WarmupFrames", WarmupFrames}, {"MeasuredFrames", Frames},
            {"TimingInstrumented", TimingInstrumented},
            {"AdmissionResources", PhysicsEvaluation::ResourceDelta(BeforeAdmission, AfterAdmission)},
            {"RunResources", PhysicsEvaluation::ResourceDelta(AfterAdmission, AfterRun)},
            {"RetireResources", PhysicsEvaluation::ResourceDelta(AfterRun, AfterRetire)},
            {"WholeProcessResources", PhysicsEvaluation::ResourceDelta(BeforeAdmission, AfterRetire)},
            {"PostRetireWorkingSetBytes", AfterRetire.WorkingSetAvailable
                ? nlohmann::json(AfterRetire.WorkingSetBytes) : nlohmann::json(nullptr)}
        };
        std::cout << "[Physics:ProviderBenchmark] lifecycle " << Lifecycle.dump() << '\n' << std::flush;
        if (QualityTrace) {
            Trace.close();
            if (!Trace) throw std::runtime_error("Quality trace close failed");
        }
        if (XpbdReference) {
#ifdef PHYSICS_EVALUATION_CONTACT_VELOCITIES
            PhysicsEvaluation::FinishExperimentalXpbdTrace();
#endif
            const auto Calibration = CalibrateXpbdRetention(
                ActualXpbdSubsteps, SubstepDamping, FrameDamping);
            std::cout << "[Physics:ProviderBenchmark] reference_calibration "
                << Calibration.dump() << '\n' << std::flush;
            if (!Calibration["Pass"].get<bool>())
                throw std::runtime_error("XPBD reference frame-retention calibration failed");
        }
        if (ContactCaptureInstrumented && JoltSoftContactVertices == 0)
            throw std::runtime_error("Driven Jolt sphere produced no actual soft contact");
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:ProviderBenchmark] " << Error.what() << '\n';
        return 1;
    }
}

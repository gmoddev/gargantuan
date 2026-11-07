#include "SoftFixtures.hpp"
#include "CouplingXpbdBackend.hpp"
#include "ExperimentSupport.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
    using PhysicsJoltEvaluation::JoltWorld;
    using Json = nlohmann::json;
    constexpr float DeltaTime = 1.0f / 60.0f;
    constexpr float RigidRadius = 0.45f;
    constexpr float RigidDensity = 0.7f;
    constexpr float RigidMass = RigidDensity * (4.0f / 3.0f) * 3.14159265358979323846f *
        RigidRadius * RigidRadius * RigidRadius;

    struct Options {
        std::string Backend = "Jolt";
        std::string Case = "SphereOnCloth";
        int Workers = 1;
        int Frames = 120;
    };

    Options Parse(int ArgumentCount, char **Arguments) {
        Options Result;
        for (int Index = 1; Index < ArgumentCount; Index += 2) {
            if (Index + 1 == ArgumentCount) throw std::runtime_error("[Physics:Coupling] Missing option value");
            const std::string_view Name = Arguments[Index];
            const std::string Value = Arguments[Index + 1];
            if (Name == "--backend") Result.Backend = Value;
            else if (Name == "--case") Result.Case = Value;
            else if (Name == "--workers") Result.Workers = std::stoi(Value);
            else if (Name == "--frames") Result.Frames = std::stoi(Value);
            else throw std::runtime_error("[Physics:Coupling] Unknown option");
        }
        if (Result.Backend != "Jolt" && Result.Backend != "XPBD") throw std::runtime_error("[Physics:Coupling] Invalid backend");
        if (Result.Case != "SphereOnCloth" && Result.Case != "RubberPush" && Result.Case != "RigidIntoCloth" &&
            Result.Case != "Repeated" && Result.Case != "Multiple")
            throw std::runtime_error("[Physics:Coupling] Invalid case");
        if (Result.Workers < 1 || Result.Workers > 8 || Result.Frames < 1 || Result.Frames > 600)
            throw std::runtime_error("[Physics:Coupling] Option outside bounded range");
        return Result;
    }

    template <class VectorType> Json Vector(VectorType Value) {
        return Json::array({Value.GetX(), Value.GetY(), Value.GetZ()});
    }

    struct SoftObservation {
        JPH::Vec3 Center{0.0f, 0.0f, 0.0f};
        JPH::Vec3 Momentum{0.0f, 0.0f, 0.0f};
        double KineticEnergy = 0.0;
        double MaxStretch = 0.0;
        double VolumeError = 0.0;
        double MaxPinDrift = 0.0;
        std::size_t InvertedTetrahedra = 0;
        bool Finite = true;
        int MovableVertices = 0;
    };

    struct ContactCounter final : JPH::SoftBodyContactListener {
        std::atomic<std::uint64_t> VertexContacts{0};
        std::array<JPH::BodyID, 2> TargetIds{};
        std::array<std::atomic<std::uint64_t>, 2> TargetContacts{};
        void OnSoftBodyContactAdded(const JPH::Body &, const JPH::SoftBodyManifold &Manifold) override {
            std::uint64_t Count = 0;
            for (const auto &Vertex : Manifold.GetVertices()) {
                if (!Manifold.HasContact(Vertex)) continue;
                ++Count;
                const auto Body = Manifold.GetContactBodyID(Vertex);
                for (std::size_t Index = 0; Index < TargetIds.size(); ++Index)
                    if (!TargetIds[Index].IsInvalid() && Body == TargetIds[Index])
                        TargetContacts[Index].fetch_add(1, std::memory_order_relaxed);
            }
            VertexContacts.fetch_add(Count, std::memory_order_relaxed);
        }
    };

    SoftObservation ObserveSoft(const JoltWorld &World, JPH::BodyID Id,
                                const JPH::SoftBodySharedSettings &Shared) {
        JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Id);
        if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Soft body lock failed");
        const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
        const auto &Vertices = Motion->GetVertices();
        SoftObservation Result;
        if (!Shared.mVolumeConstraints.empty()) {
            Result.MaxStretch = PhysicsJoltEvaluation::MaxEdgeStretch(Shared, *Motion);
        } else {
            const int N = static_cast<int>(std::lround(std::sqrt(static_cast<double>(Vertices.size()))));
            const float Rest = 4.0f / (N - 1);
            const auto Check = [&](int A, int B, float Length) {
                Result.MaxStretch = std::max(Result.MaxStretch,
                    std::abs(static_cast<double>((Vertices[B].mPosition - Vertices[A].mPosition).Length() / Length) - 1.0));
            };
            for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X) {
                const int I = Y * N + X;
                if (X + 1 < N) Check(I, I + 1, Rest);
                if (Y + 1 < N) Check(I, I + N, Rest);
                if (X + 1 < N && Y + 1 < N) {
                    Check(I, I + N + 1, Rest * std::sqrt(2.0f));
                    Check(I + 1, I + N, Rest * std::sqrt(2.0f));
                }
            }
        }
        double VolumeRest = 0.0;
        double VolumeCurrent = 0.0;
        for (const auto &Volume : Shared.mVolumeConstraints) {
            const auto A = Vertices[Volume.mVertex[0]].mPosition;
            const auto B = Vertices[Volume.mVertex[1]].mPosition;
            const auto C = Vertices[Volume.mVertex[2]].mPosition;
            const auto D = Vertices[Volume.mVertex[3]].mPosition;
            VolumeRest += Volume.mSixRestVolume;
            VolumeCurrent += std::abs((B - A).Cross(C - A).Dot(D - A));
            const auto RestA = JPH::Vec3(Shared.mVertices[Volume.mVertex[0]].mPosition);
            const auto RestB = JPH::Vec3(Shared.mVertices[Volume.mVertex[1]].mPosition);
            const auto RestC = JPH::Vec3(Shared.mVertices[Volume.mVertex[2]].mPosition);
            const auto RestD = JPH::Vec3(Shared.mVertices[Volume.mVertex[3]].mPosition);
            const auto SignedRest = (RestB - RestA).Cross(RestC - RestA).Dot(RestD - RestA);
            const auto SignedCurrent = (B - A).Cross(C - A).Dot(D - A);
            if (SignedRest * SignedCurrent <= 0.0f) ++Result.InvertedTetrahedra;
        }
        if (VolumeRest > 0.0) Result.VolumeError = std::abs(VolumeCurrent / VolumeRest - 1.0);
        for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
            const auto &Vertex = Vertices[Index];
            const auto Position = Vertex.mPosition;
            Result.Finite &= std::isfinite(Position.GetX()) && std::isfinite(Position.GetY()) && std::isfinite(Position.GetZ());
            Result.Center += Position;
            if (Vertex.mInvMass > 0.0f) {
                const float Mass = 1.0f / Vertex.mInvMass;
                Result.Momentum += Mass * Vertex.mVelocity;
                Result.KineticEnergy += 0.5 * Mass * Vertex.mVelocity.LengthSq();
                ++Result.MovableVertices;
            } else {
                const auto Rest = JPH::Vec3(Shared.mVertices[Index].mPosition);
                Result.MaxPinDrift = std::max(Result.MaxPinDrift, static_cast<double>((Position - Rest).Length()));
            }
        }
        if (!Vertices.empty()) Result.Center /= static_cast<float>(Vertices.size());
        const auto Origin = Lock.GetBody().GetPosition();
        Result.Center += JPH::Vec3(static_cast<float>(Origin.GetX()), static_cast<float>(Origin.GetY()), static_cast<float>(Origin.GetZ()));
        return Result;
    }

    Json RunJolt(const Options &Config, bool EnableContact) {
        // One calling thread plus this many background threads is the actual Jolt budget.
        const int BackgroundWorkers = std::max(0, Config.Workers - 1);
        ContactCounter Contacts;
        JoltWorld World(BackgroundWorkers, EnableContact);
        World.World.SetSoftBodyContactListener(&Contacts);
        const bool Horizontal = Config.Case == "SphereOnCloth";
        const bool IsRubber = Config.Case == "RubberPush";
        const JPH::RVec3 SoftOrigin = IsRubber ? JPH::RVec3(0.0, 0.0, 0.0) : JPH::RVec3(0.0, 0.0, 0.0);
        auto Shared = IsRubber ? PhysicsJoltEvaluation::Rubber() :
            PhysicsJoltEvaluation::Cloth(Horizontal ? 17 : 13, Horizontal ? 4.0f : 4.0f,
                                         Horizontal, Horizontal, 0.1f, 0.0f);
        if (IsRubber) for (auto &Vertex : Shared->mVertices) Vertex.mVelocity = JPH::Float3(4.0f, 0.0f, 0.0f);
        auto Soft = World.AddSoft(Shared, SoftOrigin, 24, IsRubber, 2.4f);
        World.World.SetGravity(Horizontal ? JPH::Vec3(0.0f, -9.81f, 0.0f) : JPH::Vec3::sZero());

        std::vector<JPH::BodyID> Rigids;
        if (IsRubber) {
            Rigids.push_back(World.AddRigid(new JPH::SphereShape(RigidRadius),
                                            JPH::RVec3(2.0, 0.0, 0.0), JPH::EMotionType::Dynamic, 1, RigidMass));
        } else if (Horizontal) {
            Rigids.push_back(World.AddRigid(new JPH::SphereShape(RigidRadius), JPH::RVec3(0.0, 2.0, 0.0),
                                            JPH::EMotionType::Dynamic, 1, RigidMass));
        } else {
            Rigids.push_back(World.AddRigid(new JPH::SphereShape(RigidRadius), JPH::RVec3(0.0, 0.0, -2.0),
                                            JPH::EMotionType::Dynamic, 1, RigidMass));
            World.World.GetBodyInterface().SetLinearVelocity(Rigids.back(), JPH::Vec3(0.0f, 0.0f, 5.0f));
            if (Config.Case == "Multiple") {
                Rigids.push_back(World.AddRigid(new JPH::SphereShape(RigidRadius), JPH::RVec3(1.0, -0.4, 2.0),
                                                JPH::EMotionType::Dynamic, 1, RigidMass));
                World.World.GetBodyInterface().SetLinearVelocity(Rigids.back(), JPH::Vec3(0.0f, 0.0f, -5.0f));
            }
        }
        for (std::size_t Index = 0; Index < Rigids.size(); ++Index) Contacts.TargetIds[Index] = Rigids[Index];

        Json Frames = Json::array();
        double MaxStretch = 0.0;
        double MaxVolumeError = 0.0;
        double MaxPinDrift = 0.0;
        double MaxParticlePenetration = 0.0;
        std::size_t InvertedTetrahedronFrames = 0;
        bool Finite = true;
        std::uint64_t TotalVertexContacts = 0;
        std::array<std::uint64_t, 2> TotalContactsPerRigid{};
        int FirstContactFrame = -1;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (Config.Case == "Repeated" && Frame > 0 && Frame % 45 == 0) {
                World.World.GetBodyInterface().SetPositionAndRotation(Rigids[0], JPH::RVec3(0.0, 0.0, -2.0),
                    JPH::Quat::sIdentity(), JPH::EActivation::Activate);
                World.World.GetBodyInterface().SetLinearVelocity(Rigids[0], JPH::Vec3(0.0f, 0.0f, 5.0f));
            }
            const double StepMs = World.Step(DeltaTime, 1);
            const auto FrameContacts = Contacts.VertexContacts.exchange(0, std::memory_order_relaxed);
            Json FrameContactsPerRigid = Json::array();
            for (std::size_t Index = 0; Index < Rigids.size(); ++Index) {
                const auto Count = Contacts.TargetContacts[Index].exchange(0, std::memory_order_relaxed);
                TotalContactsPerRigid[Index] += Count;
                FrameContactsPerRigid.push_back(Count);
            }
            TotalVertexContacts += FrameContacts;
            if (FrameContacts > 0 && FirstContactFrame < 0) FirstContactFrame = Frame;
            auto SoftState = ObserveSoft(World, Soft, *Shared);
            MaxStretch = std::max(MaxStretch, SoftState.MaxStretch);
            MaxVolumeError = std::max(MaxVolumeError, SoftState.VolumeError);
            MaxPinDrift = std::max(MaxPinDrift, SoftState.MaxPinDrift);
            InvertedTetrahedronFrames += SoftState.InvertedTetrahedra;
            Finite &= SoftState.Finite;
            Json RigidStates = Json::array();
            JPH::Vec3 RigidMomentum = JPH::Vec3::sZero();
            double RigidKineticEnergy = 0.0;
            for (auto Id : Rigids) {
                JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Id);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Rigid body lock failed");
                const auto &Body = Lock.GetBody();
                const auto Velocity = Body.GetLinearVelocity();
                const float InverseMass = Body.GetMotionProperties()->GetInverseMass();
                const float Mass = InverseMass > 0.0f ? 1.0f / InverseMass : 0.0f;
                const auto Position = Body.GetPosition();
                Finite &= std::isfinite(Position.GetX()) && std::isfinite(Position.GetY()) && std::isfinite(Position.GetZ()) &&
                          std::isfinite(Velocity.GetX()) && std::isfinite(Velocity.GetY()) && std::isfinite(Velocity.GetZ());
                RigidMomentum += Mass * Velocity;
                RigidKineticEnergy += 0.5 * Mass * Velocity.LengthSq();
                RigidStates.push_back({{"Position", Vector(Position)}, {"Velocity", Vector(Velocity)}, {"Mass", Mass}});
            }
            double FramePenetration = 0.0;
            {
                JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Soft);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Jolt penetration lock failed");
                const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
                const auto SoftPosition = Lock.GetBody().GetPosition();
                for (const auto &Vertex : Motion->GetVertices()) {
                    const auto Position = Vertex.mPosition + JPH::Vec3(static_cast<float>(SoftPosition.GetX()),
                        static_cast<float>(SoftPosition.GetY()), static_cast<float>(SoftPosition.GetZ()));
                    for (auto Id : Rigids) {
                        const auto RigidPosition = World.World.GetBodyInterface().GetPosition(Id);
                        const auto Distance = (Position - JPH::Vec3(static_cast<float>(RigidPosition.GetX()),
                            static_cast<float>(RigidPosition.GetY()), static_cast<float>(RigidPosition.GetZ()))).Length();
                        FramePenetration = std::max(FramePenetration,
                            std::max(0.0, static_cast<double>(RigidRadius + 0.05f - Distance)));
                    }
                }
            }
            MaxParticlePenetration = std::max(MaxParticlePenetration, FramePenetration);
            Frames.push_back({{"Frame", Frame}, {"StepMs", StepMs}, {"VertexContacts", FrameContacts},
                              {"ContactsPerRigid", std::move(FrameContactsPerRigid)},
                              {"Rigids", std::move(RigidStates)},
                              {"SoftCenter", Vector(SoftState.Center)}, {"SoftMomentum", Vector(SoftState.Momentum)},
                              {"RigidMomentum", Vector(RigidMomentum)},
                              {"TotalMomentum", Vector(RigidMomentum + SoftState.Momentum)},
                              {"KineticEnergy", RigidKineticEnergy + SoftState.KineticEnergy},
                              {"MaxEdgeStretch", SoftState.MaxStretch}, {"VolumeError", SoftState.VolumeError},
                              {"PinDrift", SoftState.MaxPinDrift},
                              {"ParticlePenetration", FramePenetration},
                              {"InvertedTetrahedra", SoftState.InvertedTetrahedra}});
        }
        return {{"Backend", "Jolt"}, {"Case", Config.Case}, {"ContactEnabled", EnableContact},
                {"Frames", std::move(Frames)}, {"Finite", Finite}, {"MaxEdgeStretch", MaxStretch},
                {"MaxVolumeError", MaxVolumeError}, {"MaxPinDrift", MaxPinDrift},
                {"MaxParticlePenetration", MaxParticlePenetration},
                {"InvertedTetrahedronFrames", InvertedTetrahedronFrames},
                {"RequestedCpuBudget", Config.Workers},
                {"AllocatedBackgroundWorkers", BackgroundWorkers},
                {"ActiveBackgroundWorkers", nullptr},
                {"WorkerActivityMeasurement", "Unavailable; thread pool capacity is not per-frame active worker count"},
                {"CallerRole", "ParticipatesInSolve"}, {"ExecutionMode", "JoltThreadPool"},
                {"CollisionSteps", 1}, {"SolverIterations", 24}, {"DeltaTime", DeltaTime},
                {"SoftVertices", Shared->mVertices.size()}, {"SoftEdges", Shared->mEdgeConstraints.size()},
                {"SoftVolumes", Shared->mVolumeConstraints.size()}, {"AnchoredBoundary", !IsRubber},
                {"TotalVertexContacts", TotalVertexContacts}, {"FirstContactFrame", FirstContactFrame},
                {"TotalContactsPerRigid", Json::array({TotalContactsPerRigid[0], TotalContactsPerRigid[1]})},
                {"RigidShape", "Ball"}, {"RigidRadius", RigidRadius}, {"RigidDensity", RigidDensity},
                {"RequestedRigidMass", RigidMass},
                {"MomentumConservationQualified", false},
                {"ControlSemantics", EnableContact ? "Unified soft-rigid contact enabled" :
                    "Soft-rigid contact disabled; rigid and soft do not collide"},
                {"PublicationOrder", "Unified Jolt update, then sample both bodies at the same completed step"},
                {"MomentumScope", "Linear rigid plus movable soft vertex momentum; damping and external boundary impulses are not integrated"},
                {"EnergyScope", "Rigid linear plus soft vertex translational kinetic energy; rigid angular kinetic energy, strain potential and damping work are unmeasured"},
                {"ExternalWork", IsRubber ? "Soft damping; collision and constraint work unmeasured" :
                    (Horizontal ? "Gravity, pinned membrane reactions, soft damping; constraint work unmeasured" :
                     "Pinned edge reactions, soft damping and any controlled restart impulses; constraint work unmeasured")},
                {"ControlledRestarts", Config.Case == "Repeated" ? (Config.Frames - 1) / 45 : 0}};
    }

    Json RunXpbd(const Options &Config, bool EnableFeedback, bool EnableContact) {
        if (Config.Case == "SphereOnCloth")
            return {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Status", "UnsupportedGeometry"},
                    {"Reason", "Current cloth topology is a fixed vertical X/Y plane; horizontal membrane comparison needs a private topology adapter"}};
        gargantuan::PhysicsWorld Rigid({.Gravity = glm::vec3(0.0f)});
        gargantuan::SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = static_cast<std::size_t>(std::max(1, Config.Workers - 1));
        const auto Hardware = std::max(1u, std::thread::hardware_concurrency());
        const auto AllocatedBackgroundWorkers = std::max<std::size_t>(1,
            std::min<std::size_t>(Hardware > 1 ? Hardware - 1 : 1, Limits.MaximumWorkers));
        auto Backend = PhysicsJoltEvaluation::CreateFeedbackSoftBodyBackend(Limits);
        auto *FeedbackBackend = Backend.get();
        gargantuan::SoftBodyWorld Soft(std::move(Backend));
        const bool Rubber = Config.Case == "RubberPush";
        gargantuan::SoftBodyDefinition Definition;
        Definition.Kind = Rubber ? gargantuan::SoftBodyKind::Rubber : gargantuan::SoftBodyKind::Cloth;
        Definition.Position = glm::vec3(0.0f);
        Definition.Size = Rubber ? glm::vec3(1.5f) : glm::vec3(4.0f, 4.0f, 0.5f);
        Definition.ResolutionX = Definition.ResolutionY = Rubber ? 4 : 13;
        Definition.ResolutionZ = 4;
        if (!Rubber) for (std::uint32_t X = 0; X < 13; ++X)
            Definition.Attachments.push_back({X, {-2.0f + (4.0f * X / 12.0f), 2.0f, 0.0f}});
        const auto SoftId = Soft.CreateBody(Definition);
        if (!SoftId.IsValid()) throw std::runtime_error("[Physics:Coupling] XPBD body admission failed");
        if (Rubber && !Soft.ApplyImpulse(SoftId, {25.6f, 0.0f, 0.0f}).Succeeded())
            throw std::runtime_error("[Physics:Coupling] XPBD rubber impulse failed");

        std::vector<gargantuan::PhysicsBodyId> Rigids;
        auto AddSphere = [&](glm::vec3 Position, glm::vec3 Velocity) {
            gargantuan::PhysicsBodyDesc Description;
            Description.Shape.Kind = gargantuan::PhysicsShapeKind::Ball;
            Description.Shape.Size = glm::vec3(2.0f * RigidRadius);
            Description.Density = RigidDensity;
            Description.Transform.Position = Position;
            Description.SleepEnabled = false;
            const auto Id = Rigid.CreateBody(Description);
            if (!Id.IsValid() || !Rigid.SetLinearVelocity(Id, Velocity).Succeeded())
                throw std::runtime_error("[Physics:Coupling] Box3D rigid admission failed");
            Rigids.push_back(Id);
        };
        if (Rubber) AddSphere({2.0f, 0.0f, 0.0f}, {});
        else {
            AddSphere({0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 5.0f});
            if (Config.Case == "Multiple") AddSphere({1.0f, -0.4f, 2.0f}, {0.0f, 0.0f, -5.0f});
        }
        Json Frames = Json::array();
        double MaximumPenetration = 0.0;
        double MaximumFeedbackImpulse = 0.0;
        double MaximumStretch = 0.0;
        double MaximumVolumeError = 0.0;
        double MaximumPinDrift = 0.0;
        std::size_t InvertedTetrahedronFrames = 0;
        std::size_t TotalContactProjections = 0;
        std::array<std::size_t, 2> TotalProjectionsPerRigid{};
        int FirstContactFrame = -1;
        bool Finite = true;
        std::vector<glm::vec3> PriorPositions;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (Config.Case == "Repeated" && Frame > 0 && Frame % 45 == 0) {
                auto Description = Rigid.GetBodyState(Rigids[0]);
                if (!Description) throw std::runtime_error("[Physics:Coupling] Box3D repeated body missing");
                Description->Description.Transform.Position = {0.0f, 0.0f, -2.0f};
                if (!Rigid.UpdateBody(Rigids[0], Description->Description).Succeeded() ||
                    !Rigid.SetLinearVelocity(Rigids[0], {0.0f, 0.0f, 5.0f}).Succeeded())
                    throw std::runtime_error("[Physics:Coupling] Box3D repeated reset failed");
            }
            const auto RigidStart = PhysicsJoltEvaluation::Clock::now();
            (void)Rigid.Step({.DeltaTime = DeltaTime, .SubStepCount = 4});
            const double RigidMs = PhysicsJoltEvaluation::Milliseconds(RigidStart);
            gargantuan::SoftBodyStepConfig SoftConfig;
            SoftConfig.Gravity = glm::vec3(0.0f);
            SoftConfig.ExecutionMode = Config.Workers == 1 ?
                gargantuan::SoftBodyExecutionMode::SynchronousReference :
                gargantuan::SoftBodyExecutionMode::Jobified;
            std::vector<gargantuan::PhysicsBodyState> RigidStates;
            for (auto Id : Rigids) {
                auto State = Rigid.GetBodyState(Id);
                if (!State) throw std::runtime_error("[Physics:Coupling] Box3D state missing");
                RigidStates.push_back(*State);
                if (EnableContact) SoftConfig.Colliders.push_back({State->Description.Shape, State->Transform});
            }
            const auto SoftStart = PhysicsJoltEvaluation::Clock::now();
            auto Result = Soft.Step(SoftConfig);
            if (Result.CollidersTruncated || Result.BodiesTruncated || Result.VerticesTruncated)
                throw std::runtime_error("[Physics:Coupling] XPBD step truncated required work");
            const auto Reactions = FeedbackBackend->TakeFeedback();
            if (EnableContact ? Reactions.size() != Rigids.size() : !Reactions.empty())
                throw std::runtime_error("[Physics:Coupling] XPBD collider feedback count mismatch");
            const double SoftMs = PhysicsJoltEvaluation::Milliseconds(SoftStart);
            const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                [&](const auto &State) { return State.Body == SoftId; });
            if (Found == Result.States.end() || !Found->Positions)
                throw std::runtime_error("[Physics:Coupling] XPBD state missing");
            const auto &Positions = *Found->Positions;
            glm::vec3 SoftCenter(0.0f);
            glm::vec3 SoftMomentum(0.0f);
            double SoftKineticEnergy = 0.0;
            const bool SoftVelocityObservationValid = PriorPositions.size() == Positions.size();
            for (const auto &Position : Positions) {
                Finite &= std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
                SoftCenter += Position;
            }
            SoftCenter /= static_cast<float>(Positions.size());
            for (std::size_t Vertex = 0; Vertex < Positions.size(); ++Vertex) {
                if (!Rubber && Vertex < Definition.ResolutionX) continue;
                if (!SoftVelocityObservationValid) continue;
                const auto Velocity = (Positions[Vertex] - PriorPositions[Vertex]) / DeltaTime;
                SoftMomentum += Definition.Material.ParticleMass * Velocity;
                SoftKineticEnergy += 0.5 * Definition.Material.ParticleMass * glm::dot(Velocity, Velocity);
            }
            double FrameStretch = 0.0;
            double FrameVolumeError = 0.0;
            double FramePinDrift = 0.0;
            std::size_t FrameInversions = 0;
            const auto CheckEdge = [&](std::size_t A, std::size_t B, float Rest) {
                FrameStretch = std::max(FrameStretch,
                    std::abs(static_cast<double>(glm::length(Positions[B] - Positions[A]) / Rest) - 1.0));
            };
            if (!Rubber) {
                const int N = static_cast<int>(Definition.ResolutionX);
                const float Rest = Definition.Size.x / (N - 1);
                for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X) {
                    const int I = Y * N + X;
                    if (X + 1 < N) CheckEdge(I, I + 1, Rest);
                    if (Y + 1 < N) CheckEdge(I, I + N, Rest);
                    if (X + 1 < N && Y + 1 < N) {
                        CheckEdge(I, I + N + 1, Rest * std::sqrt(2.0f));
                        CheckEdge(I + 1, I + N, Rest * std::sqrt(2.0f));
                    }
                }
                for (const auto &Attachment : Definition.Attachments)
                    FramePinDrift = std::max(FramePinDrift,
                        static_cast<double>(glm::length(Positions[Attachment.Vertex] - Attachment.Position)));
            } else {
                const int N = static_cast<int>(Definition.ResolutionX);
                const auto At = [N](int X, int Y, int Z) { return (Z * N + Y) * N + X; };
                const float Rest = Definition.Size.x / (N - 1);
                const std::array<glm::vec3, 8> CornerRest{{
                    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                    {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}
                }};
                constexpr std::array<std::array<int, 4>, 5> Tetra{{
                    {{0, 1, 2, 4}}, {{1, 3, 2, 7}}, {{1, 2, 4, 7}},
                    {{1, 4, 5, 7}}, {{2, 6, 4, 7}}
                }};
                double CurrentVolume = 0.0;
                for (int Z = 0; Z < N; ++Z) for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X) {
                    if (X + 1 < N) CheckEdge(At(X, Y, Z), At(X + 1, Y, Z), Rest);
                    if (Y + 1 < N) CheckEdge(At(X, Y, Z), At(X, Y + 1, Z), Rest);
                    if (Z + 1 < N) CheckEdge(At(X, Y, Z), At(X, Y, Z + 1), Rest);
                    if (X + 1 == N || Y + 1 == N || Z + 1 == N) continue;
                    const std::array<std::size_t, 8> Corner{{
                        At(X, Y, Z), At(X + 1, Y, Z), At(X, Y + 1, Z), At(X + 1, Y + 1, Z),
                        At(X, Y, Z + 1), At(X + 1, Y, Z + 1), At(X, Y + 1, Z + 1), At(X + 1, Y + 1, Z + 1)
                    }};
                    for (const auto &Ids : Tetra) {
                        const auto Signed = glm::dot(glm::cross(
                            Positions[Corner[Ids[1]]] - Positions[Corner[Ids[0]]],
                            Positions[Corner[Ids[2]]] - Positions[Corner[Ids[0]]]),
                            Positions[Corner[Ids[3]]] - Positions[Corner[Ids[0]]]);
                        const auto RestSigned = glm::dot(glm::cross(
                            CornerRest[Ids[1]] - CornerRest[Ids[0]],
                            CornerRest[Ids[2]] - CornerRest[Ids[0]]),
                            CornerRest[Ids[3]] - CornerRest[Ids[0]]);
                        if (Signed * RestSigned <= 0.0f) ++FrameInversions;
                        CurrentVolume += std::abs(Signed) / 6.0;
                    }
                }
                FrameVolumeError = std::abs(CurrentVolume /
                    (Definition.Size.x * Definition.Size.y * Definition.Size.z) - 1.0);
            }
            MaximumStretch = std::max(MaximumStretch, FrameStretch);
            MaximumVolumeError = std::max(MaximumVolumeError, FrameVolumeError);
            MaximumPinDrift = std::max(MaximumPinDrift, FramePinDrift);
            InvertedTetrahedronFrames += FrameInversions;

            const auto FeedbackStart = PhysicsJoltEvaluation::Clock::now();
            Json Feedback = Json::array();
            for (std::size_t RigidIndex = 0; RigidIndex < Rigids.size(); ++RigidIndex) {
                const auto &State = RigidStates[RigidIndex];
                const PhysicsJoltEvaluation::FeedbackImpulse EmptyReaction{.ColliderIndex = RigidIndex};
                const auto &Observed = EnableContact ? Reactions[RigidIndex] : EmptyReaction;
                if (Observed.ColliderIndex != RigidIndex)
                    throw std::runtime_error("[Physics:Coupling] XPBD collider ordinal mismatch");
                TotalContactProjections += Observed.Contacts;
                TotalProjectionsPerRigid[RigidIndex] += Observed.Contacts;
                if (Observed.Contacts > 0 && FirstContactFrame < 0) FirstContactFrame = Frame;
                const auto RawAngular = Observed.AngularImpulseOrigin -
                    glm::cross(State.Transform.Position, Observed.Impulse);
                const float MaximumImpulse = State.Mass * 8.0f;
                const float MaximumAngularImpulse = 0.4f * State.Mass * RigidRadius * RigidRadius * 8.0f;
                float Scale = 1.0f;
                const float LinearMagnitude = glm::length(Observed.Impulse);
                const float AngularMagnitude = glm::length(RawAngular);
                if (LinearMagnitude > MaximumImpulse && MaximumImpulse > 0.0f)
                    Scale = std::min(Scale, MaximumImpulse / LinearMagnitude);
                if (AngularMagnitude > MaximumAngularImpulse && MaximumAngularImpulse > 0.0f)
                    Scale = std::min(Scale, MaximumAngularImpulse / AngularMagnitude);
                const glm::vec3 Reaction = Scale * Observed.Impulse;
                const glm::vec3 AngularReaction = Scale * RawAngular;
                const float Radius = RigidRadius + Definition.Material.Thickness;
                for (const auto &Position : Positions) {
                    const float Distance = glm::length(Position - State.Transform.Position);
                    MaximumPenetration = std::max(MaximumPenetration,
                        std::max(0.0, static_cast<double>(Radius - Distance)));
                }
                bool Applied = false;
                if (EnableFeedback && Observed.Contacts > 0 &&
                    (glm::length(Reaction) > 0.0f || glm::length(AngularReaction) > 0.0f)) {
                    if (!Rigid.IsBodyValid(Rigids[RigidIndex]))
                        throw std::runtime_error("[Physics:Coupling] Box3D feedback target stale");
                    if (!Rigid.ApplyLinearImpulse(Rigids[RigidIndex], Reaction).Succeeded() ||
                        !Rigid.ApplyAngularImpulse(Rigids[RigidIndex], AngularReaction).Succeeded())
                        throw std::runtime_error("[Physics:Coupling] Box3D feedback impulse failed");
                    Applied = true;
                }
                MaximumFeedbackImpulse = std::max(MaximumFeedbackImpulse, static_cast<double>(glm::length(Observed.Impulse)));
                Feedback.push_back({{"ContactProjections", Observed.Contacts},
                                    {"ObservedReaction", {Observed.Impulse.x, Observed.Impulse.y, Observed.Impulse.z}},
                                    {"ObservedAngularReactionAtCOM", {RawAngular.x, RawAngular.y, RawAngular.z}},
                                    {"ClampScale", Scale},
                                    {"ClampedReaction", {Reaction.x, Reaction.y, Reaction.z}},
                                    {"ClampedAngularReaction", {AngularReaction.x, AngularReaction.y, AngularReaction.z}},
                                    {"AppliedReaction", Applied ? Json::array({Reaction.x, Reaction.y, Reaction.z}) : Json::array({0.0f, 0.0f, 0.0f})},
                                    {"AppliedAngularReaction", Applied ? Json::array({AngularReaction.x, AngularReaction.y, AngularReaction.z}) : Json::array({0.0f, 0.0f, 0.0f})},
                                    {"Applied", Applied}});
            }
            const double FeedbackMs = PhysicsJoltEvaluation::Milliseconds(FeedbackStart);
            PriorPositions.assign(Positions.begin(), Positions.end());
            Json RigidJson = Json::array();
            for (auto Id : Rigids) {
                auto State = Rigid.GetBodyState(Id);
                if (!State) throw std::runtime_error("[Physics:Coupling] Box3D feedback state missing");
                RigidJson.push_back({{"Position", {State->Transform.Position.x, State->Transform.Position.y,
                                                     State->Transform.Position.z}},
                                     {"Velocity", {State->LinearVelocity.x, State->LinearVelocity.y,
                                                     State->LinearVelocity.z}}, {"Mass", State->Mass}});
            }
            Frames.push_back({{"Frame", Frame}, {"RigidMs", RigidMs}, {"SoftMs", SoftMs},
                              {"SoftWorkerCount", Result.Profile.WorkerCount},
                              {"SoftWorkerUtilization", Result.Profile.WorkerUtilization},
                              {"FeedbackMs", FeedbackMs}, {"Rigids", std::move(RigidJson)},
                              {"Feedback", std::move(Feedback)},
                              {"SoftCenter", {SoftCenter.x, SoftCenter.y, SoftCenter.z}},
                              {"SoftMomentumEstimate", {SoftMomentum.x, SoftMomentum.y, SoftMomentum.z}},
                              {"SoftKineticEnergyEstimate", SoftKineticEnergy},
                              {"SoftVelocityObservationValid", SoftVelocityObservationValid},
                              {"MaxEdgeStretch", FrameStretch}, {"VolumeError", FrameVolumeError},
                              {"PinDrift", FramePinDrift}, {"InvertedTetrahedra", FrameInversions}});
        }
        Soft.Shutdown();
        return {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Status", "ExperimentalProjectionReaction"},
                {"ContactEnabled", EnableContact}, {"FeedbackEnabled", EnableFeedback},
                {"Frames", std::move(Frames)},
                {"Finite", Finite}, {"MaxPenetration", MaximumPenetration},
                {"MaxEdgeStretch", MaximumStretch}, {"MaxVolumeError", MaximumVolumeError},
                {"MaxPinDrift", MaximumPinDrift},
                {"InvertedTetrahedronFrames", InvertedTetrahedronFrames},
                {"MaxEstimatedFeedbackImpulse", MaximumFeedbackImpulse},
                {"TotalContactProjections", TotalContactProjections}, {"FirstContactFrame", FirstContactFrame},
                {"TotalProjectionsPerRigid", Json::array({TotalProjectionsPerRigid[0], TotalProjectionsPerRigid[1]})},
                {"DeltaTime", DeltaTime}, {"RigidSubsteps", 4},
                {"RigidShape", "Ball"}, {"RigidRadius", RigidRadius}, {"RigidDensity", RigidDensity},
                {"RequestedCpuBudget", Config.Workers},
                {"AllocatedSoftBackgroundWorkers", AllocatedBackgroundWorkers},
                {"ActiveSoftBackgroundWorkers", Config.Workers == 1 ? Json(0) : Json(nullptr)},
                {"WorkerActivityMeasurement", Config.Workers == 1 ? "Caller solves; reserved worker idle" :
                    "Unavailable; worker count is allocated capacity, utilization is recorded per frame"},
                {"SoftCallerRole", Config.Workers == 1 ? "SynchronousSolve" : "DispatchAndWait"},
                {"SoftExecutionMode", Config.Workers == 1 ? "SynchronousReference" : "Jobified"},
                {"MomentumConservationQualified", false},
                {"ControlSemantics", !EnableContact ? "No soft collider snapshots; no contact projection or feedback" :
                    (EnableFeedback ? "Soft collides with sampled Box rigid and observed reaction is applied" :
                     "Soft still collides with sampled Box rigid; observed reaction is not applied")},
                {"PublicationOrder", "Box rigid step, XPBD soft step against sampled collider, apply observed reaction to Box velocity, then sample; the new rigid position is integrated next frame"},
                {"MomentumScope", "Rigid linear momentum plus soft position-difference estimate from frame 1; first frame soft estimate is unavailable; damping and external boundary impulses are not integrated"},
                {"EnergyScope", "Rigid linear plus soft position-difference translational kinetic estimate from frame 1; rigid angular kinetic energy, strain potential and damping work are unmeasured"},
                {"ExternalWork", Rubber ? "Soft damping; collision projection and constraint work unmeasured" :
                    "Pinned edge reactions, soft damping and any controlled restart impulses; projection and constraint work unmeasured"},
                {"Limit", "The private XPBD copy observes each collision projection and transfers its opposite particle velocity change to Box3D at the next rigid safe point. This staggered reaction is clamped and is not a solved two-body constraint or conservation proof"}};
    }

    bool PhysicalQualityPass(const Json &Result, bool Rubber) {
        for (const char *Name : {"MaxEdgeStretch", "MaxVolumeError", "MaxPinDrift"})
            if (!Result.contains(Name) || !Result[Name].is_number() ||
                !std::isfinite(Result[Name].get<double>())) return false;
        const char *PenetrationName = Result.value("Backend", std::string{}) == "Jolt" ?
            "MaxParticlePenetration" : "MaxPenetration";
        if (!Result.contains(PenetrationName) || !Result[PenetrationName].is_number() ||
            !std::isfinite(Result[PenetrationName].get<double>()) ||
            !Result.contains("InvertedTetrahedronFrames")) return false;
        return Result.value("Finite", false) &&
            Result["MaxEdgeStretch"].get<double>() <= 0.1 &&
            Result["MaxPinDrift"].get<double>() <= 1.0e-4 &&
            Result[PenetrationName].get<double>() <= 0.1 &&
            (!Rubber || (Result["MaxVolumeError"].get<double>() <= 0.15 &&
                         Result["InvertedTetrahedronFrames"].get<std::size_t>() == 0));
    }

    Json CompareRigidResponse(const Json &Contact, const Json &Control, const Options &Config) {
        if (!Contact.contains("Frames") || !Control.contains("Frames") ||
            Contact["Frames"].size() != Control["Frames"].size())
            throw std::runtime_error("[Physics:Coupling] Control frame mismatch");
        const int Axis = Config.Case == "RubberPush" ? 0 : (Config.Case == "SphereOnCloth" ? 1 : 2);
        const double ExpectedSign = Config.Case == "RubberPush" || Config.Case == "SphereOnCloth" ? 1.0 : -1.0;
        const auto RigidCount = Contact["Frames"][0]["Rigids"].size();
        if (RigidCount < 1 || RigidCount > 2)
            throw std::runtime_error("[Physics:Coupling] Invalid rigid count");
        std::vector<double> VelocityDifferencePerRigid(RigidCount, 0.0);
        std::vector<double> DirectionalTransferPerRigid(RigidCount, 0.0);
        double MaximumVelocityDifference = 0.0;
        double MaximumSoftMomentumDifference = 0.0;
        bool MassMatched = true;
        for (std::size_t Frame = 0; Frame < Contact["Frames"].size(); ++Frame) {
            const auto &ContactFrame = Contact["Frames"][Frame];
            const auto &ControlFrame = Control["Frames"][Frame];
            if (ContactFrame["Rigids"].size() != ControlFrame["Rigids"].size())
                throw std::runtime_error("[Physics:Coupling] Control rigid count mismatch");
            for (std::size_t Rigid = 0; Rigid < ContactFrame["Rigids"].size(); ++Rigid) {
                double Squared = 0.0;
                for (int Component = 0; Component < 3; ++Component) {
                    const double Difference = ContactFrame["Rigids"][Rigid]["Velocity"][Component].get<double>() -
                        ControlFrame["Rigids"][Rigid]["Velocity"][Component].get<double>();
                    Squared += Difference * Difference;
                }
                VelocityDifferencePerRigid[Rigid] = std::max(VelocityDifferencePerRigid[Rigid], std::sqrt(Squared));
                MaximumVelocityDifference = std::max(MaximumVelocityDifference, VelocityDifferencePerRigid[Rigid]);
                const double Mass = ContactFrame["Rigids"][Rigid]["Mass"].get<double>();
                MassMatched &= std::abs(Mass / RigidMass - 1.0) <= 0.02;
                const double AxisDifference = ContactFrame["Rigids"][Rigid]["Velocity"][Axis].get<double>() -
                    ControlFrame["Rigids"][Rigid]["Velocity"][Axis].get<double>();
                const double Direction = Config.Case == "Multiple" && Rigid == 1 ? -ExpectedSign : ExpectedSign;
                DirectionalTransferPerRigid[Rigid] = std::max(DirectionalTransferPerRigid[Rigid],
                    Direction * Mass * AxisDifference);
            }
            const char *MomentumName = Contact.value("Backend", std::string{}) == "Jolt" ?
                "SoftMomentum" : "SoftMomentumEstimate";
            double SoftSquared = 0.0;
            for (int Component = 0; Component < 3; ++Component) {
                const double Difference = ContactFrame[MomentumName][Component].get<double>() -
                    ControlFrame[MomentumName][Component].get<double>();
                SoftSquared += Difference * Difference;
            }
            MaximumSoftMomentumDifference = std::max(MaximumSoftMomentumDifference, std::sqrt(SoftSquared));
        }
        const bool AllRigidsRespond = std::all_of(VelocityDifferencePerRigid.begin(), VelocityDifferencePerRigid.end(),
            [](double Difference) { return Difference > 0.01; }) &&
            std::all_of(DirectionalTransferPerRigid.begin(), DirectionalTransferPerRigid.end(),
            [](double Transfer) { return Transfer > 0.001; });
        return {{"MaxRigidVelocityDifference", MaximumVelocityDifference},
                {"VelocityDifferencePerRigid", VelocityDifferencePerRigid},
                {"DirectionalMomentumTransferPerRigid", DirectionalTransferPerRigid},
                {"AllRigidsRespond", AllRigidsRespond},
                {"MaxSoftMomentumDifference", MaximumSoftMomentumDifference},
                {"RigidMassMatchedToDensity", MassMatched},
                {"ExpectedMomentumAxis", Axis},
                {"ExpectedMomentumSigns", Config.Case == "Multiple" ?
                    Json::array({ExpectedSign, -ExpectedSign}) : Json::array({ExpectedSign})}};
    }

    Json EvaluateNecessaryEnergyBudget(const Json &Result, const Options &Config) {
        if (Result.value("Status", std::string{}) == "UnsupportedGeometry")
            return {{"Status", "UnsupportedGeometry"}, {"Qualified", nullptr}};
        if (Config.Case == "SphereOnCloth" || Config.Case == "Repeated")
            return {{"Status", "UnmeasuredExternalWork"}, {"Qualified", nullptr},
                    {"Reason", Config.Case == "SphereOnCloth" ?
                        "Gravity does positive work and its contribution is not integrated" :
                        "Rigid teleport and velocity restart work is not integrated"}};

        constexpr double SoftParticleMass = 0.1;
        constexpr double RubberParticleCount = 64.0;
        constexpr double AuthoredSoftSpeed = 4.0;
        constexpr double AuthoredRigidSpeed = 5.0;
        const double InitialSoftEnergy = Config.Case == "RubberPush" ?
            0.5 * RubberParticleCount * SoftParticleMass * AuthoredSoftSpeed * AuthoredSoftSpeed : 0.0;
        const int InitiallyMovingRigids = Config.Case == "Multiple" ? 2 :
            (Config.Case == "RigidIntoCloth" ? 1 : 0);
        const double InitialRigidEnergy = 0.5 * InitiallyMovingRigids * RigidMass *
            AuthoredRigidSpeed * AuthoredRigidSpeed;
        const double InitialEnergy = InitialSoftEnergy + InitialRigidEnergy;
        const double Limit = 1.05 * InitialEnergy + 1.0e-5;
        double MaximumObserved = 0.0;
        int MaximumFrame = -1;
        std::size_t ValidFrames = 0;
        const bool Jolt = Result.value("Backend", std::string{}) == "Jolt";
        for (const auto &Frame : Result["Frames"]) {
            if (!Jolt && !Frame.value("SoftVelocityObservationValid", false)) continue;
            double Energy = 0.0;
            if (Jolt) Energy = Frame["KineticEnergy"].get<double>();
            else {
                Energy = Frame["SoftKineticEnergyEstimate"].get<double>();
                for (const auto &Rigid : Frame["Rigids"]) {
                    const double Mass = Rigid["Mass"].get<double>();
                    const auto &Velocity = Rigid["Velocity"];
                    const double SpeedSquared = Velocity[0].get<double>() * Velocity[0].get<double>() +
                        Velocity[1].get<double>() * Velocity[1].get<double>() +
                        Velocity[2].get<double>() * Velocity[2].get<double>();
                    Energy += 0.5 * Mass * SpeedSquared;
                }
            }
            if (!std::isfinite(Energy) || Energy < 0.0)
                return {{"Status", "InvalidEnergyObservation"}, {"Qualified", false},
                        {"InitialTranslationalKineticEnergy", InitialEnergy}, {"Limit", Limit}};
            ++ValidFrames;
            if (Energy > MaximumObserved) {
                MaximumObserved = Energy;
                MaximumFrame = Frame["Frame"].get<int>();
            }
        }
        const bool Qualified = ValidFrames > 0 && MaximumObserved <= Limit;
        return {{"Status", Qualified ? "PassedNecessaryBound" : "ExceededNecessaryBound"},
                {"Qualified", Qualified}, {"InitialTranslationalKineticEnergy", InitialEnergy},
                {"InitialSoftTranslationalKineticEnergy", InitialSoftEnergy},
                {"InitialRigidTranslationalKineticEnergy", InitialRigidEnergy},
                {"AuthoredSoftParticleCount", Config.Case == "RubberPush" ? RubberParticleCount : 0.0},
                {"AuthoredSoftParticleMass", SoftParticleMass},
                {"AuthoredSoftSpeed", Config.Case == "RubberPush" ? AuthoredSoftSpeed : 0.0},
                {"AuthoredMovingRigidCount", InitiallyMovingRigids},
                {"AuthoredRigidSpeed", InitiallyMovingRigids > 0 ? AuthoredRigidSpeed : 0.0},
                {"MaximumObservedTranslationalKineticEnergy", MaximumObserved},
                {"MaximumObservedFrame", MaximumFrame}, {"ValidFrames", ValidFrames},
                {"Limit", Limit}, {"RelativeTolerance", 0.05}, {"AbsoluteTolerance", 1.0e-5},
                {"Scope", "Necessary rejection bound only; angular and strain energy and damping work are unmeasured"}};
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        const auto Config = Parse(ArgumentCount, Arguments);
        PhysicsEvaluation::InstallPhysicsAllocators();
        std::optional<PhysicsJoltEvaluation::JoltRuntime> Runtime;
        if (Config.Backend == "Jolt") Runtime.emplace();
        const auto Before = PhysicsEvaluation::CaptureResources();
        Json Result;
        if (Config.Backend == "Jolt") {
            Result = RunJolt(Config, true);
            // A separate world and identical initial state provide the contact-disabled control.
            const auto Control = RunJolt(Config, false);
            Result["ContactDisabledControl"] = Control;
            const auto Comparison = CompareRigidResponse(Result, Control, Config);
            Result["ControlComparison"] = Comparison;
            bool PerRigidContact = Result.contains("TotalContactsPerRigid") && Control.contains("TotalContactsPerRigid");
            for (std::size_t Index = 0; Index < Result["Frames"][0]["Rigids"].size() && PerRigidContact; ++Index)
                PerRigidContact = Result["TotalContactsPerRigid"][Index].get<std::uint64_t>() > 0 &&
                    Control["TotalContactsPerRigid"][Index].get<std::uint64_t>() == 0;
            Result["ContactEvidence"] = PerRigidContact;
            Result["PhysicalQualityQualified"] = PhysicalQualityPass(Result, Config.Case == "RubberPush");
            Result["ReciprocalRigidResponse"] = Comparison["AllRigidsRespond"].get<bool>();
        } else {
            Result = RunXpbd(Config, true, true);
            if (Result.value("Status", std::string{}) != "UnsupportedGeometry") {
                const auto Control = RunXpbd(Config, false, true);
                Result["FeedbackDisabledControl"] = Control;
                const auto Comparison = CompareRigidResponse(Result, Control, Config);
                Result["ControlComparison"] = Comparison;
                if (Config.Case == "RubberPush") {
                    const auto NoContactControl = RunXpbd(Config, false, false);
                    Result["ContactDisabledControl"] = NoContactControl;
                    Result["NoContactControlComparison"] = CompareRigidResponse(Result, NoContactControl, Config);
                }
                bool PerRigidContact = Result.contains("TotalProjectionsPerRigid") &&
                    Control.contains("TotalProjectionsPerRigid");
                for (std::size_t Index = 0; Index < Result["Frames"][0]["Rigids"].size() && PerRigidContact; ++Index)
                    PerRigidContact = Result["TotalProjectionsPerRigid"][Index].get<std::size_t>() > 0 &&
                        Control["TotalProjectionsPerRigid"][Index].get<std::size_t>() > 0;
                Result["ContactEvidence"] = PerRigidContact;
                Result["PhysicalQualityQualified"] = PhysicalQualityPass(Result, Config.Case == "RubberPush");
                Result["RigidResponseObserved"] = Comparison["AllRigidsRespond"].get<bool>();
            } else {
                Result["ContactEvidence"] = false;
                Result["PhysicalQualityQualified"] = false;
                Result["RigidResponseObserved"] = false;
            }
        }
        const bool ContactDeformationQualified = Result.value("ContactEvidence", false) &&
            Result.value("PhysicalQualityQualified", false);
        const bool ReciprocalRigidResponse = Result.value("ReciprocalRigidResponse",
            Result.value("RigidResponseObserved", false));
        const bool RigidMassMatched = Result.contains("ControlComparison") &&
            Result["ControlComparison"].value("RigidMassMatchedToDensity", false);
        const auto EnergyBudget = EvaluateNecessaryEnergyBudget(Result, Config);
        const bool EnergyBudgetQualified = EnergyBudget.contains("Qualified") &&
            EnergyBudget["Qualified"].is_boolean() && EnergyBudget["Qualified"].get<bool>();
        Result["ContactDeformationQualified"] = ContactDeformationQualified;
        Result["RigidResponseQualified"] = ReciprocalRigidResponse && RigidMassMatched;
        Result["NecessaryEnergyBudget"] = EnergyBudget;
        Result["QualityQualified"] = ContactDeformationQualified && ReciprocalRigidResponse &&
            RigidMassMatched && EnergyBudgetQualified;
        const auto BudgetStatus = EnergyBudget.value("Status", std::string{});
        Result["PhysicalQualificationStatus"] = BudgetStatus == "UnsupportedGeometry" ?
            "UnsupportedGeometry" : (BudgetStatus == "UnmeasuredExternalWork" ?
                "IncompleteExternalWork" :
                (Result["QualityQualified"].get<bool>() ? "PassedNecessaryBounds" : "FailedNecessaryBounds"));
        Result["QualityGates"] = {{"MaxEdgeStrain", 0.1}, {"MaxPinDrift", 1.0e-4},
                                  {"MaxPenetration", 0.1}, {"RubberVolumeError", 0.15},
                                  {"RubberInvertedTetrahedra", 0},
                                  {"MinimumDirectionalRigidMomentumTransfer", 0.001},
                                  {"MinimumRigidVelocityDifference", 0.01}};
        Result["QualityQualificationScope"] = "Contact evidence, bounded deformation, reciprocal rigid response and a necessary translational kinetic energy ceiling where external positive work is absent";
        Result["MomentumConservationStatus"] = "Incomplete: damping and external momentum impulses are not integrated";
        Result["EnergyConservationStatus"] = "Incomplete: angular kinetic energy, strain potential and damping work are unmeasured";
        Result["PhysicsEquivalentTimingQualified"] = false;
        Result["Resources"] = PhysicsEvaluation::ResourceDelta(Before, PhysicsEvaluation::CaptureResources());
        std::cout << Result.dump() << '\n';
        return Result.value("QualityQualified", false) ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cout << Json{{"Status", "Failed"}, {"Error", std::string("[Physics:Coupling] ") + Error.what()}}.dump() << '\n';
        return 2;
    }
}

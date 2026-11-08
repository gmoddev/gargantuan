#include "SoftFixtures.hpp"
#ifndef PHYSICS_UNIFIED_NATIVE_ONLY
#include "CouplingXpbdBackend.hpp"
#endif
#include "ExperimentSupport.hpp"
#include "ExperimentalVelocityReconstruction.hpp"
#ifndef PHYSICS_UNIFIED_NATIVE_ONLY
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"
#endif
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#ifdef PHYSICS_ENERGY_OBSERVER
#include <Jolt/Physics/SoftBody/SoftBodyUpdateContext.h>
#include <functional>
#include <mutex>
#endif

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

#ifdef PHYSICS_ENERGY_OBSERVER
namespace {
    std::function<void(const void *, const void *, double)> PhysicsJoltEnergyCallback;
}
extern "C" void PhysicsJoltEnergyState(const void *Motion, const void *Context,
                                         double PendingRigidKineticDelta) {
    if (PhysicsJoltEnergyCallback)
        PhysicsJoltEnergyCallback(Motion, Context, PendingRigidKineticDelta);
}
#endif

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
        int CollisionSteps = 1;
        int WorldUpdates = 1;
        int SoftIterations = 24;
        int ConstraintSweeps = 1;
        int VelocityReconstruction = 0;
        float StretchCompliance = 0.0f;
        bool ObserveAccepted = true;
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
            else if (Name == "--collision-steps") Result.CollisionSteps = std::stoi(Value);
            else if (Name == "--world-updates") Result.WorldUpdates = std::stoi(Value);
            else if (Name == "--soft-iterations") Result.SoftIterations = std::stoi(Value);
            else if (Name == "--constraint-sweeps") Result.ConstraintSweeps = std::stoi(Value);
            else if (Name == "--velocity-reconstruction") Result.VelocityReconstruction =
                PhysicsJoltEvaluation::ParseVelocityReconstruction(Value);
            else if (Name == "--stretch-compliance") Result.StretchCompliance = std::stof(Value);
            else if (Name == "--observe-accepted") {
                if (Value != "0" && Value != "1")
                    throw std::runtime_error("[Physics:Coupling] Invalid observe-accepted option");
                Result.ObserveAccepted = Value == "1";
            }
            else throw std::runtime_error("[Physics:Coupling] Unknown option");
        }
        if (Result.Backend != "Jolt" && Result.Backend != "XPBD") throw std::runtime_error("[Physics:Coupling] Invalid backend");
        if (Result.Case != "SphereOnCloth" && Result.Case != "RubberPush" && Result.Case != "RigidIntoCloth" &&
            Result.Case != "Repeated" && Result.Case != "Multiple")
            throw std::runtime_error("[Physics:Coupling] Invalid case");
        if (Result.Workers < 1 || Result.Workers > 8 || Result.Frames < 1 || Result.Frames > 600 ||
            Result.CollisionSteps < 1 || Result.CollisionSteps > 8 ||
            Result.WorldUpdates < 1 || Result.WorldUpdates > 384 ||
            Result.SoftIterations < 1 || Result.SoftIterations > 64 ||
            !std::isfinite(Result.StretchCompliance) || Result.StretchCompliance < 0.0f)
            throw std::runtime_error("[Physics:Coupling] Option outside bounded range");
        PhysicsJoltEvaluation::ValidateConstraintSweeps(Result.ConstraintSweeps, Result.VelocityReconstruction);
        if (Result.Backend != "Jolt" && Result.ConstraintSweeps != 1)
            throw std::runtime_error("[Physics:Coupling] Constraint sweeps are only available for native Jolt");
        return Result;
    }

    template <class VectorType> Json Vector(VectorType Value) {
        return Json::array({Value.GetX(), Value.GetY(), Value.GetZ()});
    }

    struct SoftObservation {
        JPH::Vec3 Center{0.0f, 0.0f, 0.0f};
        JPH::Vec3 Momentum{0.0f, 0.0f, 0.0f};
        double KineticEnergy = 0.0;
        double CompliantEdgeElasticEstimate = 0.0;
        double CompliantVolumeElasticEstimate = 0.0;
        double GravitationalPotentialChange = 0.0;
        double GravitationalPotentialAbsolute = 0.0;
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
                                const JPH::SoftBodySharedSettings &Shared,
                                JPH::Vec3 Gravity, JPH::RVec3 InitialOrigin,
                                JPH::Vec3 GravityReferenceOrigin,
                                const JPH::BodyLockInterface &LockInterface) {
        JPH::BodyLockRead Lock(LockInterface, Id);
        if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Soft body lock failed");
        const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
        const auto &Vertices = Motion->GetVertices();
        const auto Origin = Lock.GetBody().GetPosition();
        const JPH::Vec3 CurrentOrigin(static_cast<float>(Origin.GetX()),
            static_cast<float>(Origin.GetY()), static_cast<float>(Origin.GetZ()));
        const JPH::Vec3 RestOrigin(static_cast<float>(InitialOrigin.GetX()),
            static_cast<float>(InitialOrigin.GetY()), static_cast<float>(InitialOrigin.GetZ()));
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
            if (Volume.mCompliance > 0.0f) {
                const double Error = std::abs(static_cast<double>(SignedCurrent)) -
                    static_cast<double>(Volume.mSixRestVolume);
                Result.CompliantVolumeElasticEstimate += Error * Error /
                    (2.0 * static_cast<double>(Volume.mCompliance));
            }
        }
        for (const auto &Edge : Shared.mEdgeConstraints) if (Edge.mCompliance > 0.0f) {
            const double Error = static_cast<double>((Vertices[Edge.mVertex[1]].mPosition -
                Vertices[Edge.mVertex[0]].mPosition).Length()) - static_cast<double>(Edge.mRestLength);
            Result.CompliantEdgeElasticEstimate += Error * Error /
                (2.0 * static_cast<double>(Edge.mCompliance));
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
                const auto Rest = JPH::Vec3(Shared.mVertices[Index].mPosition);
                Result.GravitationalPotentialChange -= static_cast<double>(Mass) *
                    static_cast<double>(Gravity.Dot(Position + CurrentOrigin - Rest - RestOrigin));
                Result.GravitationalPotentialAbsolute -= static_cast<double>(Mass) *
                    static_cast<double>(Gravity.Dot(Position + CurrentOrigin - GravityReferenceOrigin));
                ++Result.MovableVertices;
            } else {
                const auto Rest = JPH::Vec3(Shared.mVertices[Index].mPosition);
                Result.MaxPinDrift = std::max(Result.MaxPinDrift, static_cast<double>((Position - Rest).Length()));
            }
        }
        if (!Vertices.empty()) Result.Center /= static_cast<float>(Vertices.size());
        Result.Center += CurrentOrigin;
        return Result;
    }

    Json RunJolt(const Options &Config, bool EnableContact) {
        PhysicsJoltEvaluation::ConfigureVelocityReconstruction(Config.VelocityReconstruction);
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
        std::size_t ChangedStretchEdges = 0;
        if (Config.StretchCompliance > 0.0f)
            for (auto &Edge : Shared->mEdgeConstraints)
                if (Edge.mCompliance == 0.0f) {
                    Edge.mCompliance = Config.StretchCompliance;
                    ++ChangedStretchEdges;
                }
        std::size_t ActiveEdgeConstraints = 0;
        std::size_t ActiveVolumeConstraints = 0;
        std::size_t NonpositiveActiveConstraints = 0;
        for (const auto &Edge : Shared->mEdgeConstraints)
            if (Shared->mVertices[Edge.mVertex[0]].mInvMass > 0.0f ||
                Shared->mVertices[Edge.mVertex[1]].mInvMass > 0.0f) {
                ++ActiveEdgeConstraints;
                if (!(Edge.mCompliance > 0.0f)) ++NonpositiveActiveConstraints;
            }
        for (const auto &Volume : Shared->mVolumeConstraints) {
            bool Active = false;
            for (const auto Vertex : Volume.mVertex)
                Active |= Shared->mVertices[Vertex].mInvMass > 0.0f;
            if (Active) {
                ++ActiveVolumeConstraints;
                if (!(Volume.mCompliance > 0.0f)) ++NonpositiveActiveConstraints;
            }
        }
        const bool UnsupportedConstraintKinds = !Shared->mDihedralBendConstraints.empty() ||
            !Shared->mSkinnedConstraints.empty() || !Shared->mLRAConstraints.empty() ||
            !Shared->mRodStretchShearConstraints.empty() || !Shared->mRodBendTwistConstraints.empty();
        PhysicsJoltEvaluation::ValidateVelocityConstraints(*Shared, Config.VelocityReconstruction);
        const bool ReferenceModelPreconditions = !UnsupportedConstraintKinds &&
            NonpositiveActiveConstraints == 0;
#ifdef PHYSICS_ENERGY_OBSERVER
        bool FullInternalObservationCoverage = Config.ObserveAccepted;
        constexpr bool EnergyObserverEnabled = true;
#else
        const bool FullInternalObservationCoverage = Config.ObserveAccepted &&
            Config.CollisionSteps == 1 && Config.SoftIterations == 1;
        constexpr bool EnergyObserverEnabled = false;
#endif
        if (IsRubber) for (auto &Vertex : Shared->mVertices) Vertex.mVelocity = JPH::Float3(4.0f, 0.0f, 0.0f);
        auto Soft = World.AddSoft(Shared, SoftOrigin, Config.SoftIterations, IsRubber, 2.4f,
                                  Config.ConstraintSweeps);
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
        double MaxAcceptedStretch = 0.0;
        double MaxAcceptedVolumeError = 0.0;
        double MaxAcceptedPinDrift = 0.0;
        double MaxAcceptedPenetration = 0.0;
        std::size_t InvertedTetrahedronFrames = 0;
        std::size_t InvertedTetrahedronAcceptedUpdates = 0;
        bool Finite = true;
        std::uint64_t TotalVertexContacts = 0;
        std::array<std::uint64_t, 2> TotalContactsPerRigid{};
        int FirstContactFrame = -1;
        std::vector<JPH::RVec3> InitialRigidPositions;
        for (auto Id : Rigids) InitialRigidPositions.push_back(World.World.GetBodyInterface().GetPosition(Id));
        const auto Gravity = World.World.GetGravity();
        JPH::Vec3 GravityReferenceOrigin = JPH::Vec3::sZero();
        for (const auto &Vertex : Shared->mVertices)
            GravityReferenceOrigin += JPH::Vec3(Vertex.mPosition);
        GravityReferenceOrigin /= static_cast<float>(Shared->mVertices.size());
        GravityReferenceOrigin += JPH::Vec3(static_cast<float>(SoftOrigin.GetX()),
            static_cast<float>(SoftOrigin.GetY()), static_cast<float>(SoftOrigin.GetZ()));
        const auto CaptureState = [&](int Frame, int Update, double StepMs,
                                      std::uint64_t UpdateContacts,
                                      const Json &UpdateContactsPerRigid,
                                      bool NoLock = false,
                                      double PendingRigidKineticDelta = 0.0) -> Json {
            const JPH::BodyLockInterface &LockInterface = NoLock ?
                static_cast<const JPH::BodyLockInterface &>(World.World.GetBodyLockInterfaceNoLock()) :
                static_cast<const JPH::BodyLockInterface &>(World.World.GetBodyLockInterface());
            const JPH::BodyInterface &BodyInterface = NoLock ?
                World.World.GetBodyInterfaceNoLock() : World.World.GetBodyInterface();
            const auto SoftState = ObserveSoft(World, Soft, *Shared, Gravity, SoftOrigin,
                GravityReferenceOrigin, LockInterface);
            bool StateFinite = SoftState.Finite;
            Json RigidStates = Json::array();
            JPH::Vec3 RigidMomentum = JPH::Vec3::sZero();
            double RigidLinearK = 0.0;
            double RigidAngularK = 0.0;
            double RigidGravitationalChange = 0.0;
            double RigidGravitationalAbsolute = 0.0;
            for (std::size_t Index = 0; Index < Rigids.size(); ++Index) {
                JPH::BodyLockRead Lock(LockInterface, Rigids[Index]);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Rigid body lock failed");
                const auto &Body = Lock.GetBody();
                const auto Velocity = Body.GetLinearVelocity();
                const auto AngularVelocity = Body.GetAngularVelocity();
                const float InverseMass = Body.GetMotionProperties()->GetInverseMass();
                const float Mass = InverseMass > 0.0f ? 1.0f / InverseMass : 0.0f;
                const auto Position = Body.GetPosition();
                StateFinite &= std::isfinite(Position.GetX()) && std::isfinite(Position.GetY()) &&
                    std::isfinite(Position.GetZ()) && std::isfinite(Velocity.GetX()) &&
                    std::isfinite(Velocity.GetY()) && std::isfinite(Velocity.GetZ()) &&
                    std::isfinite(AngularVelocity.GetX()) && std::isfinite(AngularVelocity.GetY()) &&
                    std::isfinite(AngularVelocity.GetZ());
                RigidMomentum += Mass * Velocity;
                RigidLinearK += 0.5 * static_cast<double>(Mass) * Velocity.LengthSq();
                const auto Inertia = Body.GetInverseInertia().Inversed3x3();
                const double AngularK = 0.5 * static_cast<double>(AngularVelocity.Dot(
                    Inertia.Multiply3x3(AngularVelocity)));
                RigidAngularK += AngularK;
                const JPH::Vec3 Displacement(static_cast<float>(Position.GetX() - InitialRigidPositions[Index].GetX()),
                    static_cast<float>(Position.GetY() - InitialRigidPositions[Index].GetY()),
                    static_cast<float>(Position.GetZ() - InitialRigidPositions[Index].GetZ()));
                const double GravitationalChange = -static_cast<double>(Mass) *
                    static_cast<double>(Gravity.Dot(Displacement));
                RigidGravitationalChange += GravitationalChange;
                const JPH::Vec3 WorldPosition(static_cast<float>(Position.GetX()),
                    static_cast<float>(Position.GetY()), static_cast<float>(Position.GetZ()));
                const double GravitationalAbsolute = -static_cast<double>(Mass) *
                    static_cast<double>(Gravity.Dot(WorldPosition - GravityReferenceOrigin));
                RigidGravitationalAbsolute += GravitationalAbsolute;
                RigidStates.push_back({{"Position", Vector(Position)}, {"Velocity", Vector(Velocity)},
                    {"AngularVelocity", Vector(AngularVelocity)}, {"Mass", Mass},
                    {"AngularKineticEnergy", AngularK},
                    {"GravitationalPotentialChange", GravitationalChange},
                    {"GravitationalPotentialAbsolute", GravitationalAbsolute}});
            }
            double Penetration = 0.0;
            {
                JPH::BodyLockRead Lock(LockInterface, Soft);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Jolt penetration lock failed");
                const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(
                    Lock.GetBody().GetMotionProperties());
                const auto SoftPosition = Lock.GetBody().GetPosition();
                for (const auto &Vertex : Motion->GetVertices()) {
                    const auto Position = Vertex.mPosition + JPH::Vec3(static_cast<float>(SoftPosition.GetX()),
                        static_cast<float>(SoftPosition.GetY()), static_cast<float>(SoftPosition.GetZ()));
                    for (auto Id : Rigids) {
                        const auto RigidPosition = BodyInterface.GetPosition(Id);
                        const auto Distance = (Position - JPH::Vec3(static_cast<float>(RigidPosition.GetX()),
                            static_cast<float>(RigidPosition.GetY()), static_cast<float>(RigidPosition.GetZ()))).Length();
                        Penetration = std::max(Penetration,
                            std::max(0.0, static_cast<double>(RigidRadius + 0.05f - Distance)));
                    }
                }
            }
            const double AngularInclusiveK = RigidLinearK + RigidAngularK +
                PendingRigidKineticDelta + SoftState.KineticEnergy;
            const double EdgeU = SoftState.CompliantEdgeElasticEstimate;
            const double VolumeU = SoftState.CompliantVolumeElasticEstimate;
            const double GravityPotential = SoftState.GravitationalPotentialChange + RigidGravitationalChange;
            const double GravityPotentialAbsolute = SoftState.GravitationalPotentialAbsolute +
                RigidGravitationalAbsolute;
            StateFinite &= std::isfinite(AngularInclusiveK) && std::isfinite(EdgeU) &&
                std::isfinite(VolumeU) && std::isfinite(GravityPotential) &&
                std::isfinite(GravityPotentialAbsolute);
            return {{"Frame", Frame}, {"WorldUpdate", Update}, {"StepMs", StepMs},
                    {"VertexContacts", UpdateContacts}, {"ContactsPerRigid", UpdateContactsPerRigid},
                    {"Rigids", std::move(RigidStates)},
                    {"SoftCenter", Vector(SoftState.Center)}, {"SoftMomentum", Vector(SoftState.Momentum)},
                    {"RigidMomentum", Vector(RigidMomentum)},
                    {"TotalMomentum", Vector(RigidMomentum + SoftState.Momentum)},
                    {"KineticEnergy", RigidLinearK + SoftState.KineticEnergy},
                    {"SoftKineticEnergy", SoftState.KineticEnergy},
                    {"RigidAngularKineticEnergy", RigidAngularK},
                    {"PendingRigidKineticDelta", PendingRigidKineticDelta},
                    {"AngularInclusiveKineticEnergy", AngularInclusiveK},
                    {"CompliantEdgeElasticEstimate", EdgeU},
                    {"CompliantVolumeElasticEstimate", VolumeU},
                    {"GravitationalPotentialChange", GravityPotential},
                    {"GravitationalPotentialAbsolute", GravityPotentialAbsolute},
                    {"ReferenceMechanicalEnergy", AngularInclusiveK + EdgeU + VolumeU + GravityPotential},
                    {"AbsoluteReferenceMechanicalEnergy", AngularInclusiveK + EdgeU + VolumeU +
                        GravityPotentialAbsolute},
                    {"MaxEdgeStretch", SoftState.MaxStretch}, {"VolumeError", SoftState.VolumeError},
                    {"PinDrift", SoftState.MaxPinDrift}, {"ParticlePenetration", Penetration},
                    {"InvertedTetrahedra", SoftState.InvertedTetrahedra}, {"Finite", StateFinite}};
        };
        const auto InitialState = CaptureState(-1, -1, 0.0, 0, Json::array());
        const double InitialAngularInclusiveK = InitialState["AngularInclusiveKineticEnergy"].get<double>();
        const double InitialEdgeU = InitialState["CompliantEdgeElasticEstimate"].get<double>();
        const double InitialVolumeU = InitialState["CompliantVolumeElasticEstimate"].get<double>();
        const double InitialReferenceMechanicalEnergy = InitialState["ReferenceMechanicalEnergy"].get<double>();
        const double InitialAbsoluteGravitationalPotential =
            InitialState["GravitationalPotentialAbsolute"].get<double>();
        const double InitialAbsoluteReferenceMechanicalEnergy =
            InitialState["AbsoluteReferenceMechanicalEnergy"].get<double>();
        double MaxAngularInclusiveK = InitialAngularInclusiveK;
        double MaxReferenceMechanicalEnergy = InitialReferenceMechanicalEnergy;
        double MaxAbsoluteReferenceMechanicalEnergy = InitialAbsoluteReferenceMechanicalEnergy;
        double SignedExternalWork = 0.0;
        double PositiveExternalWork = 0.0;
        double NegativeExternalWork = 0.0;
        double MaximumReferenceModelExcess = -std::numeric_limits<double>::infinity();
        int FirstReferenceModelFailureFrame = -1;
        int FirstReferenceModelFailureUpdate = -1;
        Json ExternalWorkEvents = Json::array();
        int MaxAngularInclusiveKFrame = -1;
        int MaxAngularInclusiveKUpdate = -1;
        int MaxReferenceMechanicalEnergyFrame = -1;
        int MaxReferenceMechanicalEnergyUpdate = -1;
        std::size_t ObservedInternalIntegrations = 0;
        const std::size_t ExpectedInternalIntegrations = static_cast<std::size_t>(Config.Frames) *
            static_cast<std::size_t>(Config.WorldUpdates) *
            static_cast<std::size_t>(Config.CollisionSteps) *
            static_cast<std::size_t>(Config.SoftIterations);
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (Config.Case == "Repeated" && Frame > 0 && Frame % 45 == 0) {
                const auto BeforeRestart = CaptureState(Frame, -1, 0.0, 0, Json::array());
                World.World.GetBodyInterface().SetPositionAndRotation(Rigids[0], JPH::RVec3(0.0, 0.0, -2.0),
                    JPH::Quat::sIdentity(), JPH::EActivation::Activate);
                World.World.GetBodyInterface().SetLinearVelocity(Rigids[0], JPH::Vec3(0.0f, 0.0f, 5.0f));
                const auto AfterRestart = CaptureState(Frame, -1, 0.0, 0, Json::array());
                const double Work = AfterRestart["AbsoluteReferenceMechanicalEnergy"].get<double>() -
                    BeforeRestart["AbsoluteReferenceMechanicalEnergy"].get<double>();
                SignedExternalWork += Work;
                if (Work >= 0.0) PositiveExternalWork += Work;
                else NegativeExternalWork += Work;
                ExternalWorkEvents.push_back({{"Frame", Frame}, {"Kind", "RigidTeleportAndVelocityRestart"},
                    {"BeforeAngularInclusiveKineticEnergy", BeforeRestart["AngularInclusiveKineticEnergy"]},
                    {"AfterAngularInclusiveKineticEnergy", AfterRestart["AngularInclusiveKineticEnergy"]},
                    {"BeforeAbsoluteReferenceMechanicalEnergy", BeforeRestart["AbsoluteReferenceMechanicalEnergy"]},
                    {"AfterAbsoluteReferenceMechanicalEnergy", AfterRestart["AbsoluteReferenceMechanicalEnergy"]},
                    {"SignedExternalWork", Work}, {"SignedExternalWorkToDate", SignedExternalWork}});
            }
            const auto FrameStart = std::chrono::steady_clock::now();
            Json AcceptedUpdates = Json::array();
            Json FinalState;
            int FrameInternalCallbacks = 0;
            std::uint64_t FrameContacts = 0;
            std::array<std::uint64_t, 2> FrameContactCounts{};
            double FrameStepMs = 0.0;
            for (int Update = 0; Update < Config.WorldUpdates; ++Update) {
                double StepMs = 0.0;
#ifdef PHYSICS_ENERGY_OBSERVER
                std::exception_ptr CallbackError;
                std::mutex CallbackMutex;
                int InternalOrdinal = 0;
                if (Config.ObserveAccepted) PhysicsJoltEnergyCallback = [&](const void *Motion,
                    const void *Context, double PendingRigidKineticDelta) {
                    std::lock_guard Guard(CallbackMutex);
                    try {
                        JPH::BodyLockRead SoftLock(World.World.GetBodyLockInterfaceNoLock(), Soft);
                        if (!SoftLock.Succeeded() || Motion != SoftLock.GetBody().GetMotionProperties())
                            throw std::runtime_error("[Physics:Coupling] Energy observer soft body mismatch");
                        const auto *UpdateContext = static_cast<const JPH::SoftBodyUpdateContext *>(Context);
                        auto State = CaptureState(Frame, Update, 0.0, 0, Json::array(), true,
                            PendingRigidKineticDelta);
                        State["VertexContacts"] = nullptr;
                        State["ContactsPerRigid"] = nullptr;
                        State["InternalOrdinalPerWorldUpdate"] = InternalOrdinal;
                        State["CollisionStepOrdinal"] = InternalOrdinal / Config.SoftIterations;
                        State["SoftIterationOrdinal"] = InternalOrdinal % Config.SoftIterations;
                        State["ContextNextIteration"] = UpdateContext->mNextIteration.load();
                        ++InternalOrdinal;
                        ++FrameInternalCallbacks;
                        ++ObservedInternalIntegrations;
                        Finite &= State["Finite"].get<bool>();
                        MaxAcceptedStretch = std::max(MaxAcceptedStretch,
                            State["MaxEdgeStretch"].get<double>());
                        MaxAcceptedVolumeError = std::max(MaxAcceptedVolumeError,
                            State["VolumeError"].get<double>());
                        MaxAcceptedPinDrift = std::max(MaxAcceptedPinDrift,
                            State["PinDrift"].get<double>());
                        MaxAcceptedPenetration = std::max(MaxAcceptedPenetration,
                            State["ParticlePenetration"].get<double>());
                        InvertedTetrahedronAcceptedUpdates +=
                            State["InvertedTetrahedra"].get<std::size_t>();
                        const double K = State["AngularInclusiveKineticEnergy"].get<double>();
                        const double Reference = State["ReferenceMechanicalEnergy"].get<double>();
                        const double AbsoluteReference =
                            State["AbsoluteReferenceMechanicalEnergy"].get<double>();
                        const double Budget = InitialAbsoluteReferenceMechanicalEnergy + SignedExternalWork;
                        const double Tolerance = 0.05 *
                            (InitialAbsoluteReferenceMechanicalEnergy + PositiveExternalWork) + 1.0e-5;
                        const double Excess = AbsoluteReference - (Budget + Tolerance);
                        State["SignedExternalWorkToDate"] = SignedExternalWork;
                        State["PositiveExternalWorkToDate"] = PositiveExternalWork;
                        State["NegativeExternalWorkToDate"] = NegativeExternalWork;
                        State["ReferenceModelBudget"] = Budget;
                        State["ReferenceModelTolerance"] = Tolerance;
                        State["ReferenceModelExcess"] = Excess;
                        if (!std::isfinite(Excess) || Excess > 0.0) {
                            if (FirstReferenceModelFailureFrame < 0) {
                                FirstReferenceModelFailureFrame = Frame;
                                FirstReferenceModelFailureUpdate = Update;
                            }
                        }
                        MaximumReferenceModelExcess = std::max(MaximumReferenceModelExcess, Excess);
                        if (K > MaxAngularInclusiveK) {
                            MaxAngularInclusiveK = K;
                            MaxAngularInclusiveKFrame = Frame;
                            MaxAngularInclusiveKUpdate = Update;
                        }
                        if (Reference > MaxReferenceMechanicalEnergy) {
                            MaxReferenceMechanicalEnergy = Reference;
                            MaxReferenceMechanicalEnergyFrame = Frame;
                            MaxReferenceMechanicalEnergyUpdate = Update;
                        }
                        MaxAbsoluteReferenceMechanicalEnergy = std::max(
                            MaxAbsoluteReferenceMechanicalEnergy, AbsoluteReference);
                        AcceptedUpdates.push_back(std::move(State));
                    } catch (...) {
                        if (!CallbackError) CallbackError = std::current_exception();
                    }
                };
                try {
#endif
                    StepMs = World.Step(DeltaTime / static_cast<float>(Config.WorldUpdates),
                        Config.CollisionSteps);
#ifdef PHYSICS_ENERGY_OBSERVER
                } catch (...) {
                    PhysicsJoltEnergyCallback = {};
                    throw;
                }
                PhysicsJoltEnergyCallback = {};
                if (CallbackError) std::rethrow_exception(CallbackError);
                if (Config.ObserveAccepted && InternalOrdinal !=
                    Config.CollisionSteps * Config.SoftIterations)
                    FullInternalObservationCoverage = false;
#endif
                FrameStepMs += StepMs;
                const auto UpdateContacts = Contacts.VertexContacts.exchange(0, std::memory_order_relaxed);
                FrameContacts += UpdateContacts;
                TotalVertexContacts += UpdateContacts;
                Json UpdateContactsPerRigid = Json::array();
                for (std::size_t Index = 0; Index < Rigids.size(); ++Index) {
                    const auto Count = Contacts.TargetContacts[Index].exchange(0, std::memory_order_relaxed);
                    FrameContactCounts[Index] += Count;
                    TotalContactsPerRigid[Index] += Count;
                    UpdateContactsPerRigid.push_back(Count);
                }
                if (!Config.ObserveAccepted && Update + 1 != Config.WorldUpdates) continue;
                auto State = CaptureState(Frame, Update, StepMs, UpdateContacts, UpdateContactsPerRigid);
                Finite &= State["Finite"].get<bool>();
                MaxAcceptedStretch = std::max(MaxAcceptedStretch, State["MaxEdgeStretch"].get<double>());
                MaxAcceptedVolumeError = std::max(MaxAcceptedVolumeError, State["VolumeError"].get<double>());
                MaxAcceptedPinDrift = std::max(MaxAcceptedPinDrift, State["PinDrift"].get<double>());
                MaxAcceptedPenetration = std::max(MaxAcceptedPenetration,
                    State["ParticlePenetration"].get<double>());
                InvertedTetrahedronAcceptedUpdates += State["InvertedTetrahedra"].get<std::size_t>();
                const double K = State["AngularInclusiveKineticEnergy"].get<double>();
                const double Reference = State["ReferenceMechanicalEnergy"].get<double>();
                const double AbsoluteReference = State["AbsoluteReferenceMechanicalEnergy"].get<double>();
                const double ReferenceBudget = InitialAbsoluteReferenceMechanicalEnergy + SignedExternalWork;
                const double ReferenceTolerance = 0.05 *
                    (InitialAbsoluteReferenceMechanicalEnergy + PositiveExternalWork) + 1.0e-5;
                const double ReferenceExcess = AbsoluteReference -
                    (ReferenceBudget + ReferenceTolerance);
                State["SignedExternalWorkToDate"] = SignedExternalWork;
                State["PositiveExternalWorkToDate"] = PositiveExternalWork;
                State["NegativeExternalWorkToDate"] = NegativeExternalWork;
                State["ReferenceModelBudget"] = ReferenceBudget;
                State["ReferenceModelTolerance"] = ReferenceTolerance;
                State["ReferenceModelExcess"] = ReferenceExcess;
                if (!std::isfinite(ReferenceExcess) || ReferenceExcess > 0.0) {
                    if (FirstReferenceModelFailureFrame < 0) {
                        FirstReferenceModelFailureFrame = Frame;
                        FirstReferenceModelFailureUpdate = Update;
                    }
                }
                if (ReferenceExcess > MaximumReferenceModelExcess)
                    MaximumReferenceModelExcess = ReferenceExcess;
                if (K > MaxAngularInclusiveK) {
                    MaxAngularInclusiveK = K;
                    MaxAngularInclusiveKFrame = Frame;
                    MaxAngularInclusiveKUpdate = Update;
                }
                if (Reference > MaxReferenceMechanicalEnergy) {
                    MaxReferenceMechanicalEnergy = Reference;
                    MaxReferenceMechanicalEnergyFrame = Frame;
                    MaxReferenceMechanicalEnergyUpdate = Update;
                }
                MaxAbsoluteReferenceMechanicalEnergy = std::max(
                    MaxAbsoluteReferenceMechanicalEnergy, AbsoluteReference);
#ifndef PHYSICS_ENERGY_OBSERVER
                if (Config.ObserveAccepted) AcceptedUpdates.push_back(State);
#endif
                FinalState = std::move(State);
            }
            if (FrameContacts > 0 && FirstContactFrame < 0) FirstContactFrame = Frame;
            Json FrameState = std::move(FinalState);
            FrameState.erase("WorldUpdate");
            FrameState["StepMs"] = FrameStepMs;
            FrameState["VertexContacts"] = FrameContacts;
            Json FrameContactsPerRigid = Json::array();
            for (std::size_t Index = 0; Index < Rigids.size(); ++Index)
                FrameContactsPerRigid.push_back(FrameContactCounts[Index]);
            FrameState["ContactsPerRigid"] = std::move(FrameContactsPerRigid);
            MaxStretch = std::max(MaxStretch, FrameState["MaxEdgeStretch"].get<double>());
            MaxVolumeError = std::max(MaxVolumeError, FrameState["VolumeError"].get<double>());
            MaxPinDrift = std::max(MaxPinDrift, FrameState["PinDrift"].get<double>());
            MaxParticlePenetration = std::max(MaxParticlePenetration,
                FrameState["ParticlePenetration"].get<double>());
            InvertedTetrahedronFrames += FrameState["InvertedTetrahedra"].get<std::size_t>();
            if (Config.ObserveAccepted) FrameState["AcceptedUpdates"] = std::move(AcceptedUpdates);
            FrameState["ObservedInternalCallbacks"] = FrameInternalCallbacks;
            FrameState["ExpectedInternalCallbacks"] = Config.WorldUpdates *
                Config.CollisionSteps * Config.SoftIterations;
            FrameState["FrameWallMs"] = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - FrameStart).count();
            Frames.push_back(std::move(FrameState));
        }
#ifndef PHYSICS_ENERGY_OBSERVER
        if (FullInternalObservationCoverage)
            ObservedInternalIntegrations = ExpectedInternalIntegrations;
#else
        if (ObservedInternalIntegrations != ExpectedInternalIntegrations && Config.ObserveAccepted)
            FullInternalObservationCoverage = false;
#endif
        return {{"Backend", "Jolt"}, {"Case", Config.Case}, {"ContactEnabled", EnableContact},
                {"Frames", std::move(Frames)}, {"Finite", Finite}, {"MaxEdgeStretch", MaxStretch},
                {"MaxVolumeError", MaxVolumeError}, {"MaxPinDrift", MaxPinDrift},
                {"MaxParticlePenetration", MaxParticlePenetration},
                {"InvertedTetrahedronFrames", InvertedTetrahedronFrames},
                {"MaxAcceptedEdgeStretch", MaxAcceptedStretch},
                {"MaxAcceptedVolumeError", MaxAcceptedVolumeError},
                {"MaxAcceptedPinDrift", MaxAcceptedPinDrift},
                {"MaxAcceptedParticlePenetration", MaxAcceptedPenetration},
                {"InvertedTetrahedronAcceptedUpdates", InvertedTetrahedronAcceptedUpdates},
                {"InitialAngularInclusiveKineticEnergy", InitialAngularInclusiveK},
                {"InitialCompliantEdgeElasticEstimate", InitialEdgeU},
                {"InitialCompliantVolumeElasticEstimate", InitialVolumeU},
                {"InitialReferenceMechanicalEnergy", InitialReferenceMechanicalEnergy},
                {"InitialAbsoluteGravitationalPotential", InitialAbsoluteGravitationalPotential},
                {"GravityReferenceOrigin", Vector(GravityReferenceOrigin)},
                {"InitialAbsoluteReferenceMechanicalEnergy", InitialAbsoluteReferenceMechanicalEnergy},
                {"MaxAngularInclusiveKineticEnergy", MaxAngularInclusiveK},
                {"MaxAngularInclusiveKineticEnergyFrame", MaxAngularInclusiveKFrame},
                {"MaxAngularInclusiveKineticEnergyUpdate", MaxAngularInclusiveKUpdate},
                {"MaxReferenceMechanicalEnergy", MaxReferenceMechanicalEnergy},
                {"MaxAbsoluteReferenceMechanicalEnergy", MaxAbsoluteReferenceMechanicalEnergy},
                {"MaxReferenceMechanicalEnergyFrame", MaxReferenceMechanicalEnergyFrame},
                {"MaxReferenceMechanicalEnergyUpdate", MaxReferenceMechanicalEnergyUpdate},
                {"ExternalWorkEvents", std::move(ExternalWorkEvents)},
                {"SignedExternalWork", SignedExternalWork},
                {"PositiveExternalWork", PositiveExternalWork},
                {"NegativeExternalWork", NegativeExternalWork},
                {"MaximumReferenceModelExcess", MaximumReferenceModelExcess},
                {"FirstReferenceModelFailureFrame", FirstReferenceModelFailureFrame},
                {"FirstReferenceModelFailureUpdate", FirstReferenceModelFailureUpdate},
                {"ReferenceModelGateQualified", FullInternalObservationCoverage ?
                    Json(ReferenceModelPreconditions && Finite && FirstReferenceModelFailureFrame < 0) :
                    Json(nullptr)},
                {"ReferenceModelPreconditionsQualified", ReferenceModelPreconditions},
                {"ActiveEdgeConstraints", ActiveEdgeConstraints},
                {"ActiveVolumeConstraints", ActiveVolumeConstraints},
                {"NonpositiveActiveConstraints", NonpositiveActiveConstraints},
                {"UnsupportedConstraintKinds", UnsupportedConstraintKinds},
                {"WorldUpdateObservationCoverage", Config.ObserveAccepted},
                {"AcceptedObservationCoverage", FullInternalObservationCoverage},
                {"EnergyObserverEnabled", EnergyObserverEnabled},
                {"EnergyObserverPoint", EnergyObserverEnabled ?
                    "After each soft collision velocity update, before cached rigid velocity writeback and rod velocity update; current fixtures contain no rods, and pending rigid kinetic delta corrects the scalar energy" :
                    "Completed PhysicsSystem::Update world boundary"},
                {"ObservedInternalIntegrations", ObservedInternalIntegrations},
                {"ExpectedInternalIntegrations", ExpectedInternalIntegrations},
                {"RequestedCpuBudget", Config.Workers},
                {"AllocatedBackgroundWorkers", BackgroundWorkers},
                {"ActiveBackgroundWorkers", nullptr},
                {"WorkerActivityMeasurement", "Unavailable; thread pool capacity is not per-frame active worker count"},
                {"CallerRole", "ParticipatesInSolve"}, {"ExecutionMode", "JoltThreadPool"},
                {"CollisionSteps", Config.CollisionSteps}, {"SolverIterations", Config.SoftIterations},
                {"ConstraintSweeps", Config.ConstraintSweeps},
                {"ConstraintProjectionsPerFrame", Config.WorldUpdates * Config.CollisionSteps *
                    Config.SoftIterations * Config.ConstraintSweeps},
                {"WorldUpdatesPerFrame", Config.WorldUpdates},
                {"VelocityReconstruction", PhysicsJoltEvaluation::VelocityReconstructionName(Config.VelocityReconstruction)},
                {"NumericalReconstructionPatch", PhysicsJoltEvaluation::NumericalReconstructionPatch},
                {"SoftIntegrationsPerFrame", Config.WorldUpdates * Config.CollisionSteps * Config.SoftIterations},
                {"SoftIntegrationDt", DeltaTime /
                    static_cast<float>(Config.WorldUpdates * Config.CollisionSteps * Config.SoftIterations)},
                {"SelectedStretchCompliance", Config.StretchCompliance},
                {"ChangedHardStretchEdgeCount", ChangedStretchEdges},
                {"DeltaTime", DeltaTime},
                {"SchedulingScope", Config.ObserveAccepted ?
                    "World.Step(dt/world-updates, collision-steps) repeated; every accepted update sampled after completion" :
                    "World.Step(dt/world-updates, collision-steps) repeated; only final frame state sampled"},
                {"TimingScope", Config.ObserveAccepted ?
                    "StepMs sums solver World.Step durations; FrameWallMs also includes accepted-update observation and JSON construction, but excludes controlled restart setup" :
                    "StepMs sums solver World.Step durations; FrameWallMs includes one final state observation and JSON construction per frame, but excludes controlled restart setup"},
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
                {"EnergyScope", "KineticEnergy retains original rigid linear plus movable soft translational scope; AngularInclusiveKineticEnergy adds measured rigid rotational energy"},
                {"ReferenceMechanicalEnergyScope", "Angular-inclusive kinetic plus positive-compliance edge C^2/(2c), positive-compliance six-volume C^2/(2c), and gravitational potential. ReferenceMechanicalEnergy uses potential change from initial state; AbsoluteReferenceMechanicalEnergy uses gravitational potential relative to the recorded initial mean soft-vertex world position. Restart teleport/velocity work is measured separately. The complete gate requires positive compliance on all active constraints and every internal integration observed, either through the read-only observer or collision-steps=1 and soft-iterations=1; it is a bounded reference-model check, not complete Jolt solver-internal conservation"},
                {"ExternalWork", "Rigid and soft gravity potential and controlled rigid teleport/velocity restart work are measured. Soft damping and collision/constraint solver-internal work are not individually balanced; fixed pin work is zero"},
                {"ControlledRestarts", Config.Case == "Repeated" ? (Config.Frames - 1) / 45 : 0}};
    }

#ifdef PHYSICS_UNIFIED_NATIVE_ONLY
    Json RunXpbd(const Options &Config, bool, bool) {
        return {{"Backend", "XPBD"}, {"Case", Config.Case},
                {"Status", "UnsupportedBackend"},
                {"Reason", "This standalone unified coupling executable contains only the pristine Jolt path"}};
    }
#else
    Json RunXpbd(const Options &Config, bool EnableFeedback, bool EnableContact) {
        if (Config.WorldUpdates != 1 || Config.SoftIterations != 24 ||
            Config.StretchCompliance != 0.0f || !Config.ObserveAccepted)
            return {{"Backend", "XPBD"}, {"Case", Config.Case},
                    {"Status", "UnsupportedExperimentalProfile"},
                    {"Reason", "The historical XPBD path does not implement the unified Jolt world-update, soft-iteration, stretch-compliance, or frame-only observation profile"}};
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
            (void)Rigid.Step({.DeltaTime = DeltaTime, .SubStepCount = Config.CollisionSteps});
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
            const auto AcceptedDynamics = FeedbackBackend->ReadAcceptedDynamics(
                SoftId, Found->VertexRevision);
            if (!AcceptedDynamics)
                throw std::runtime_error("[Physics:Coupling] XPBD accepted dynamics unavailable or stale");
            Finite &= AcceptedDynamics->Finite;
            const auto &Positions = *Found->Positions;
            glm::vec3 SoftCenter(0.0f);
            glm::vec3 DisplacementMomentum(0.0f);
            double DisplacementKineticEnergy = 0.0;
            const bool DisplacementObservationValid = PriorPositions.size() == Positions.size();
            for (const auto &Position : Positions) {
                Finite &= std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
                SoftCenter += Position;
            }
            SoftCenter /= static_cast<float>(Positions.size());
            for (std::size_t Vertex = 0; Vertex < Positions.size(); ++Vertex) {
                if (!Rubber && Vertex < Definition.ResolutionX) continue;
                if (!DisplacementObservationValid) continue;
                const auto Velocity = (Positions[Vertex] - PriorPositions[Vertex]) / DeltaTime;
                DisplacementMomentum += Definition.Material.ParticleMass * Velocity;
                DisplacementKineticEnergy += 0.5 * Definition.Material.ParticleMass * glm::dot(Velocity, Velocity);
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
                const auto At = [N](int X, int Y, int Z) -> std::size_t {
                    return (static_cast<std::size_t>(Z) * N + Y) * N + X;
                };
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
                              {"SoftMomentum", AcceptedDynamics->Finite ?
                                  Json::array({AcceptedDynamics->Momentum[0], AcceptedDynamics->Momentum[1],
                                               AcceptedDynamics->Momentum[2]}) : Json(nullptr)},
                              {"SoftKineticEnergy", AcceptedDynamics->Finite ?
                                  Json(AcceptedDynamics->TranslationalKineticEnergy) : Json(nullptr)},
                              {"SoftMovableMass", AcceptedDynamics->MovableMass},
                              {"SoftMovableVertices", AcceptedDynamics->MovableVertices},
                              {"AcceptedVelocityFinite", AcceptedDynamics->Finite},
                              {"SoftDisplacementMomentumEstimate", {DisplacementMomentum.x,
                                  DisplacementMomentum.y, DisplacementMomentum.z}},
                              {"SoftDisplacementKineticEnergyEstimate", DisplacementKineticEnergy},
                              {"SoftDisplacementObservationValid", DisplacementObservationValid},
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
                {"DeltaTime", DeltaTime}, {"CollisionSteps", Config.CollisionSteps},
                {"RigidSubsteps", Config.CollisionSteps}, {"SoftSubsteps", 1},
                {"SchedulingScope", "Box rigid uses the requested substeps, then XPBD soft runs once at full frame delta time before feedback publication"},
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
                {"MomentumScope", "Rigid linear momentum plus accepted private XPBD node velocity momentum; frame displacement estimate is diagnostic only; damping and external boundary impulses are not integrated"},
                {"EnergyScope", "Rigid linear plus accepted private XPBD node translational kinetic energy; frame displacement estimate is diagnostic only; rigid angular kinetic energy, strain potential and damping work are unmeasured"},
                {"ExternalWork", Rubber ? "Soft damping; collision projection and constraint work unmeasured" :
                    "Pinned edge reactions, soft damping and any controlled restart impulses; projection and constraint work unmeasured"},
                {"Limit", "The private XPBD copy observes each collision projection and transfers its opposite particle velocity change to Box3D at the next rigid safe point. This staggered reaction is clamped and is not a solved two-body constraint or conservation proof"}};
    }

#endif

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
        bool SoftMomentumAvailable = true;
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
            if (!ContactFrame["SoftMomentum"].is_array() ||
                !ControlFrame["SoftMomentum"].is_array()) {
                SoftMomentumAvailable = false;
                continue;
            }
            double SoftSquared = 0.0;
            for (int Component = 0; Component < 3; ++Component) {
                const double Difference = ContactFrame["SoftMomentum"][Component].get<double>() -
                    ControlFrame["SoftMomentum"][Component].get<double>();
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
                {"MaxSoftMomentumDifference", SoftMomentumAvailable ?
                    Json(MaximumSoftMomentumDifference) : Json(nullptr)},
                {"SoftMomentumAvailable", SoftMomentumAvailable},
                {"RigidMassMatchedToDensity", MassMatched},
                {"ExpectedMomentumAxis", Axis},
                {"ExpectedMomentumSigns", Config.Case == "Multiple" ?
                    Json::array({ExpectedSign, -ExpectedSign}) : Json::array({ExpectedSign})}};
    }

    Json EvaluateNecessaryEnergyBudget(const Json &Result, const Options &Config) {
        if (Result.value("Status", std::string{}) == "UnsupportedGeometry" ||
            Result.value("Status", std::string{}) == "UnsupportedExperimentalProfile" ||
            Result.value("Status", std::string{}) == "UnsupportedBackend")
            return {{"Status", Result.value("Status", std::string{})}, {"Qualified", nullptr}};
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
            if (!Jolt && !Frame.value("AcceptedVelocityFinite", false))
                return {{"Status", "InvalidEnergyObservation"}, {"Qualified", false},
                        {"InitialTranslationalKineticEnergy", InitialEnergy}, {"Limit", Limit}};
            double Energy = 0.0;
            if (Jolt) Energy = Frame["KineticEnergy"].get<double>();
            else {
                Energy = Frame["SoftKineticEnergy"].get<double>();
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

    Json EvaluateAcceptedEnergyBudget(const Json &Result, const Options &Config) {
        if (Result.value("Backend", std::string{}) != "Jolt")
            return {{"Status", "UnsupportedBackend"}, {"Qualified", nullptr}};
        if (Config.Case == "SphereOnCloth" || Config.Case == "Repeated")
            return {{"Status", "UseReferenceModelWithMeasuredExternalWork"}, {"Qualified", nullptr},
                    {"Reason", Config.Case == "SphereOnCloth" ?
                        "Gravity potential is included in the separate accepted reference-model gate" :
                        "Rigid teleport and restart work are included in the separate accepted reference-model gate"}};
        const double Initial = Result["InitialAngularInclusiveKineticEnergy"].get<double>();
        const double Maximum = Result["MaxAngularInclusiveKineticEnergy"].get<double>();
        const double Limit = 1.05 * Initial + 1.0e-5;
        const bool Valid = std::isfinite(Initial) && Initial >= 0.0 &&
            std::isfinite(Maximum) && Maximum >= Initial;
        const bool Qualified = Valid && Maximum <= Limit;
        return {{"Status", !Valid ? "InvalidEnergyObservation" :
                    (Qualified ? "PassedNecessaryBound" : "ExceededNecessaryBound")},
                {"Qualified", Qualified}, {"InitialAngularInclusiveKineticEnergy", Initial},
                {"MaximumObservedAngularInclusiveKineticEnergy", Maximum},
                {"MaximumFrame", Result["MaxAngularInclusiveKineticEnergyFrame"]},
                {"MaximumWorldUpdate", Result["MaxAngularInclusiveKineticEnergyUpdate"]},
                {"Limit", Limit}, {"RelativeTolerance", 0.05}, {"AbsoluteTolerance", 1.0e-5},
                {"Scope", "Necessary rejection bound at every accepted world update; includes rigid angular and movable soft kinetic energy, excludes stored strain, damping and external work"}};
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
            if (Result.value("Status", std::string{}) != "UnsupportedGeometry" &&
                Result.value("Status", std::string{}) != "UnsupportedExperimentalProfile" &&
                Result.value("Status", std::string{}) != "UnsupportedBackend") {
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
        if (Config.Backend == "Jolt") {
            const bool FullInternalObservationCoverage = Result.value("AcceptedObservationCoverage", false);
            const bool AcceptedPhysicalQuality = FullInternalObservationCoverage && Result.value("Finite", false) &&
                Result["MaxAcceptedEdgeStretch"].get<double>() <= 0.1 &&
                Result["MaxAcceptedPinDrift"].get<double>() <= 1.0e-4 &&
                Result["MaxAcceptedParticlePenetration"].get<double>() <= 0.1 &&
                (Config.Case != "RubberPush" ||
                    (Result["MaxAcceptedVolumeError"].get<double>() <= 0.15 &&
                     Result["InvertedTetrahedronAcceptedUpdates"].get<std::size_t>() == 0));
            const auto AcceptedBudget = FullInternalObservationCoverage ?
                EvaluateAcceptedEnergyBudget(Result, Config) :
                Json{{"Status", Config.ObserveAccepted ? "UnqualifiedInternalIntegrationCoverage" :
                    "UnqualifiedFrameOnlyObservation"}, {"Qualified", nullptr}};
            const bool AcceptedBudgetQualified = AcceptedBudget.contains("Qualified") &&
                AcceptedBudget["Qualified"].is_boolean() &&
                AcceptedBudget["Qualified"].get<bool>();
            const bool ReferenceModelQualified = Result["ReferenceModelGateQualified"].is_boolean() &&
                Result["ReferenceModelGateQualified"].get<bool>();
            const bool ClosedCase = Config.Case != "SphereOnCloth" && Config.Case != "Repeated";
            Result["AcceptedPhysicalQualityQualified"] = FullInternalObservationCoverage ?
                Json(AcceptedPhysicalQuality) : Json(nullptr);
            Result["AcceptedNecessaryEnergyBudget"] = AcceptedBudget;
            Result["AcceptedQualityQualified"] = FullInternalObservationCoverage ? Json(AcceptedPhysicalQuality &&
                Result.value("ContactEvidence", false) && ReciprocalRigidResponse &&
                RigidMassMatched && ReferenceModelQualified &&
                (!ClosedCase || AcceptedBudgetQualified)) : Json(nullptr);
            Result["AcceptedQualityScope"] = FullInternalObservationCoverage ?
                "Every internal integration endpoint is observed: contact geometry, mass, measured angular-inclusive K, and bounded reference-model energy with gravity potential and signed measured restart work. All active constraints must have positive compliance. Closed cases also require the angular-inclusive kinetic ceiling; passing is not a complete conservation proof" :
                (Config.ObserveAccepted ?
                    "Every completed World.Step endpoint is observed, but internal collision steps or soft iterations are not individually observed; complete accepted-update qualification is unmeasured" :
                    "Frame-only timing observation; intermediate world updates and internal integrations are unmeasured");
            Result["PublishedFrameQualityQualified"] = Result["QualityQualified"];
            Result["QualityQualified"] = FullInternalObservationCoverage ?
                Result["AcceptedQualityQualified"] : Json(false);
        } else {
            Result["AcceptedQualityQualified"] = nullptr;
            Result["AcceptedQualityScope"] = "Historical XPBD path has no accepted unified-world-update sampling";
        }
        const auto BudgetStatus = EnergyBudget.value("Status", std::string{});
        Result["PhysicalQualificationStatus"] = Config.Backend == "Jolt" ?
            (!Config.ObserveAccepted ? "UnqualifiedFrameOnlyObservation" :
                (!Result.value("AcceptedObservationCoverage", false) ?
                    "UnqualifiedInternalIntegrationCoverage" :
                    (!Result.value("ReferenceModelPreconditionsQualified", false) ?
                        "UnqualifiedReferenceModelPrecondition" :
                        (Result["QualityQualified"].get<bool>() ? "PassedAcceptedReferenceModelBounds" :
                            "FailedAcceptedReferenceModelBounds")))) :
            (BudgetStatus == "UnsupportedGeometry" ||
             BudgetStatus == "UnsupportedExperimentalProfile" || BudgetStatus == "UnsupportedBackend" ?
                BudgetStatus : (BudgetStatus == "UnmeasuredExternalWork" ?
                    "IncompleteExternalWork" :
                    (Result["QualityQualified"].get<bool>() ? "PassedNecessaryBounds" : "FailedNecessaryBounds")));
        Result["QualityGates"] = {{"MaxEdgeStrain", 0.1}, {"MaxPinDrift", 1.0e-4},
                                  {"MaxPenetration", 0.1}, {"RubberVolumeError", 0.15},
                                  {"RubberInvertedTetrahedra", 0},
                                  {"MinimumDirectionalRigidMomentumTransfer", 0.001},
                                  {"MinimumRigidVelocityDifference", 0.01}};
        Result["QualityQualificationScope"] = Config.Backend == "Jolt" ?
            (Result.value("AcceptedObservationCoverage", false) ?
                "Experimental complete internal integration endpoint contact, deformation, reciprocal rigid response, angular-inclusive closed-case kinetic ceiling, and gravity/restart-adjusted reference-model bound with positive-compliance active constraints" :
                "Incomplete internal integration endpoint observation; complete accepted-update physical qualification is unmeasured") :
            "Historical XPBD contact evidence, bounded deformation, reciprocal rigid response and necessary translational kinetic ceiling where external positive work is absent";
        Result["MomentumConservationStatus"] = "Incomplete: damping and external momentum impulses are not integrated";
        Result["EnergyConservationStatus"] = Config.Backend == "Jolt" ?
            (Result.value("AcceptedObservationCoverage", false) ?
                "Bounded reference-model gate measured at every internal integration endpoint when its preconditions hold; not a complete Jolt solver-internal energy balance" :
                "Internal integration endpoint energy incompletely observed; full reference-model gate unqualified") :
            "Incomplete: angular kinetic energy, strain potential and damping work are unmeasured";
        Result["PhysicsEquivalentTimingQualified"] = false;
        Result["ExperimentalProfile"] = {{"WorldUpdatesPerFrame", Config.WorldUpdates},
            {"CollisionStepsPerUpdate", Config.CollisionSteps},
            {"SoftIterationsPerCollisionStep", Config.SoftIterations},
            {"SelectedStretchCompliance", Config.StretchCompliance},
            {"ObserveAcceptedUpdates", Config.ObserveAccepted},
            {"HistoricalDefaults", Config.WorldUpdates == 1 && Config.SoftIterations == 24 &&
                Config.StretchCompliance == 0.0f}};
        Result["Resources"] = PhysicsEvaluation::ResourceDelta(Before, PhysicsEvaluation::CaptureResources());
        std::cout << Result.dump() << '\n';
        if (Config.Backend == "Jolt" && !Config.ObserveAccepted) return 0;
        return Result.value("QualityQualified", false) &&
            (Config.Backend != "Jolt" || Result.value("AcceptedQualityQualified", false)) ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cout << Json{{"Status", "Failed"}, {"Error", std::string("[Physics:Coupling] ") + Error.what()}}.dump() << '\n';
        return 2;
    }
}


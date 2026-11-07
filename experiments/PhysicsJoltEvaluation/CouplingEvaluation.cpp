#include "SoftFixtures.hpp"
#include "ExperimentSupport.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
    using PhysicsJoltEvaluation::JoltWorld;
    using Json = nlohmann::json;
    constexpr float DeltaTime = 1.0f / 60.0f;

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
        bool Finite = true;
        int MovableVertices = 0;
    };

    SoftObservation ObserveSoft(const JoltWorld &World, JPH::BodyID Id,
                                const JPH::SoftBodySharedSettings &Shared) {
        JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Id);
        if (!Lock.Succeeded()) throw std::runtime_error("[Physics:Coupling] Soft body lock failed");
        const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
        const auto &Vertices = Motion->GetVertices();
        SoftObservation Result;
        Result.MaxStretch = PhysicsJoltEvaluation::MaxEdgeStretch(Shared, *Motion);
        double VolumeRest = 0.0;
        double VolumeCurrent = 0.0;
        for (const auto &Volume : Shared.mVolumeConstraints) {
            const auto A = Vertices[Volume.mVertex[0]].mPosition;
            const auto B = Vertices[Volume.mVertex[1]].mPosition;
            const auto C = Vertices[Volume.mVertex[2]].mPosition;
            const auto D = Vertices[Volume.mVertex[3]].mPosition;
            VolumeRest += Volume.mSixRestVolume;
            VolumeCurrent += std::abs((B - A).Cross(C - A).Dot(D - A));
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
        JoltWorld World(BackgroundWorkers, EnableContact);
        const bool Horizontal = Config.Case == "SphereOnCloth";
        const bool IsRubber = Config.Case == "RubberPush";
        const JPH::RVec3 SoftOrigin = IsRubber ? JPH::RVec3(0.0, 0.0, 0.0) : JPH::RVec3(0.0, 0.0, 0.0);
        auto Shared = IsRubber ? PhysicsJoltEvaluation::Rubber() :
            PhysicsJoltEvaluation::Cloth(Horizontal ? 17 : 13, Horizontal ? 4.0f : 4.0f,
                                         Horizontal, Horizontal, 0.1f, 0.0f);
        if (IsRubber) for (auto &Vertex : Shared->mVertices) Vertex.mVelocity = JPH::Float3(4.0f, 0.0f, 0.0f);
        auto Soft = World.AddSoft(Shared, SoftOrigin, 24, IsRubber);
        World.World.SetGravity(Horizontal ? JPH::Vec3(0.0f, -9.81f, 0.0f) : JPH::Vec3::sZero());

        std::vector<JPH::BodyID> Rigids;
        if (IsRubber) {
            Rigids.push_back(World.AddRigid(new JPH::BoxShape(JPH::Vec3(0.4f, 0.4f, 0.4f)),
                                            JPH::RVec3(2.0, 0.0, 0.0), JPH::EMotionType::Dynamic, 1));
        } else if (Horizontal) {
            Rigids.push_back(World.AddRigid(new JPH::SphereShape(0.45f), JPH::RVec3(0.0, 2.0, 0.0),
                                            JPH::EMotionType::Dynamic, 1));
        } else {
            Rigids.push_back(World.AddRigid(new JPH::SphereShape(0.45f), JPH::RVec3(0.0, 0.0, -2.0),
                                            JPH::EMotionType::Dynamic, 1));
            World.World.GetBodyInterface().SetLinearVelocity(Rigids.back(), JPH::Vec3(0.0f, 0.0f, 5.0f));
            if (Config.Case == "Multiple") {
                Rigids.push_back(World.AddRigid(new JPH::SphereShape(0.45f), JPH::RVec3(1.0, -0.4, 2.0),
                                                JPH::EMotionType::Dynamic, 1));
                World.World.GetBodyInterface().SetLinearVelocity(Rigids.back(), JPH::Vec3(0.0f, 0.0f, -5.0f));
            }
        }

        Json Frames = Json::array();
        double MaxStretch = 0.0;
        double MaxVolumeError = 0.0;
        double MaxPinDrift = 0.0;
        bool Finite = true;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (Config.Case == "Repeated" && Frame > 0 && Frame % 45 == 0)
                World.World.GetBodyInterface().SetLinearVelocity(Rigids[0], JPH::Vec3(0.0f, 0.0f, 5.0f));
            const double StepMs = World.Step(DeltaTime, 1);
            auto SoftState = ObserveSoft(World, Soft, *Shared);
            MaxStretch = std::max(MaxStretch, SoftState.MaxStretch);
            MaxVolumeError = std::max(MaxVolumeError, SoftState.VolumeError);
            MaxPinDrift = std::max(MaxPinDrift, SoftState.MaxPinDrift);
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
            Frames.push_back({{"Frame", Frame}, {"StepMs", StepMs}, {"Rigids", std::move(RigidStates)},
                              {"SoftCenter", Vector(SoftState.Center)}, {"SoftMomentum", Vector(SoftState.Momentum)},
                              {"RigidMomentum", Vector(RigidMomentum)},
                              {"TotalMomentum", Vector(RigidMomentum + SoftState.Momentum)},
                              {"KineticEnergy", RigidKineticEnergy + SoftState.KineticEnergy},
                              {"MaxEdgeStretch", SoftState.MaxStretch}, {"VolumeError", SoftState.VolumeError},
                              {"PinDrift", SoftState.MaxPinDrift}});
        }
        return {{"Backend", "Jolt"}, {"Case", Config.Case}, {"ContactEnabled", EnableContact},
                {"Frames", std::move(Frames)}, {"Finite", Finite}, {"MaxEdgeStretch", MaxStretch},
                {"MaxVolumeError", MaxVolumeError}, {"MaxPinDrift", MaxPinDrift},
                {"BackgroundWorkers", BackgroundWorkers}, {"CallingThreads", 1},
                {"CollisionSteps", 1}, {"SolverIterations", 24}, {"DeltaTime", DeltaTime},
                {"SoftVertices", Shared->mVertices.size()}, {"SoftEdges", Shared->mEdgeConstraints.size()},
                {"SoftVolumes", Shared->mVolumeConstraints.size()}, {"AnchoredBoundary", !IsRubber}};
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        const auto Config = Parse(ArgumentCount, Arguments);
        PhysicsEvaluation::InstallPhysicsAllocators();
        const auto Before = PhysicsEvaluation::CaptureResources();
        Json Result;
        if (Config.Backend == "Jolt") {
            Result = RunJolt(Config, true);
            // A separate world and identical initial state provide the contact-disabled control.
            const auto Control = RunJolt(Config, false);
            Result["ContactDisabledControl"] = Control;
            Result["ReciprocalRigidResponse"] = false;
            const auto &ContactFrames = Result["Frames"];
            const auto &ControlFrames = Control["Frames"];
            double MaximumRigidVelocityDifference = 0.0;
            for (std::size_t Frame = 0; Frame < ContactFrames.size(); ++Frame)
                for (std::size_t Rigid = 0; Rigid < ContactFrames[Frame]["Rigids"].size(); ++Rigid) {
                    double Squared = 0.0;
                    for (int Axis = 0; Axis < 3; ++Axis) {
                        const double Delta = ContactFrames[Frame]["Rigids"][Rigid]["Velocity"][Axis].get<double>() -
                                             ControlFrames[Frame]["Rigids"][Rigid]["Velocity"][Axis].get<double>();
                        Squared += Delta * Delta;
                    }
                    MaximumRigidVelocityDifference = std::max(MaximumRigidVelocityDifference, std::sqrt(Squared));
                }
            Result["MaxRigidVelocityDifferenceFromNoContact"] = MaximumRigidVelocityDifference;
            Result["ReciprocalRigidResponse"] = MaximumRigidVelocityDifference > 0.01;
            Result["ResponseThreshold"] = 0.01;
        } else {
            Result = {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Status", "Unimplemented"},
                      {"Reason", "Neutral collider snapshots have no rigid identity, mass, velocity, or impulse result; a private feedback adapter is required"}};
        }
        Result["Resources"] = PhysicsEvaluation::ResourceDelta(Before, PhysicsEvaluation::CaptureResources());
        std::cout << Result.dump() << '\n';
        return Result.value("ReciprocalRigidResponse", false) || Config.Backend == "XPBD" ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cout << Json{{"Status", "Failed"}, {"Error", std::string("[Physics:Coupling] ") + Error.what()}}.dump() << '\n';
        return 2;
    }
}

#include "SoftFixtures.hpp"
#include "CouplingXpbdBackend.hpp"
#include "ExperimentSupport.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

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
            if (Config.Case == "Repeated" && Frame > 0 && Frame % 45 == 0) {
                World.World.GetBodyInterface().SetPositionAndRotation(Rigids[0], JPH::RVec3(0.0, 0.0, -2.0),
                    JPH::Quat::sIdentity(), JPH::EActivation::Activate);
                World.World.GetBodyInterface().SetLinearVelocity(Rigids[0], JPH::Vec3(0.0f, 0.0f, 5.0f));
            }
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
                {"SoftVolumes", Shared->mVolumeConstraints.size()}, {"AnchoredBoundary", !IsRubber},
                {"ControlledRestarts", Config.Case == "Repeated" ? (Config.Frames - 1) / 45 : 0}};
    }

    Json RunXpbd(const Options &Config, bool EnableFeedback) {
        if (Config.Case == "SphereOnCloth")
            return {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Status", "UnsupportedGeometry"},
                    {"Reason", "Current cloth topology is a fixed vertical X/Y plane; horizontal membrane comparison needs a private topology adapter"}};
        gargantuan::PhysicsWorld Rigid({.Gravity = glm::vec3(0.0f)});
        gargantuan::SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = static_cast<std::size_t>(Config.Workers);
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
            Description.Shape.Size = glm::vec3(0.9f);
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
        bool Finite = true;
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
            std::vector<gargantuan::PhysicsBodyState> RigidStates;
            for (auto Id : Rigids) {
                auto State = Rigid.GetBodyState(Id);
                if (!State) throw std::runtime_error("[Physics:Coupling] Box3D state missing");
                RigidStates.push_back(*State);
                SoftConfig.Colliders.push_back({State->Description.Shape, State->Transform});
            }
            const auto SoftStart = PhysicsJoltEvaluation::Clock::now();
            auto Result = Soft.Step(SoftConfig);
            const auto Reactions = FeedbackBackend->TakeFeedback();
            const double SoftMs = PhysicsJoltEvaluation::Milliseconds(SoftStart);
            const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                [&](const auto &State) { return State.Body == SoftId; });
            if (Found == Result.States.end() || !Found->Positions)
                throw std::runtime_error("[Physics:Coupling] XPBD state missing");
            const auto &Positions = *Found->Positions;
            glm::vec3 SoftCenter(0.0f);
            for (const auto &Position : Positions) {
                Finite &= std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
                SoftCenter += Position;
            }
            SoftCenter /= static_cast<float>(Positions.size());

            const auto FeedbackStart = PhysicsJoltEvaluation::Clock::now();
            Json Feedback = Json::array();
            for (std::size_t RigidIndex = 0; RigidIndex < Rigids.size(); ++RigidIndex) {
                const auto &State = RigidStates[RigidIndex];
                if (RigidIndex >= Reactions.size()) throw std::runtime_error("[Physics:Coupling] XPBD reaction mapping missing");
                const auto &Observed = Reactions[RigidIndex];
                if (Observed.ColliderIndex != RigidIndex)
                    throw std::runtime_error("[Physics:Coupling] XPBD collider ordinal mismatch");
                glm::vec3 Reaction = Observed.Impulse;
                const float Radius = 0.45f + Definition.Material.Thickness;
                for (const auto &Position : Positions) {
                    const float Distance = glm::length(Position - State.Transform.Position);
                    MaximumPenetration = std::max(MaximumPenetration,
                        std::max(0.0, static_cast<double>(Radius - Distance)));
                }
                const float MaximumImpulse = State.Mass * 8.0f;
                const float Magnitude = glm::length(Reaction);
                if (Magnitude > MaximumImpulse && MaximumImpulse > 0.0f)
                    Reaction *= MaximumImpulse / Magnitude;
                if (EnableFeedback && Observed.Contacts > 0 && glm::length(Reaction) > 0.0f &&
                    Rigid.IsBodyValid(Rigids[RigidIndex]) &&
                    !Rigid.ApplyImpulseAtPosition(Rigids[RigidIndex], Reaction, Observed.Position).Succeeded())
                    throw std::runtime_error("[Physics:Coupling] Box3D feedback impulse failed");
                MaximumFeedbackImpulse = std::max(MaximumFeedbackImpulse, static_cast<double>(glm::length(Reaction)));
                Feedback.push_back({{"ContactProjections", Observed.Contacts},
                                    {"ObservedReaction", {Observed.Impulse.x, Observed.Impulse.y, Observed.Impulse.z}},
                                    {"AppliedReaction", {Reaction.x, Reaction.y, Reaction.z}},
                                    {"Applied", EnableFeedback && Observed.Contacts > 0}});
            }
            const double FeedbackMs = PhysicsJoltEvaluation::Milliseconds(FeedbackStart);
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
                              {"FeedbackMs", FeedbackMs}, {"Rigids", std::move(RigidJson)},
                              {"Feedback", std::move(Feedback)},
                              {"SoftCenter", {SoftCenter.x, SoftCenter.y, SoftCenter.z}}});
        }
        Soft.Shutdown();
        return {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Status", "ExperimentalProjectionReaction"},
                {"FeedbackEnabled", EnableFeedback}, {"Frames", std::move(Frames)},
                {"Finite", Finite}, {"MaxPenetration", MaximumPenetration},
                {"MaxEstimatedFeedbackImpulse", MaximumFeedbackImpulse},
                {"DeltaTime", DeltaTime}, {"RigidSubsteps", 4},
                {"MaximumSoftWorkers", Config.Workers},
                {"MomentumConservationQualified", false},
                {"Limit", "The private XPBD copy observes each collision projection and transfers its opposite particle velocity change to Box3D at the next rigid safe point. This staggered reaction is clamped and is not a solved two-body constraint or conservation proof"}};
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
            Result = RunXpbd(Config, true);
            if (Result.value("Status", std::string{}) != "UnsupportedGeometry") {
                const auto Control = RunXpbd(Config, false);
                Result["FeedbackDisabledControl"] = Control;
                double MaximumRigidVelocityDifference = 0.0;
                for (std::size_t Frame = 0; Frame < Result["Frames"].size(); ++Frame)
                    for (std::size_t Rigid = 0; Rigid < Result["Frames"][Frame]["Rigids"].size(); ++Rigid) {
                        double Squared = 0.0;
                        for (int Axis = 0; Axis < 3; ++Axis) {
                            const double Delta = Result["Frames"][Frame]["Rigids"][Rigid]["Velocity"][Axis].get<double>() -
                                Control["Frames"][Frame]["Rigids"][Rigid]["Velocity"][Axis].get<double>();
                            Squared += Delta * Delta;
                        }
                        MaximumRigidVelocityDifference = std::max(MaximumRigidVelocityDifference, std::sqrt(Squared));
                    }
                Result["MaxRigidVelocityDifferenceFromNoFeedback"] = MaximumRigidVelocityDifference;
                Result["RigidResponseObserved"] = MaximumRigidVelocityDifference > 0.01;
            }
        }
        Result["Resources"] = PhysicsEvaluation::ResourceDelta(Before, PhysicsEvaluation::CaptureResources());
        std::cout << Result.dump() << '\n';
        return Result.value("ReciprocalRigidResponse", false) || Config.Backend == "XPBD" ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cout << Json{{"Status", "Failed"}, {"Error", std::string("[Physics:Coupling] ") + Error.what()}}.dump() << '\n';
        return 2;
    }
}

// Private, bounded diagnostic for the PR #19 XPBD coupling fixture.
#include "XpbdDiagnosticBackend.hpp"
#include "ExperimentSupport.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

namespace {
    using Json = nlohmann::json;
    constexpr float FrameDeltaTime = 1.0f / 60.0f;
    constexpr float SphereRadius = 0.45f;
    constexpr float SphereDensity = 0.7f;

    struct Options {
        int Frames = 36;
        int Substeps = 1;
        int Iterations = 8;
        bool Contact = true;
        bool Feedback = true;
        bool Clamp = true;
        bool Pinned = true;
        bool Dynamic = true;
        bool PhaseParticles = false;
        bool Interleaved = false;
        float StretchCompliance = 1.0e-6f;
        float BendCompliance = 1.0e-4f;
        float Damping = 0.04f;
        float Friction = 0.2f;
        float Speed = 5.0f;
        float StartZ = -2.0f;
    };

    Json Vector(glm::vec3 Value) { return Json::array({Value.x, Value.y, Value.z}); }

    bool Boolean(const std::string &Value) {
        if (Value == "1" || Value == "true") return true;
        if (Value == "0" || Value == "false") return false;
        throw std::invalid_argument("[Physics:XpbdDiagnostic] Boolean must be 0 or 1");
    }

    Options Parse(int Count, char **Arguments) {
        Options Result;
        for (int Index = 1; Index < Count; Index += 2) {
            if (Index + 1 >= Count) throw std::invalid_argument("[Physics:XpbdDiagnostic] Missing option value");
            const std::string Name = Arguments[Index];
            const std::string Value = Arguments[Index + 1];
            if (Name == "--frames") Result.Frames = std::stoi(Value);
            else if (Name == "--substeps") Result.Substeps = std::stoi(Value);
            else if (Name == "--iterations") Result.Iterations = std::stoi(Value);
            else if (Name == "--contact") Result.Contact = Boolean(Value);
            else if (Name == "--feedback") Result.Feedback = Boolean(Value);
            else if (Name == "--clamp") Result.Clamp = Boolean(Value);
            else if (Name == "--pinned") Result.Pinned = Boolean(Value);
            else if (Name == "--dynamic") Result.Dynamic = Boolean(Value);
            else if (Name == "--phase-particles") Result.PhaseParticles = Boolean(Value);
            else if (Name == "--interleaved") Result.Interleaved = Boolean(Value);
            else if (Name == "--stretch-compliance") Result.StretchCompliance = std::stof(Value);
            else if (Name == "--bend-compliance") Result.BendCompliance = std::stof(Value);
            else if (Name == "--damping") Result.Damping = std::stof(Value);
            else if (Name == "--friction") Result.Friction = std::stof(Value);
            else if (Name == "--speed") Result.Speed = std::stof(Value);
            else if (Name == "--start-z") Result.StartZ = std::stof(Value);
            else throw std::invalid_argument("[Physics:XpbdDiagnostic] Unknown option: " + Name);
        }
        if (Result.Frames < 1 || Result.Frames > 240 ||
            (Result.Substeps != 1 && Result.Substeps != 4) ||
            Result.Iterations < 1 || Result.Iterations > 64 ||
            !std::isfinite(Result.StretchCompliance) || Result.StretchCompliance < 0.0f ||
            !std::isfinite(Result.BendCompliance) || Result.BendCompliance < 0.0f ||
            !std::isfinite(Result.Damping) || Result.Damping < 0.0f || Result.Damping > 1.0f ||
            !std::isfinite(Result.Friction) || Result.Friction < 0.0f || Result.Friction > 1.0f ||
            !std::isfinite(Result.Speed) || !std::isfinite(Result.StartZ))
            throw std::invalid_argument("[Physics:XpbdDiagnostic] Option out of bounded range");
        return Result;
    }

    double RigidLinearEnergy(const gargantuan::PhysicsBodyState &State) {
        return 0.5 * static_cast<double>(State.Mass) *
            static_cast<double>(glm::dot(State.LinearVelocity, State.LinearVelocity));
    }

    double RigidAngularEnergy(const gargantuan::PhysicsBodyState &State) {
        // The fixture is a uniform sphere: its inertia is rotation invariant.
        const auto Squared = State.AngularVelocity * State.AngularVelocity;
        return 0.5 * static_cast<double>(glm::dot(State.LocalInertia, Squared));
    }

    Json RigidState(const gargantuan::PhysicsBodyState &State) {
        return {{"Position", Vector(State.Transform.Position)},
                {"LinearVelocity", Vector(State.LinearVelocity)},
                {"AngularVelocity", Vector(State.AngularVelocity)},
                {"Mass", State.Mass}, {"LocalInertia", Vector(State.LocalInertia)},
                {"LinearKineticEnergy", RigidLinearEnergy(State)},
                {"AngularKineticEnergy", RigidAngularEnergy(State)}};
    }

    Json Particle(const PhysicsJoltEvaluation::DiagnosticParticle &Node) {
        return {{"Position", Vector(Node.Position)}, {"Velocity", Vector(Node.Velocity)},
                {"Mass", Node.InverseMass > 0.0f ? 1.0f / Node.InverseMass : 0.0f},
                {"Pinned", Node.InverseMass == 0.0f}};
    }

    Json Diagnostic(const PhysicsJoltEvaluation::DiagnosticStep &Step, bool PhaseParticles) {
        Json Phases = Json::array();
        for (const auto &Phase : Step.Phases) {
            Json Entry = {{"Name", Phase.Name}, {"Iteration", Phase.Iteration},
                          {"SoftTranslationalKineticEnergy", Phase.TranslationalKineticEnergy},
                          {"SoftMomentum", Phase.Momentum},
                          {"PositionCorrectionSum", Phase.PositionCorrectionSum},
                          {"PositionCorrectionMax", Phase.PositionCorrectionMax}};
            if (PhaseParticles) {
                Entry["Particles"] = Json::array();
                for (const auto &Node : Phase.Particles) Entry["Particles"].push_back(Particle(Node));
            }
            Phases.push_back(std::move(Entry));
        }
        Json Contacts = Json::array();
        for (const auto &Contact : Step.Contacts)
            Contacts.push_back({{"Iteration", Contact.Iteration}, {"Particle", Contact.ParticleIndex},
                                {"Collider", Contact.ColliderIndex}, {"Normal", Vector(Contact.Normal)},
                                {"PositionBefore", Vector(Contact.PositionBefore)},
                                {"PositionAfter", Vector(Contact.PositionAfter)},
                                {"ProjectionDepth", Contact.PenetrationDepth},
                                {"EstimatedReaction", Vector(Contact.Reaction)},
                                {"FrictionApplied", Contact.FrictionApplied}});
        Json Particles = Json::array();
        if (!Step.Phases.empty())
            for (const auto &Node : Step.Phases.back().Particles) Particles.push_back(Particle(Node));
        return {{"VertexRevision", Step.VertexRevision}, {"Iterations", Step.Iterations},
                {"Phases", std::move(Phases)}, {"Contacts", std::move(Contacts)},
                {"Particles", std::move(Particles)}};
    }

    gargantuan::PhysicsBodyState ReadRigid(gargantuan::PhysicsWorld &World,
                                           gargantuan::PhysicsBodyId Body) {
        const auto State = World.GetBodyState(Body);
        if (!State) throw std::runtime_error("[Physics:XpbdDiagnostic] Rigid state unavailable");
        return *State;
    }

    void Run(const Options &Config) {
        PhysicsEvaluation::InstallPhysicsAllocators();
        gargantuan::PhysicsWorld Rigid({.Gravity = glm::vec3(0.0f)});
        gargantuan::SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = 1;
        auto Backend = PhysicsJoltEvaluation::CreateDiagnosticSoftBodyBackend(
            Limits, {.Iterations = Config.Iterations});
        auto *DiagnosticBackend = Backend.get();
        gargantuan::SoftBodyWorld Soft(std::move(Backend));

        gargantuan::SoftBodyDefinition Definition;
        Definition.Kind = gargantuan::SoftBodyKind::Cloth;
        Definition.Position = glm::vec3(0.0f);
        Definition.Size = glm::vec3(4.0f, 4.0f, 0.5f);
        Definition.ResolutionX = Definition.ResolutionY = 13;
        Definition.Quality = gargantuan::SoftBodyQuality::High;
        Definition.Material.StretchCompliance = Config.StretchCompliance;
        Definition.Material.BendCompliance = Config.BendCompliance;
        Definition.Material.Damping = Config.Damping;
        Definition.Material.Friction = Config.Friction;
        if (Config.Pinned)
            for (std::uint32_t X = 0; X < 13; ++X)
                Definition.Attachments.push_back({X, {-2.0f + 4.0f * X / 12.0f, 2.0f, 0.0f}});
        const auto SoftId = Soft.CreateBody(Definition);
        if (!SoftId.IsValid()) throw std::runtime_error("[Physics:XpbdDiagnostic] Soft admission failed");

        gargantuan::PhysicsBodyDesc RigidDescription;
        RigidDescription.Shape.Kind = gargantuan::PhysicsShapeKind::Ball;
        RigidDescription.Shape.Size = glm::vec3(2.0f * SphereRadius);
        RigidDescription.Density = SphereDensity;
        RigidDescription.Transform.Position = {0.0f, 0.0f, Config.StartZ};
        RigidDescription.Anchored = !Config.Dynamic;
        RigidDescription.SleepEnabled = false;
        const auto RigidId = Rigid.CreateBody(RigidDescription);
        if (!RigidId.IsValid()) throw std::runtime_error("[Physics:XpbdDiagnostic] Rigid admission failed");
        if (Config.Dynamic && !Rigid.SetLinearVelocity(RigidId, {0.0f, 0.0f, Config.Speed}).Succeeded())
            throw std::runtime_error("[Physics:XpbdDiagnostic] Initial rigid velocity failed");
        const auto InitialRigid = ReadRigid(Rigid, RigidId);
        const double InitialKineticEnergy = RigidLinearEnergy(InitialRigid) + RigidAngularEnergy(InitialRigid);

        std::cout << Json({{"Record", "Config"}, {"Frames", Config.Frames},
                           {"Substeps", Config.Substeps}, {"Interleaved", Config.Interleaved},
                           {"Iterations", Config.Iterations}, {"Contact", Config.Contact},
                           {"Feedback", Config.Feedback}, {"Clamp", Config.Clamp},
                           {"Pinned", Config.Pinned}, {"Dynamic", Config.Dynamic},
                           {"StretchCompliance", Config.StretchCompliance},
                           {"BendCompliance", Config.BendCompliance}, {"Damping", Config.Damping},
                           {"Friction", Config.Friction}, {"ParticleMass", Definition.Material.ParticleMass},
                           {"Thickness", Definition.Material.Thickness},
                           {"RigidInitial", RigidState(InitialRigid)},
                           {"InitialKineticEnergy", InitialKineticEnergy},
                           {"PhaseParticles", Config.PhaseParticles},
                           {"EnergyNote", "Kinetic energy only. Phase energy uses reconstructed candidate velocity after position solves. Constraint potential, pin reaction work, and damping/friction work are not integrated."}}).dump() << '\n';

        int FirstBudgetExceededFrame = -1;
        double MaximumKineticEnergy = InitialKineticEnergy;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            Json SubstepRecords = Json::array();
            for (int Substep = 0; Substep < (Config.Interleaved ? Config.Substeps : 1); ++Substep) {
                const float StepTime = Config.Interleaved ? FrameDeltaTime / Config.Substeps : FrameDeltaTime;
                const auto BeforeRigid = ReadRigid(Rigid, RigidId);
                (void)Rigid.Step({.DeltaTime = StepTime,
                                  .SubStepCount = Config.Interleaved ? 1 : Config.Substeps});
                const auto AfterRigid = ReadRigid(Rigid, RigidId);

                gargantuan::SoftBodyStepConfig SoftConfig;
                SoftConfig.DeltaTime = StepTime;
                SoftConfig.Gravity = glm::vec3(0.0f);
                SoftConfig.ExecutionMode = gargantuan::SoftBodyExecutionMode::SynchronousReference;
                if (Config.Contact)
                    SoftConfig.Colliders.push_back({AfterRigid.Description.Shape, AfterRigid.Transform});
                const auto Result = Soft.Step(SoftConfig);
                if (Result.CollidersTruncated || Result.BodiesTruncated || Result.VerticesTruncated)
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Soft step truncated");
                const auto State = std::find_if(Result.States.begin(), Result.States.end(),
                    [&](const auto &Candidate) { return Candidate.Body == SoftId; });
                if (State == Result.States.end())
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Soft state missing");
                const auto Trace = DiagnosticBackend->ReadDiagnosticStep(SoftId, State->VertexRevision);
                if (!Trace || Trace->Phases.empty())
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Diagnostic trace missing");
                const auto Reactions = DiagnosticBackend->TakeFeedback();
                if (Reactions.size() != (Config.Contact ? 1u : 0u))
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Feedback collider count mismatch");
                const PhysicsJoltEvaluation::FeedbackImpulse Empty;
                const auto &Observed = Config.Contact ? Reactions.front() : Empty;
                const glm::vec3 RawAngular = Observed.AngularImpulseOrigin -
                    glm::cross(AfterRigid.Transform.Position, Observed.Impulse);
                const float MaximumLinearImpulse = AfterRigid.Mass * 8.0f;
                const float MaximumAngularImpulse = 0.4f * AfterRigid.Mass * SphereRadius * SphereRadius * 8.0f;
                float Scale = 1.0f;
                if (Config.Clamp) {
                    const float LinearMagnitude = glm::length(Observed.Impulse);
                    const float AngularMagnitude = glm::length(RawAngular);
                    if (LinearMagnitude > MaximumLinearImpulse && MaximumLinearImpulse > 0.0f)
                        Scale = std::min(Scale, MaximumLinearImpulse / LinearMagnitude);
                    if (AngularMagnitude > MaximumAngularImpulse && MaximumAngularImpulse > 0.0f)
                        Scale = std::min(Scale, MaximumAngularImpulse / AngularMagnitude);
                }
                const glm::vec3 AppliedLinear = Config.Feedback && Config.Dynamic ? Scale * Observed.Impulse : glm::vec3{};
                const glm::vec3 AppliedAngular = Config.Feedback && Config.Dynamic ? Scale * RawAngular : glm::vec3{};
                if (Observed.Contacts > 0 && Config.Feedback && Config.Dynamic &&
                    (!Rigid.ApplyLinearImpulse(RigidId, AppliedLinear).Succeeded() ||
                     !Rigid.ApplyAngularImpulse(RigidId, AppliedAngular).Succeeded()))
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Feedback application failed");
                const auto AfterFeedback = ReadRigid(Rigid, RigidId);
                const double SoftEnergy = Trace->Phases.back().TranslationalKineticEnergy;
                const double TotalEnergy = RigidLinearEnergy(AfterFeedback) +
                    RigidAngularEnergy(AfterFeedback) + SoftEnergy;
                MaximumKineticEnergy = std::max(MaximumKineticEnergy, TotalEnergy);
                if (FirstBudgetExceededFrame < 0 && TotalEnergy > 1.05 * InitialKineticEnergy + 1.0e-5)
                    FirstBudgetExceededFrame = Frame;
                SubstepRecords.push_back({{"Substep", Substep}, {"DeltaTime", StepTime},
                                          {"RigidBeforeStep", RigidState(BeforeRigid)},
                                          {"RigidAfterStep", RigidState(AfterRigid)},
                                          {"RigidAfterFeedback", RigidState(AfterFeedback)},
                                          {"ContactProjections", Observed.Contacts},
                                          {"RawLinearReaction", Vector(Observed.Impulse)},
                                          {"RawAngularReaction", Vector(RawAngular)},
                                          {"ClampScale", Scale},
                                          {"AppliedLinearReaction", Vector(AppliedLinear)},
                                          {"AppliedAngularReaction", Vector(AppliedAngular)},
                                          {"SoftDiagnostic", Diagnostic(*Trace, Config.PhaseParticles)},
                                          {"TotalKineticEnergy", TotalEnergy}});
            }
            std::cout << Json({{"Record", "Frame"}, {"Frame", Frame},
                               {"Substeps", std::move(SubstepRecords)},
                               {"FirstBudgetExceededFrame", FirstBudgetExceededFrame},
                               {"MaximumKineticEnergy", MaximumKineticEnergy}}).dump() << '\n';
        }
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        Run(Parse(ArgumentCount, Arguments));
        return 0;
    } catch (const std::exception &Error) {
        std::cout << Json({{"Record", "Error"}, {"Message", Error.what()}}).dump() << '\n';
        return 2;
    }
}

// Private, bounded diagnostic for the PR #19 XPBD coupling fixture.
#include "XpbdDiagnosticBackend.hpp"
#include "ExperimentSupport.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <algorithm>
#include <chrono>
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

    enum class ColliderPathMode { Sampled, Linear };

    struct Options {
        int Frames = 36;
        int Substeps = 1;
        int SoftSubsteps = 1;
        int Iterations = 8;
        int Workers = 1;
        bool Diagnostics = true;
        bool Contact = true;
        bool Feedback = true;
        bool Clamp = true;
        bool Pinned = true;
        bool Dynamic = true;
        bool PhaseParticles = false;
        bool Interleaved = false;
        ColliderPathMode ColliderPath = ColliderPathMode::Sampled;
        float StretchCompliance = 1.0e-6f;
        float BendCompliance = 1.0e-4f;
        float Damping = 0.04f;
        float Friction = 0.2f;
        float Speed = 5.0f;
        float StartZ = -2.0f;
    };

    Json Vector(glm::vec3 Value) { return Json::array({Value.x, Value.y, Value.z}); }

    double Milliseconds(std::chrono::steady_clock::time_point Start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
    }

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
            else if (Name == "--soft-substeps") Result.SoftSubsteps = std::stoi(Value);
            else if (Name == "--iterations") Result.Iterations = std::stoi(Value);
            else if (Name == "--workers") Result.Workers = std::stoi(Value);
            else if (Name == "--diagnostics") Result.Diagnostics = Boolean(Value);
            else if (Name == "--contact") Result.Contact = Boolean(Value);
            else if (Name == "--feedback") Result.Feedback = Boolean(Value);
            else if (Name == "--clamp") Result.Clamp = Boolean(Value);
            else if (Name == "--pinned") Result.Pinned = Boolean(Value);
            else if (Name == "--dynamic") Result.Dynamic = Boolean(Value);
            else if (Name == "--phase-particles") Result.PhaseParticles = Boolean(Value);
            else if (Name == "--interleaved") Result.Interleaved = Boolean(Value);
            else if (Name == "--collider-path") {
                if (Value == "sampled") Result.ColliderPath = ColliderPathMode::Sampled;
                else if (Value == "linear") Result.ColliderPath = ColliderPathMode::Linear;
                else throw std::invalid_argument("[Physics:XpbdDiagnostic] Collider path must be sampled or linear");
            }
            else if (Name == "--stretch-compliance") Result.StretchCompliance = std::stof(Value);
            else if (Name == "--bend-compliance") Result.BendCompliance = std::stof(Value);
            else if (Name == "--damping") Result.Damping = std::stof(Value);
            else if (Name == "--friction") Result.Friction = std::stof(Value);
            else if (Name == "--speed") Result.Speed = std::stof(Value);
            else if (Name == "--start-z") Result.StartZ = std::stof(Value);
            else throw std::invalid_argument("[Physics:XpbdDiagnostic] Unknown option: " + Name);
        }
        if (Result.Frames < 1 || Result.Frames > 240 ||
            Result.Substeps < 1 || Result.Substeps > 96 ||
            Result.SoftSubsteps < 1 || Result.SoftSubsteps > 24 ||
            Result.Substeps * Result.SoftSubsteps > 96 ||
            (!Result.Interleaved && Result.Substeps != 1 && Result.Substeps != 4) ||
            (!Result.Interleaved && Result.SoftSubsteps != 1) ||
            (Result.ColliderPath == ColliderPathMode::Linear && !Result.Interleaved) ||
            Result.Iterations < 1 || Result.Iterations > 64 ||
            (Result.Workers != 1 && Result.Workers != 2 && Result.Workers != 8) ||
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

    struct QualityObservation {
        double MaxEdgeStretch = 0.0;
        double MaxPinDrift = 0.0;
        double MaxParticlePenetration = 0.0;
        double CompliantEdgeElasticEstimate = 0.0;
        std::size_t OmittedHardEdgeCount = 0;
        bool Finite = true;
    };

    struct RunSummary {
        int Frames = 0;
        int SoftSubsteps = 0;
        int FirstBudgetExceededFrame = -1;
        double InitialKineticEnergy = 0.0;
        double MaximumKineticEnergy = 0.0;
        double MaxEdgeStretch = 0.0;
        double MaxPinDrift = 0.0;
        double MaxParticlePenetration = 0.0;
        double MaximumCompliantEdgeElasticEstimate = 0.0;
        double MaximumKineticPlusCompliantEdgeEstimate = 0.0;
        std::size_t MaximumOmittedHardEdgeCount = 0;
        std::size_t TotalContactProjections = 0;
        bool Finite = true;
        double TotalPhysicalPhasesMs = 0.0;
        double TotalSubstepPipelineMs = 0.0;
        double TotalObservationReadMs = 0.0;
        double TotalCouplingPipelineWithoutObservationsMs = 0.0;
    };

    QualityObservation ObserveQuality(const std::vector<glm::vec3> &Positions,
                                      const gargantuan::SoftBodyDefinition &Definition,
                                      glm::vec3 RigidPosition) {
        QualityObservation Result;
        const std::size_t N = Definition.ResolutionX;
        if (Positions.size() != N * Definition.ResolutionY)
            throw std::runtime_error("[Physics:XpbdDiagnostic] Unexpected accepted cloth vertex count");
        const float RestX = Definition.Size.x / static_cast<float>(N - 1);
        const float RestY = Definition.Size.y / static_cast<float>(Definition.ResolutionY - 1);
        const float RestDiagonal = std::sqrt(RestX * RestX + RestY * RestY);
        const auto CheckEdge = [&](std::size_t A, std::size_t B, float Length,
                                   float Compliance, bool Structural) {
            const double CurrentLength = static_cast<double>(glm::length(Positions[B] - Positions[A]));
            const double Error = CurrentLength - static_cast<double>(Length);
            Result.Finite &= std::isfinite(CurrentLength) && std::isfinite(Error);
            if (Structural)
                Result.MaxEdgeStretch = std::max(Result.MaxEdgeStretch,
                    std::abs(CurrentLength / static_cast<double>(Length) - 1.0));
            if (Compliance > 0.0f)
                Result.CompliantEdgeElasticEstimate += Error * Error / (2.0 * static_cast<double>(Compliance));
            else
                ++Result.OmittedHardEdgeCount;
        };
        for (std::size_t Y = 0; Y < Definition.ResolutionY; ++Y)
            for (std::size_t X = 0; X < N; ++X) {
                const auto Index = Y * N + X;
                if (X + 1 < N) CheckEdge(Index, Index + 1, RestX,
                    Definition.Material.StretchCompliance, true);
                if (Y + 1 < Definition.ResolutionY) CheckEdge(Index, Index + N, RestY,
                    Definition.Material.StretchCompliance, true);
                if (X + 1 < N && Y + 1 < Definition.ResolutionY) {
                    CheckEdge(Index, Index + N + 1, RestDiagonal,
                        Definition.Material.StretchCompliance, true);
                    CheckEdge(Index + 1, Index + N, RestDiagonal,
                        Definition.Material.StretchCompliance, true);
                }
                if (X + 2 < N) CheckEdge(Index, Index + 2, 2.0f * RestX,
                    Definition.Material.BendCompliance, false);
                if (Y + 2 < Definition.ResolutionY) CheckEdge(Index, Index + 2 * N, 2.0f * RestY,
                    Definition.Material.BendCompliance, false);
            }
        Result.Finite &= std::isfinite(Result.CompliantEdgeElasticEstimate);
        for (const auto &Attachment : Definition.Attachments) {
            const double Drift = static_cast<double>(glm::length(Positions[Attachment.Vertex] - Attachment.Position));
            Result.Finite &= std::isfinite(Drift);
            Result.MaxPinDrift = std::max(Result.MaxPinDrift, Drift);
        }
        const float ContactRadius = SphereRadius + Definition.Material.Thickness;
        for (const auto &Position : Positions) {
            Result.Finite &= std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
            const double Distance = static_cast<double>(glm::length(Position - RigidPosition));
            Result.Finite &= std::isfinite(Distance);
            Result.MaxParticlePenetration = std::max(Result.MaxParticlePenetration,
                std::max(0.0, static_cast<double>(ContactRadius) - Distance));
        }
        return Result;
    }

    RunSummary Run(const Options &Config) {
        gargantuan::PhysicsWorld Rigid({.Gravity = glm::vec3(0.0f)});
        gargantuan::SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = static_cast<std::size_t>(std::max(1, Config.Workers - 1));
        auto Backend = PhysicsJoltEvaluation::CreateDiagnosticSoftBodyBackend(
            Limits, {.Iterations = Config.Iterations, .Diagnostics = Config.Diagnostics});
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

        const int OuterSteps = Config.Interleaved ? Config.Substeps : 1;
        const int SoftStepsPerOuter = Config.Interleaved ? Config.SoftSubsteps : 1;
        const char *ColliderPathName = Config.ColliderPath == ColliderPathMode::Linear ? "linear" : "sampled";
        std::cout << Json({{"Record", "Config"}, {"Frames", Config.Frames},
                           {"Substeps", Config.Substeps}, {"Interleaved", Config.Interleaved},
                           {"ColliderPath", ColliderPathName},
                           {"OuterRigidStepsPerFrame", OuterSteps},
                           {"SoftStepsPerOuterRigidStep", SoftStepsPerOuter},
                           {"SoftIntegrationsPerFrame", OuterSteps * SoftStepsPerOuter},
                           {"RigidOuterDeltaTime", FrameDeltaTime / OuterSteps},
                           {"SoftStepDeltaTime", FrameDeltaTime / (OuterSteps * SoftStepsPerOuter)},
                           {"SoftDampingMultiplierPerStep", 1.0f - Config.Damping},
                           {"NominalSoftDampingRetentionPerFrame",
                            std::pow(1.0 - static_cast<double>(Config.Damping), OuterSteps * SoftStepsPerOuter)},
                           {"ColliderTransformHold", Config.ColliderPath == ColliderPathMode::Linear ?
                            "Sphere collider translation is sampled linearly from the outer rigid step's begin/end poses at each inner soft step. The actual rigid body advances once per outer step; feedback changes velocity after each inner soft step and position advances at the next outer step. This is a staggered approximation, not a simultaneous solve." :
                            "One sampled rigid transform per outer step; held fixed during inner soft steps. Feedback changes rigid velocity after each inner soft step, while rigid position advances at the next outer step."},
                           {"PenetrationReference", "Collider position sampled for each accepted soft step"},
                           {"Iterations", Config.Iterations}, {"Workers", Config.Workers},
                           {"Diagnostics", Config.Diagnostics}, {"Contact", Config.Contact},
                           {"Feedback", Config.Feedback}, {"Clamp", Config.Clamp},
                           {"Pinned", Config.Pinned}, {"Dynamic", Config.Dynamic},
                           {"StretchCompliance", Config.StretchCompliance},
                           {"BendCompliance", Config.BendCompliance}, {"Damping", Config.Damping},
                           {"Friction", Config.Friction}, {"ParticleMass", Definition.Material.ParticleMass},
                           {"Thickness", Definition.Material.Thickness},
                           {"RigidInitial", RigidState(InitialRigid)},
                           {"InitialKineticEnergy", InitialKineticEnergy},
                           {"PhaseParticles", Config.PhaseParticles},
                           {"EnergyNote", "Kinetic energy and a separate approximate compliant-edge estimate. Phase energy uses reconstructed candidate velocity after position solves. Zero-compliance edges, pin/contact reactions, and damping/friction work are not integrated."},
                           {"ElasticEstimateScope", "Sum (current edge length - rest length)^2 / (2*compliance) over positive-compliance structural, diagonal, and two-hop bend distance edges. Hard edges are omitted and counted. This is a spring analogy, not stored XPBD solver energy or a complete mechanical-energy balance."}}).dump() << '\n';

        int FirstBudgetExceededFrame = -1;
        double MaximumKineticEnergy = InitialKineticEnergy;
        double MaximumEdgeStretch = 0.0;
        double MaximumPinDrift = 0.0;
        double MaximumParticlePenetration = 0.0;
        double MaximumCompliantEdgeElasticEstimate = 0.0;
        double MaximumKineticPlusCompliantEdgeEstimate = 0.0;
        std::size_t MaximumOmittedHardEdgeCount = 0;
        double TotalPhysicalPhasesMs = 0.0;
        double TotalSubstepPipelineMs = 0.0;
        double TotalObservationReadMs = 0.0;
        std::size_t TotalContactProjections = 0;
        bool Finite = true;
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            Json SubstepRecords = Json::array();
            gargantuan::PhysicsBodyState ColliderState;
            glm::vec3 OuterRigidBeginPosition{};
            for (int Substep = 0; Substep < OuterSteps * SoftStepsPerOuter; ++Substep) {
                const int OuterSubstep = Substep / SoftStepsPerOuter;
                const int SoftSubstep = Substep % SoftStepsPerOuter;
                const float OuterTime = FrameDeltaTime / OuterSteps;
                const float StepTime = OuterTime / SoftStepsPerOuter;
                const auto PipelineStart = std::chrono::steady_clock::now();
                const auto BeforeRigid = ReadRigid(Rigid, RigidId);
                double RigidMs = 0.0;
                if (SoftSubstep == 0) {
                    OuterRigidBeginPosition = BeforeRigid.Transform.Position;
                    const auto RigidStart = std::chrono::steady_clock::now();
                    (void)Rigid.Step({.DeltaTime = OuterTime,
                                      .SubStepCount = Config.Interleaved ? 1 : Config.Substeps});
                    RigidMs = Milliseconds(RigidStart);
                    ColliderState = ReadRigid(Rigid, RigidId);
                }
                const auto &AfterRigid = ColliderState;
                auto ColliderTransform = AfterRigid.Transform;
                if (Config.ColliderPath == ColliderPathMode::Linear) {
                    const float Fraction = static_cast<float>(SoftSubstep + 1) / SoftStepsPerOuter;
                    ColliderTransform.Position = OuterRigidBeginPosition +
                        Fraction * (AfterRigid.Transform.Position - OuterRigidBeginPosition);
                }
                const auto ColliderSamplePosition = ColliderTransform.Position;

                gargantuan::SoftBodyStepConfig SoftConfig;
                SoftConfig.DeltaTime = StepTime;
                SoftConfig.Gravity = glm::vec3(0.0f);
                SoftConfig.ExecutionMode = Config.Workers == 1 ?
                    gargantuan::SoftBodyExecutionMode::SynchronousReference :
                    gargantuan::SoftBodyExecutionMode::Jobified;
                if (Config.Contact)
                    SoftConfig.Colliders.push_back({AfterRigid.Description.Shape, ColliderTransform});
                const auto SoftStart = std::chrono::steady_clock::now();
                const auto Result = Soft.Step(SoftConfig);
                const double SoftMs = Milliseconds(SoftStart);
                const auto ObservationReadStart = std::chrono::steady_clock::now();
                if (Result.CollidersTruncated || Result.BodiesTruncated || Result.VerticesTruncated)
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Soft step truncated");
                const auto State = std::find_if(Result.States.begin(), Result.States.end(),
                    [&](const auto &Candidate) { return Candidate.Body == SoftId; });
                if (State == Result.States.end() || !State->Positions)
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Soft state missing");
                const auto Dynamics = DiagnosticBackend->ReadAcceptedDynamics(SoftId, State->VertexRevision);
                if (!Dynamics || !Dynamics->Finite)
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Accepted dynamics unavailable");
                const auto Trace = Config.Diagnostics ?
                    DiagnosticBackend->ReadDiagnosticStep(SoftId, State->VertexRevision) :
                    std::optional<PhysicsJoltEvaluation::DiagnosticStep>{};
                if (Config.Diagnostics && (!Trace || Trace->Phases.empty()))
                    throw std::runtime_error("[Physics:XpbdDiagnostic] Diagnostic trace missing");
                const double ObservationReadMs = Milliseconds(ObservationReadStart);
                const auto FeedbackStart = std::chrono::steady_clock::now();
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
                const double FeedbackMs = Milliseconds(FeedbackStart);
                const double SubstepPipelineMs = Milliseconds(PipelineStart);
                const double CouplingPipelineWithoutObservationsMs =
                    SubstepPipelineMs - ObservationReadMs;
                const double PhysicalPhasesMs = RigidMs + SoftMs + FeedbackMs;
                // Quality is intentionally outside the solve/pipeline timing windows.
                const auto Quality = ObserveQuality(*State->Positions, Definition, ColliderSamplePosition);
                MaximumEdgeStretch = std::max(MaximumEdgeStretch, Quality.MaxEdgeStretch);
                MaximumPinDrift = std::max(MaximumPinDrift, Quality.MaxPinDrift);
                MaximumParticlePenetration = std::max(MaximumParticlePenetration, Quality.MaxParticlePenetration);
                MaximumCompliantEdgeElasticEstimate = std::max(MaximumCompliantEdgeElasticEstimate,
                    Quality.CompliantEdgeElasticEstimate);
                MaximumOmittedHardEdgeCount = std::max(MaximumOmittedHardEdgeCount,
                    Quality.OmittedHardEdgeCount);
                TotalPhysicalPhasesMs += PhysicalPhasesMs;
                TotalSubstepPipelineMs += SubstepPipelineMs;
                TotalObservationReadMs += ObservationReadMs;
                TotalContactProjections += Observed.Contacts;
                const double SoftEnergy = Dynamics->TranslationalKineticEnergy;
                const double TotalEnergy = RigidLinearEnergy(AfterFeedback) +
                    RigidAngularEnergy(AfterFeedback) + SoftEnergy;
                const double KineticPlusCompliantEdgeEstimate = TotalEnergy +
                    Quality.CompliantEdgeElasticEstimate;
                MaximumKineticPlusCompliantEdgeEstimate = std::max(
                    MaximumKineticPlusCompliantEdgeEstimate, KineticPlusCompliantEdgeEstimate);
                Finite &= Quality.Finite && std::isfinite(TotalEnergy) &&
                    std::isfinite(KineticPlusCompliantEdgeEstimate) &&
                    std::isfinite(AfterFeedback.Transform.Position.x) &&
                    std::isfinite(AfterFeedback.Transform.Position.y) &&
                    std::isfinite(AfterFeedback.Transform.Position.z) &&
                    std::isfinite(AfterFeedback.LinearVelocity.x) &&
                    std::isfinite(AfterFeedback.LinearVelocity.y) &&
                    std::isfinite(AfterFeedback.LinearVelocity.z) &&
                    std::isfinite(AfterFeedback.AngularVelocity.x) &&
                    std::isfinite(AfterFeedback.AngularVelocity.y) &&
                    std::isfinite(AfterFeedback.AngularVelocity.z);
                MaximumKineticEnergy = std::max(MaximumKineticEnergy, TotalEnergy);
                if (FirstBudgetExceededFrame < 0 && TotalEnergy > 1.05 * InitialKineticEnergy + 1.0e-5)
                    FirstBudgetExceededFrame = Frame;
                SubstepRecords.push_back({{"Substep", Substep},
                                          {"OuterSubstep", OuterSubstep},
                                          {"SoftSubstep", SoftSubstep},
                                          {"ColliderPath", ColliderPathName},
                                          {"OuterRigidBeginPosition", Vector(OuterRigidBeginPosition)},
                                          {"ColliderSamplePosition", Vector(ColliderSamplePosition)},
                                          {"RigidAdvanced", SoftSubstep == 0},
                                          {"OuterDeltaTime", OuterTime},
                                          {"DeltaTime", StepTime},
                                          {"RigidMs", RigidMs}, {"SoftMs", SoftMs},
                                          {"FeedbackMs", FeedbackMs},
                                          {"PhysicalPhasesMs", PhysicalPhasesMs},
                                          {"SubstepPipelineMs", SubstepPipelineMs},
                                          {"ObservationReadMs", ObservationReadMs},
                                          {"CouplingPipelineWithoutObservationsMs",
                                           CouplingPipelineWithoutObservationsMs},
                                          {"SoftWorkerCount", Result.Profile.WorkerCount},
                                          {"RigidBeforeStep", RigidState(BeforeRigid)},
                                          {"RigidAfterStep", RigidState(AfterRigid)},
                                          {"RigidAfterFeedback", RigidState(AfterFeedback)},
                                          {"ContactProjections", Observed.Contacts},
                                          {"RawLinearReaction", Vector(Observed.Impulse)},
                                          {"RawAngularReaction", Vector(RawAngular)},
                                          {"ClampScale", Scale},
                                          {"AppliedLinearReaction", Vector(AppliedLinear)},
                                          {"AppliedAngularReaction", Vector(AppliedAngular)},
                                          {"SoftKineticEnergy", SoftEnergy},
                                          {"SoftMomentum", Dynamics->Momentum},
                                          {"SoftMovableMass", Dynamics->MovableMass},
                                          {"SoftDiagnostic", Trace ? Json(Diagnostic(*Trace, Config.PhaseParticles)) : Json(nullptr)},
                                          {"MaxEdgeStretch", Quality.MaxEdgeStretch},
                                          {"MaxPinDrift", Quality.MaxPinDrift},
                                          {"MaxParticlePenetration", Quality.MaxParticlePenetration},
                                          {"CompliantEdgeElasticEstimate", Quality.CompliantEdgeElasticEstimate},
                                          {"OmittedHardEdgeCount", Quality.OmittedHardEdgeCount},
                                          {"KineticPlusCompliantEdgeEstimate", KineticPlusCompliantEdgeEstimate},
                                          {"Finite", Quality.Finite},
                                          {"TotalKineticEnergy", TotalEnergy}});
            }
            std::cout << Json({{"Record", "Frame"}, {"Frame", Frame},
                               {"Substeps", std::move(SubstepRecords)},
                               {"FirstBudgetExceededFrame", FirstBudgetExceededFrame},
                               {"MaximumKineticEnergy", MaximumKineticEnergy}}).dump() << '\n';
        }
        return {Config.Frames, Config.Frames * OuterSteps * SoftStepsPerOuter,
                FirstBudgetExceededFrame, InitialKineticEnergy, MaximumKineticEnergy,
                MaximumEdgeStretch, MaximumPinDrift, MaximumParticlePenetration,
                MaximumCompliantEdgeElasticEstimate, MaximumKineticPlusCompliantEdgeEstimate,
                MaximumOmittedHardEdgeCount,
                TotalContactProjections, Finite, TotalPhysicalPhasesMs, TotalSubstepPipelineMs,
                TotalObservationReadMs, TotalSubstepPipelineMs - TotalObservationReadMs};
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        const auto Config = Parse(ArgumentCount, Arguments);
        PhysicsEvaluation::InstallPhysicsAllocators();
        const auto Before = PhysicsEvaluation::CaptureResources();
        const auto Result = Run(Config);
        const auto After = PhysicsEvaluation::CaptureResources();
        Json Summary = {{"Record", "Summary"}, {"Frames", Result.Frames},
                        {"SoftSubsteps", Result.SoftSubsteps},
                        {"FirstBudgetExceededFrame", Result.FirstBudgetExceededFrame},
                        {"InitialKineticEnergy", Result.InitialKineticEnergy},
                        {"MaximumKineticEnergy", Result.MaximumKineticEnergy},
                        {"MaxEdgeStretch", Result.MaxEdgeStretch},
                        {"MaxPinDrift", Result.MaxPinDrift},
                        {"MaxParticlePenetration", Result.MaxParticlePenetration},
                        {"MaximumCompliantEdgeElasticEstimate", Result.MaximumCompliantEdgeElasticEstimate},
                        {"MaximumKineticPlusCompliantEdgeEstimate", Result.MaximumKineticPlusCompliantEdgeEstimate},
                        {"MaximumOmittedHardEdgeCount", Result.MaximumOmittedHardEdgeCount},
                        {"TotalContactProjections", Result.TotalContactProjections},
                        {"Finite", Result.Finite},
                        {"TotalPhysicalPhasesMs", Result.TotalPhysicalPhasesMs},
                        {"TotalSubstepPipelineMs", Result.TotalSubstepPipelineMs},
                        {"TotalObservationReadMs", Result.TotalObservationReadMs},
                        {"TotalCouplingPipelineWithoutObservationsMs",
                         Result.TotalCouplingPipelineWithoutObservationsMs},
                        {"ElasticEstimateScope", "Sum (current edge length - rest length)^2 / (2*compliance) over positive-compliance structural, diagonal, and two-hop bend distance edges. Hard edges are omitted and counted. This is a spring analogy, not stored XPBD solver energy or a complete mechanical-energy balance."},
                        {"TimingScope", "RigidMs=Box3D Step only; SoftMs=XPBD Step only; FeedbackMs=feedback extraction, clamp/application and final rigid read; PhysicalPhasesMs=sum of these three; ObservationReadMs=post-soft-step result validation, state lookup, accepted dynamics read and optional diagnostic trace read/validation; SubstepPipelineMs=from initial rigid read through collider snapshot, observation reads, feedback and final rigid read; CouplingPipelineWithoutObservationsMs=SubstepPipelineMs minus ObservationReadMs; quality checks and JSON serialization excluded"}};
        Summary["FullRunResources"] = PhysicsEvaluation::ResourceDelta(Before, After);
        Summary["ResourceScope"] = "Full Run including world setup, solve, quality observations, per-frame JSON construction/output and teardown; summary JSON is constructed after the after-snapshot; not per-solver allocation cost";
        std::cout << Summary.dump() << '\n';
        return 0;
    } catch (const std::exception &Error) {
        std::cout << Json({{"Record", "Error"}, {"Message", Error.what()}}).dump() << '\n';
        return 2;
    }
}

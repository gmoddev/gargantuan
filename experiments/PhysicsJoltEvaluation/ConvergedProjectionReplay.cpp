// Standalone, contact-free ordered-edge replay with retained per-step XPBD multipliers.
// One projection sweep is the prior calibrated Jolt scalar replay; additional
// sweeps test a converged constraint solve without changing production code.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using Json = nlohmann::json;

struct Options {
    std::string GraphPath;
    std::string Precision = "float";
    std::string RestMode = "captured";
    std::string VelocityReconstruction = "difference";
    bool VelocityReconstructionSpecified = false;
    int Steps = 1;
    int Frames = 150;
    int IntegrationsPerFrame = 0;
    int StageLimit = 9;
    int SeedIndex = 84;
    int ProjectionIterations = 1;
    double Tolerance = 0.0;
    double SeedX = 0.0;
    double SeedY = 0.0;
    double SeedZ = 1.0;
    bool Particles = false;
};

Options Parse(int Count, char **Arguments) {
    Options Result;
    for (int Index = 1; Index < Count; ++Index) {
        const std::string Name = Arguments[Index];
        const auto Value = [&]() -> std::string {
            if (Index + 1 >= Count) throw std::invalid_argument("[Physics:ConvergedReplay] Missing option value: " + Name);
            return Arguments[++Index];
        };
        if (Name == "--graph") Result.GraphPath = Value();
        else if (Name == "--precision") Result.Precision = Value();
        else if (Name == "--rest") Result.RestMode = Value();
        else if (Name == "--velocity-reconstruction") {
            Result.VelocityReconstruction = Value();
            Result.VelocityReconstructionSpecified = true;
        }
        else if (Name == "--steps") Result.Steps = std::stoi(Value());
        else if (Name == "--frames") Result.Frames = std::stoi(Value());
        else if (Name == "--integrations-per-frame") Result.IntegrationsPerFrame = std::stoi(Value());
        else if (Name == "--stage-limit") Result.StageLimit = std::stoi(Value());
        else if (Name == "--seed-index") Result.SeedIndex = std::stoi(Value());
        else if (Name == "--projection-iterations") Result.ProjectionIterations = std::stoi(Value());
        else if (Name == "--tolerance") Result.Tolerance = std::stod(Value());
        else if (Name == "--seed-x") Result.SeedX = std::stod(Value());
        else if (Name == "--seed-y") Result.SeedY = std::stod(Value());
        else if (Name == "--seed-z") Result.SeedZ = std::stod(Value());
        else if (Name == "--particles") Result.Particles = std::stoi(Value()) != 0;
        else if (Name == "--steps1") Result.Steps = 1;
        else if (Name == "--steps4") Result.Steps = 4;
        else if (Name == "--seed-x0") Result.SeedX = 0.0;
        else if (Name == "--seed-x1") Result.SeedX = 1.0;
        else if (Name == "--seed-y0") Result.SeedY = 0.0;
        else if (Name == "--seed-y1") Result.SeedY = 1.0;
        else if (Name == "--seed-z0") Result.SeedZ = 0.0;
        else if (Name == "--seed-z1") Result.SeedZ = 1.0;
        else if (Name == "--unseeded") Result.SeedX = Result.SeedY = Result.SeedZ = 0.0;
        else if (Name == "--particles0") Result.Particles = false;
        else if (Name == "--particles1") Result.Particles = true;
        else throw std::invalid_argument("[Physics:ConvergedReplay] Unknown option: " + Name);
    }
    if (Result.GraphPath.empty() ||
        (Result.Precision != "float" && Result.Precision != "double" && Result.Precision != "longdouble") ||
        (Result.RestMode != "captured" && Result.RestMode != "recomputed") ||
        (Result.VelocityReconstruction != "difference" && Result.VelocityReconstruction != "delta" &&
         Result.VelocityReconstruction != "applieddelta") ||
        (Result.Steps != 1 && Result.Steps != 4) || Result.Frames < 1 || Result.Frames > 3600 ||
        Result.IntegrationsPerFrame < 0 || Result.IntegrationsPerFrame > 768 ||
        Result.StageLimit < 0 || Result.StageLimit > 96 || Result.SeedIndex < 0 ||
        Result.ProjectionIterations < 1 || Result.ProjectionIterations > 256 ||
        !std::isfinite(Result.Tolerance) || Result.Tolerance < 0.0 || Result.Tolerance > 1.0 ||
        !std::isfinite(Result.SeedX) || !std::isfinite(Result.SeedY) || !std::isfinite(Result.SeedZ) ||
        std::abs(Result.SeedX) > 100.0 || std::abs(Result.SeedY) > 100.0 ||
        std::abs(Result.SeedZ) > 100.0)
        throw std::invalid_argument("[Physics:ConvergedReplay] Option outside bounded range");
    return Result;
}

template <class T> struct Vector3 { T X{}, Y{}, Z{}; };
template <class T> struct Vertex {
    Vector3<T> Position;
    Vector3<T> Previous;
    Vector3<T> Velocity;
    Vector3<T> Rest;
    T InverseMass{};
};
template <class T> struct Edge {
    std::size_t I{}, J{};
    T RestLength{}, Compliance{};
};

template <class T> T Length(const Vector3<T> &Delta) {
    // Parenthesization matches the x/y/z reduction requested for native parity.
    const T Squared = (Delta.X * Delta.X + Delta.Y * Delta.Y) + Delta.Z * Delta.Z;
    return std::sqrt(Squared);
}

template <class T> Vector3<T> Difference(const Vector3<T> &A, const Vector3<T> &B) {
    return {A.X - B.X, A.Y - B.Y, A.Z - B.Z};
}

template <class T> Json Vector(const Vector3<T> &Value) {
    return Json::array({static_cast<double>(Value.X), static_cast<double>(Value.Y),
                        static_cast<double>(Value.Z)});
}

template <class T> struct Observation {
    double Kinetic{}, KineticX{}, KineticY{}, KineticZ{};
    double Elastic{}, MaxStrain{}, MaxPinDrift{}, MaxPinSpeed{};
    double MomentumX{}, MomentumY{}, MomentumZ{};
    bool Finite = true;
};

template <class T> Observation<T> Observe(const std::vector<Vertex<T>> &Vertices,
                                           const std::vector<Edge<T>> &Edges,
                                           T StepTime, bool Candidate) {
    Observation<T> Result;
    for (const auto &Item : Vertices) {
        const auto Velocity = Candidate ? Vector3<T>{
            (Item.Position.X - Item.Previous.X) / StepTime,
            (Item.Position.Y - Item.Previous.Y) / StepTime,
            (Item.Position.Z - Item.Previous.Z) / StepTime} : Item.Velocity;
        if (Item.InverseMass > T(0)) {
            const double Mass = 1.0 / static_cast<double>(Item.InverseMass);
            const double X = static_cast<double>(Velocity.X);
            const double Y = static_cast<double>(Velocity.Y);
            const double Z = static_cast<double>(Velocity.Z);
            Result.KineticX += 0.5 * Mass * X * X;
            Result.KineticY += 0.5 * Mass * Y * Y;
            Result.KineticZ += 0.5 * Mass * Z * Z;
            Result.MomentumX += Mass * X;
            Result.MomentumY += Mass * Y;
            Result.MomentumZ += Mass * Z;
        } else {
            const auto Drift = Length(Difference(Item.Position, Item.Rest));
            const auto Speed = Length(Velocity);
            Result.MaxPinDrift = std::max(Result.MaxPinDrift, static_cast<double>(Drift));
            Result.MaxPinSpeed = std::max(Result.MaxPinSpeed, static_cast<double>(Speed));
        }
        Result.Finite = Result.Finite && std::isfinite(Item.Position.X) &&
            std::isfinite(Item.Position.Y) && std::isfinite(Item.Position.Z) &&
            std::isfinite(Velocity.X) && std::isfinite(Velocity.Y) && std::isfinite(Velocity.Z);
    }
    Result.Kinetic = Result.KineticX + Result.KineticY + Result.KineticZ;
    for (const auto &Constraint : Edges) {
        const T CurrentLength = Length(Difference(Vertices[Constraint.J].Position,
                                                   Vertices[Constraint.I].Position));
        const double Error = static_cast<double>(CurrentLength - Constraint.RestLength);
        const double Rest = static_cast<double>(Constraint.RestLength);
        Result.MaxStrain = std::max(Result.MaxStrain, std::abs(Error) / Rest);
        if (Constraint.Compliance > T(0))
            Result.Elastic += Error * Error / (2.0 * static_cast<double>(Constraint.Compliance));
    }
    Result.Finite = Result.Finite && std::isfinite(Result.Kinetic) &&
        std::isfinite(Result.Elastic) && std::isfinite(Result.MaxStrain);
    return Result;
}

template <class T> Json Particles(const std::vector<Vertex<T>> &Vertices) {
    Json Result = Json::array();
    for (const auto &Item : Vertices)
        Result.push_back({{"P", Vector(Item.Position)}, {"Prev", Vector(Item.Previous)},
                          {"V", Vector(Item.Velocity)},
                          {"InvMass", static_cast<double>(Item.InverseMass)}});
    return Result;
}

template <class T> Json Metrics(const Observation<T> &Value) {
    return {{"KineticEnergy", Value.Kinetic}, {"SoftKE", Value.Kinetic},
            {"KineticX", Value.KineticX}, {"KineticY", Value.KineticY},
            {"KineticZ", Value.KineticZ},
            {"CompliantEdgeElasticEstimate", Value.Elastic},
            {"KineticPlusElasticEstimate", Value.Kinetic + Value.Elastic},
            {"MaxEdgeStrain", Value.MaxStrain},
            {"MaxPinDrift", Value.MaxPinDrift}, {"MaxPinSpeed", Value.MaxPinSpeed},
            {"SoftMomentum", Json::array({Value.MomentumX, Value.MomentumY, Value.MomentumZ})},
            {"Finite", Value.Finite}};
}

template <class T> void EmitStage(const Options &Config, int Frame, int Microstep,
                                  const char *Phase, const std::vector<Vertex<T>> &Vertices,
                                  const std::vector<Edge<T>> &Edges, T StepTime,
                                  bool Candidate) {
    if (Frame != 0 || Microstep >= Config.StageLimit) return;
    Json Row = Metrics(Observe(Vertices, Edges, StepTime, Candidate));
    Row["Type"] = "Stage";
    Row["Frame"] = Frame;
    Row["Microstep"] = Microstep;
    Row["Phase"] = Phase;
    Row["CandidateFromPositions"] = Candidate;
    if (Config.Particles) Row["Particles"] = Particles(Vertices);
    std::cout << Row.dump() << '\n';
}

template <class T> void Replay(const Options &Config, const Json &Graph) {
    const auto &Positions = Graph.at("Positions");
    const auto &InverseMasses = Graph.at("InverseMasses");
    const auto &GraphEdges = Graph.at("Edges");
    if (!Positions.is_array() || !InverseMasses.is_array() || !GraphEdges.is_array() ||
        Positions.empty() || Positions.size() != InverseMasses.size() ||
        Positions.size() > 4096 || GraphEdges.empty() || GraphEdges.size() > 20000)
        throw std::invalid_argument("[Physics:ConvergedReplay] Invalid or oversized graph");
    std::vector<Vertex<T>> Vertices;
    Vertices.reserve(Positions.size());
    for (std::size_t Index = 0; Index < Positions.size(); ++Index) {
        const auto &Entry = Positions.at(Index);
        if (!Entry.is_array() || Entry.size() != 3)
            throw std::invalid_argument("[Physics:ConvergedReplay] Position must have three coordinates");
        const Vector3<T> Position{static_cast<T>(Entry.at(0).get<double>()),
                                  static_cast<T>(Entry.at(1).get<double>()),
                                  static_cast<T>(Entry.at(2).get<double>())};
        const T InverseMass = static_cast<T>(InverseMasses.at(Index).get<double>());
        if (!std::isfinite(Position.X) || !std::isfinite(Position.Y) ||
            !std::isfinite(Position.Z) || !std::isfinite(InverseMass) || InverseMass < T(0))
            throw std::invalid_argument("[Physics:ConvergedReplay] Nonfinite position or inverse mass");
        Vertices.push_back({Position, Position, {}, Position, InverseMass});
    }
    if (static_cast<std::size_t>(Config.SeedIndex) >= Vertices.size() ||
        Vertices[Config.SeedIndex].InverseMass <= T(0))
        throw std::invalid_argument("[Physics:ConvergedReplay] Movable seed vertex is required");
    Vertices[Config.SeedIndex].Velocity = {static_cast<T>(Config.SeedX), static_cast<T>(Config.SeedY),
                             static_cast<T>(Config.SeedZ)};
    std::vector<Edge<T>> Edges;
    Edges.reserve(GraphEdges.size());
    double MaximumCapturedRestDifference = 0.0;
    for (const auto &Entry : GraphEdges) {
        const auto I = Entry.at("I").get<std::size_t>();
        const auto J = Entry.at("J").get<std::size_t>();
        if (I >= Vertices.size() || J >= Vertices.size() || I == J)
            throw std::invalid_argument("[Physics:ConvergedReplay] Invalid edge indices");
        T RestLength = static_cast<T>(Entry.at("RestLength").get<double>());
        const T Compliance = static_cast<T>(Entry.at("Compliance").get<double>());
        const T RecomputedLength = Length(Difference(Vertices[J].Rest, Vertices[I].Rest));
        MaximumCapturedRestDifference = std::max(MaximumCapturedRestDifference,
            std::abs(static_cast<double>(RestLength) - static_cast<double>(RecomputedLength)));
        if (Config.RestMode == "recomputed")
            RestLength = RecomputedLength;
        if (!std::isfinite(RestLength) || RestLength <= T(0) ||
            !std::isfinite(Compliance) || Compliance < T(0))
            throw std::invalid_argument("[Physics:ConvergedReplay] Invalid edge rest length/compliance");
        Edges.push_back({I, J, RestLength, Compliance});
    }
    // The frame/outer division is intentionally performed in float even when
    // the solver arithmetic below uses double or long double.
    const float FrameTimeNative = 1.0f / 60.0f;
    const float OuterTimeNative = FrameTimeNative / static_cast<float>(Config.Steps);
    // The explicit cadence models one native World.Update per integration:
    // the frame duration is divided directly in float by that update count.
    // The default preserves the historical outer/24 division exactly.
    const float InnerTimeNative = Config.IntegrationsPerFrame > 0 ?
        FrameTimeNative / static_cast<float>(Config.IntegrationsPerFrame) : OuterTimeNative / 24.0f;
    const int IntegrationsPerFrame = Config.IntegrationsPerFrame > 0 ?
        Config.IntegrationsPerFrame : 24 * Config.Steps;
    const T StepTime = static_cast<T>(InnerTimeNative);
    const T DampingRate = static_cast<T>(2.4f);
    const T Damping = std::max(T(0), T(1) - DampingRate * StepTime);
    const T InverseTimeSquared = T(1) / (StepTime * StepTime);
    const auto Initial = Observe(Vertices, Edges, StepTime, false);
    Json ConfigRow = {{"Type", "Config"}, {"Precision", Config.Precision},
                       {"RestMode", Config.RestMode}, {"Steps", Config.Steps},
                       {"Frames", Config.Frames}, {"SoftIntegrationsPerOuter", 24},
                       {"InternalIntegrationDt", static_cast<double>(InnerTimeNative)},
                       {"DampingRateCapturedFloat", static_cast<double>(2.4f)},
                       {"DampingMultiplier", static_cast<double>(Damping)},
                       {"SeedCenterIndex", Config.SeedIndex},
                       {"ProjectionIterations", Config.ProjectionIterations},
                       {"NormalizedEquilibriumTolerance", Config.Tolerance},
                       {"MultiplierScope", "Per-edge XPBD multipliers reset at each integration and retained across ordered projection sweeps. Iteration 1 uses the original native scalar correction order."},
                       {"SeedVelocity", Json::array({Config.SeedX, Config.SeedY, Config.SeedZ})},
                       {"InitialVelocityKineticEnergy", Initial.Kinetic},
                       {"MaximumCapturedVsRecomputedRestLengthDifference", MaximumCapturedRestDifference},
                       {"EdgeCount", Edges.size()}, {"VertexCount", Vertices.size()},
                       {"ArithmeticScope", "float: captured float graph, native float h/damping, ordered scalar edge formula; double/longdouble: same captured float inputs and h, but solver arithmetic at selected precision. Recomputed rest derives from graph positions at selected precision."},
                       {"ModelLimit", "Contact-free, zero-gravity, no native SIMD/job side effects. Float parity requires hosted native frame and particle comparison."}};
    if (Config.IntegrationsPerFrame > 0) {
        ConfigRow["IntegrationsPerFrame"] = IntegrationsPerFrame;
        ConfigRow["IntegrationSchedule"] = "Direct float frame duration divided by integrations per frame; one projection solve per integration.";
        ConfigRow["SoftIntegrationsPerOuter"] = nullptr;
    }
    if (Config.VelocityReconstructionSpecified) {
        ConfigRow["VelocityReconstruction"] = Config.VelocityReconstruction;
        ConfigRow["VelocityReconstructionScope"] = "difference reconstructs from stored position difference; delta accumulates requested edge displacement in solver precision; applieddelta accumulates stored edge displacement in solver precision. The alternatives start from damped velocity and exclude integration prediction rounding.";
    }
    std::cout << ConfigRow.dump() << '\n';
    double MaximumKinetic = Initial.Kinetic;
    int FirstNonfiniteFrame = -1;
    double MaximumEquilibriumResidual = 0.0;
    double MaximumRawConstraintViolation = 0.0;
    double MaximumAbsLambda = 0.0;
    int MaximumSweepsUsed = 0;
    int MinimumSweepsUsed = Config.ProjectionIterations;
    const bool AlternateVelocity = Config.VelocityReconstruction != "difference";
    std::vector<Vector3<T>> DampedVelocities;
    std::vector<Vector3<T>> CorrectionSum;
    if (AlternateVelocity) {
        DampedVelocities.resize(Vertices.size());
        CorrectionSum.resize(Vertices.size());
    }
    double MaximumVelocityReconstructionDifference = 0.0;
    for (int Frame = 0; Frame < Config.Frames; ++Frame) {
        double FrameMaximumEquilibriumResidual = 0.0;
        double FrameMaximumRawConstraintViolation = 0.0;
        double FrameMaximumAbsLambda = 0.0;
        int FrameMaximumSweepsUsed = 0;
        int FrameMinimumSweepsUsed = Config.ProjectionIterations;
        double FrameVelocityReconstructionDifferenceSquared = 0.0;
        double FrameMaximumVelocityReconstructionDifference = 0.0;
        std::uint64_t FrameVelocityReconstructionSamples = 0;
        for (int Microstep = 0; Microstep < IntegrationsPerFrame; ++Microstep) {
            if (AlternateVelocity)
                std::fill(CorrectionSum.begin(), CorrectionSum.end(), Vector3<T>{});
            EmitStage(Config, Frame, Microstep, "BeforeIntegration", Vertices, Edges,
                      StepTime, false);
            for (std::size_t VertexIndex = 0; VertexIndex < Vertices.size(); ++VertexIndex) {
                auto &Item = Vertices[VertexIndex];
                if (Item.InverseMass > T(0)) {
                    Item.Velocity.X *= Damping;
                    Item.Velocity.Y *= Damping;
                    Item.Velocity.Z *= Damping;
                }
                if (AlternateVelocity) DampedVelocities[VertexIndex] = Item.Velocity;
                const auto OldPosition = Item.Position;
                Item.Previous = OldPosition;
                Item.Position.X = OldPosition.X + Item.Velocity.X * StepTime;
                Item.Position.Y = OldPosition.Y + Item.Velocity.Y * StepTime;
                Item.Position.Z = OldPosition.Z + Item.Velocity.Z * StepTime;
            }
            EmitStage(Config, Frame, Microstep, "AfterIntegration", Vertices, Edges,
                      StepTime, true);
            // XPBD multipliers live for one integration only; retaining them over
            // the sweeps is essential for compliant constraints.
            std::vector<T> Lambda(Edges.size(), T(0));
            int SweepsUsed = 0;
            double EquilibriumResidual = 0.0;
            double RawConstraintViolation = 0.0;
            for (int Iteration = 0; Iteration < Config.ProjectionIterations; ++Iteration) {
                for (std::size_t EdgeIndex = 0; EdgeIndex < Edges.size(); ++EdgeIndex) {
                    const auto &Constraint = Edges[EdgeIndex];
                    auto &V0 = Vertices[Constraint.I];
                    auto &V1 = Vertices[Constraint.J];
                    const auto X0 = V0.Position;
                    const auto X1 = V1.Position;
                    const T Dx = X1.X - X0.X;
                    const T Dy = X1.Y - X0.Y;
                    const T Dz = X1.Z - X0.Z;
                    const T CurrentLength = std::sqrt((Dx * Dx + Dy * Dy) + Dz * Dz);
                    const T Alpha = Constraint.Compliance * InverseTimeSquared;
                    const T Weight = V0.InverseMass + V1.InverseMass;
                    const T Denominator = CurrentLength * (Weight + Alpha);
                    if (Denominator < static_cast<T>(1.0e-12f)) continue;
                    const T Residual = CurrentLength - Constraint.RestLength;
                    T Cx{}, Cy{}, Cz{};
                    if (Iteration == 0) {
                        // Match the original one-pass float expression exactly.
                        Cx = (Dx * Residual) / Denominator;
                        Cy = (Dy * Residual) / Denominator;
                        Cz = (Dz * Residual) / Denominator;
                        Lambda[EdgeIndex] = -Residual / (Weight + Alpha);
                    } else {
                        const T DeltaLambda = -(Residual + Alpha * Lambda[EdgeIndex]) /
                            (Weight + Alpha);
                        Lambda[EdgeIndex] += DeltaLambda;
                        const T Scale = -DeltaLambda / CurrentLength;
                        Cx = Dx * Scale;
                        Cy = Dy * Scale;
                        Cz = Dz * Scale;
                    }
                    V0.Position.X = X0.X + V0.InverseMass * Cx;
                    V0.Position.Y = X0.Y + V0.InverseMass * Cy;
                    V0.Position.Z = X0.Z + V0.InverseMass * Cz;
                    V1.Position.X = X1.X - V1.InverseMass * Cx;
                    V1.Position.Y = X1.Y - V1.InverseMass * Cy;
                    V1.Position.Z = X1.Z - V1.InverseMass * Cz;
                    if (AlternateVelocity) {
                        auto &Sum0 = CorrectionSum[Constraint.I];
                        auto &Sum1 = CorrectionSum[Constraint.J];
                        if (Config.VelocityReconstruction == "delta") {
                            Sum0.X += V0.InverseMass * Cx;
                            Sum0.Y += V0.InverseMass * Cy;
                            Sum0.Z += V0.InverseMass * Cz;
                            Sum1.X -= V1.InverseMass * Cx;
                            Sum1.Y -= V1.InverseMass * Cy;
                            Sum1.Z -= V1.InverseMass * Cz;
                        } else {
                            Sum0.X += V0.Position.X - X0.X;
                            Sum0.Y += V0.Position.Y - X0.Y;
                            Sum0.Z += V0.Position.Z - X0.Z;
                            Sum1.X += V1.Position.X - X1.X;
                            Sum1.Y += V1.Position.Y - X1.Y;
                            Sum1.Z += V1.Position.Z - X1.Z;
                        }
                    }
                }
                SweepsUsed = Iteration + 1;
                if (Config.Tolerance > 0.0 || SweepsUsed == Config.ProjectionIterations) {
                    EquilibriumResidual = 0.0;
                    RawConstraintViolation = 0.0;
                    for (std::size_t EdgeIndex = 0; EdgeIndex < Edges.size(); ++EdgeIndex) {
                        const auto &Constraint = Edges[EdgeIndex];
                        const T CurrentLength = Length(Difference(
                            Vertices[Constraint.J].Position, Vertices[Constraint.I].Position));
                        const T C = CurrentLength - Constraint.RestLength;
                        const T Alpha = Constraint.Compliance * InverseTimeSquared;
                        const double InverseRest = 1.0 / static_cast<double>(Constraint.RestLength);
                        EquilibriumResidual = std::max(EquilibriumResidual,
                            std::abs(static_cast<double>(C + Alpha * Lambda[EdgeIndex])) * InverseRest);
                        RawConstraintViolation = std::max(RawConstraintViolation,
                            std::abs(static_cast<double>(C)) * InverseRest);
                    }
                    if (Config.Tolerance > 0.0 && EquilibriumResidual <= Config.Tolerance)
                        break;
                }
            }
            double AbsLambda = 0.0;
            for (const T Value : Lambda)
                AbsLambda = std::max(AbsLambda, std::abs(static_cast<double>(Value)));
            FrameMaximumEquilibriumResidual = std::max(FrameMaximumEquilibriumResidual,
                EquilibriumResidual);
            FrameMaximumRawConstraintViolation = std::max(FrameMaximumRawConstraintViolation,
                RawConstraintViolation);
            FrameMaximumAbsLambda = std::max(FrameMaximumAbsLambda, AbsLambda);
            FrameMaximumSweepsUsed = std::max(FrameMaximumSweepsUsed, SweepsUsed);
            FrameMinimumSweepsUsed = std::min(FrameMinimumSweepsUsed, SweepsUsed);
            EmitStage(Config, Frame, Microstep, "AfterProjection", Vertices, Edges,
                      StepTime, true);
            for (std::size_t VertexIndex = 0; VertexIndex < Vertices.size(); ++VertexIndex) {
                auto &Item = Vertices[VertexIndex];
                if (Item.InverseMass > T(0)) {
                    const Vector3<T> DifferenceVelocity{
                        (Item.Position.X - Item.Previous.X) / StepTime,
                        (Item.Position.Y - Item.Previous.Y) / StepTime,
                        (Item.Position.Z - Item.Previous.Z) / StepTime};
                    if (AlternateVelocity) {
                        const auto &DampedVelocity = DampedVelocities[VertexIndex];
                        const auto &Sum = CorrectionSum[VertexIndex];
                        Item.Velocity = {
                            DampedVelocity.X + Sum.X / StepTime,
                            DampedVelocity.Y + Sum.Y / StepTime,
                            DampedVelocity.Z + Sum.Z / StepTime};
                        const double Dx = static_cast<double>(Item.Velocity.X) - static_cast<double>(DifferenceVelocity.X);
                        const double Dy = static_cast<double>(Item.Velocity.Y) - static_cast<double>(DifferenceVelocity.Y);
                        const double Dz = static_cast<double>(Item.Velocity.Z) - static_cast<double>(DifferenceVelocity.Z);
                        const double MagnitudeSquared = Dx * Dx + Dy * Dy + Dz * Dz;
                        FrameVelocityReconstructionDifferenceSquared += MagnitudeSquared;
                        FrameMaximumVelocityReconstructionDifference = std::max(
                            FrameMaximumVelocityReconstructionDifference, std::sqrt(MagnitudeSquared));
                        ++FrameVelocityReconstructionSamples;
                    } else {
                        Item.Velocity = DifferenceVelocity;
                    }
                }
            }
            EmitStage(Config, Frame, Microstep, "AfterReconstruction", Vertices, Edges,
                      StepTime, false);
        }
        const auto State = Observe(Vertices, Edges, StepTime, false);
        MaximumKinetic = std::max(MaximumKinetic, State.Kinetic);
        Json Row = Metrics(State);
        Row["Type"] = "Frame";
        Row["Frame"] = Frame;
        Row["CenterPosition"] = Vector(Vertices[Config.SeedIndex].Position);
        Row["CenterVelocity"] = Vector(Vertices[Config.SeedIndex].Velocity);
        Row["MaxNormalizedEquilibriumResidual"] = FrameMaximumEquilibriumResidual;
        Row["MaxNormalizedRawConstraintViolation"] = FrameMaximumRawConstraintViolation;
        Row["MaxAbsLambda"] = FrameMaximumAbsLambda;
        Row["MinProjectionSweepsUsed"] = FrameMinimumSweepsUsed;
        Row["MaxProjectionSweepsUsed"] = FrameMaximumSweepsUsed;
        if (AlternateVelocity) {
            Row["VelocityReconstructionDifferenceRmsMps"] = std::sqrt(
                FrameVelocityReconstructionDifferenceSquared /
                static_cast<double>(std::max<std::uint64_t>(FrameVelocityReconstructionSamples, 1)));
            Row["VelocityReconstructionDifferenceMaxMps"] = FrameMaximumVelocityReconstructionDifference;
            MaximumVelocityReconstructionDifference = std::max(
                MaximumVelocityReconstructionDifference, FrameMaximumVelocityReconstructionDifference);
        }
        MaximumEquilibriumResidual = std::max(MaximumEquilibriumResidual, FrameMaximumEquilibriumResidual);
        MaximumRawConstraintViolation = std::max(MaximumRawConstraintViolation, FrameMaximumRawConstraintViolation);
        MaximumAbsLambda = std::max(MaximumAbsLambda, FrameMaximumAbsLambda);
        MaximumSweepsUsed = std::max(MaximumSweepsUsed, FrameMaximumSweepsUsed);
        MinimumSweepsUsed = std::min(MinimumSweepsUsed, FrameMinimumSweepsUsed);
        if (Config.Particles) Row["Particles"] = Particles(Vertices);
        std::cout << Row.dump() << '\n';
        if (!State.Finite) {
            FirstNonfiniteFrame = Frame;
            break;
        }
    }
    Json SummaryRow = {{"Type", "Summary"}, {"MaximumKineticEnergy", MaximumKinetic},
                       {"FirstNonfiniteFrame", FirstNonfiniteFrame},
                       {"MaxNormalizedEquilibriumResidual", MaximumEquilibriumResidual},
                       {"MaxNormalizedRawConstraintViolation", MaximumRawConstraintViolation},
                       {"MaxAbsLambda", MaximumAbsLambda},
                       {"MinProjectionSweepsUsed", MinimumSweepsUsed},
                       {"MaxProjectionSweepsUsed", MaximumSweepsUsed},
                       {"InitialVelocityKineticEnergy", Initial.Kinetic}};
    if (AlternateVelocity)
        SummaryRow["MaximumVelocityReconstructionDifferenceMps"] = MaximumVelocityReconstructionDifference;
    std::cout << SummaryRow.dump() << '\n';
}
} // namespace

int main(int ArgumentCount, char **Arguments) {
    try {
        const auto Config = Parse(ArgumentCount, Arguments);
        std::ifstream Input(Config.GraphPath);
        if (!Input) throw std::runtime_error("[Physics:ConvergedReplay] Cannot open graph");
        Json Graph;
        Input >> Graph;
        if (Config.Precision == "float") Replay<float>(Config, Graph);
        else if (Config.Precision == "double") Replay<double>(Config, Graph);
        else Replay<long double>(Config, Graph);
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << Error.what() << '\n';
        return 2;
    }
}


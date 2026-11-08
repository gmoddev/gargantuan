// Fixed-prediction XPBD convergence probe for the exported contact-free cloth.
// This replays one frozen integration snapshot and never advances the trajectory.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;

struct Options {
    std::string GraphPath, StagesPath, Precision = "float", RestMode = "captured";
    int Microstep = 57;
    int Sweeps = 16384;
};

Options Parse(int Count, char **Arguments) {
    Options Result;
    for (int Index = 1; Index < Count; ++Index) {
        const std::string Name = Arguments[Index];
        if (Index + 1 >= Count)
            throw std::invalid_argument("[Physics:SnapshotProbe] Missing option value: " + Name);
        const std::string Value = Arguments[++Index];
        if (Name == "--graph") Result.GraphPath = Value;
        else if (Name == "--stages") Result.StagesPath = Value;
        else if (Name == "--precision") Result.Precision = Value;
        else if (Name == "--rest") Result.RestMode = Value;
        else if (Name == "--microstep") Result.Microstep = std::stoi(Value);
        else if (Name == "--sweeps") Result.Sweeps = std::stoi(Value);
        else throw std::invalid_argument("[Physics:SnapshotProbe] Unknown option: " + Name);
    }
    if (Result.GraphPath.empty() || Result.StagesPath.empty() ||
        (Result.Precision != "float" && Result.Precision != "double") ||
        (Result.RestMode != "captured" && Result.RestMode != "recomputed") ||
        Result.Microstep < 0 || Result.Microstep > 95 ||
        Result.Sweeps < 1 || Result.Sweeps > 65536)
        throw std::invalid_argument("[Physics:SnapshotProbe] Option outside bounded range");
    return Result;
}

template <class T> struct Vector3 { T X{}, Y{}, Z{}; };
template <class T> struct Vertex { Vector3<T> Position, Previous; T InverseMass{}; };
template <class T> struct Edge { std::size_t I{}, J{}; T Rest{}, Compliance{}; };

template <class T> Vector3<T> ReadVector(const Json &Value) {
    if (!Value.is_array() || Value.size() != 3)
        throw std::invalid_argument("[Physics:SnapshotProbe] Invalid vector");
    return {static_cast<T>(Value.at(0).get<double>()),
            static_cast<T>(Value.at(1).get<double>()),
            static_cast<T>(Value.at(2).get<double>())};
}

template <class T> T Length(const Vector3<T> &Delta) {
    return std::sqrt((Delta.X * Delta.X + Delta.Y * Delta.Y) + Delta.Z * Delta.Z);
}

template <class T> Vector3<T> Difference(const Vector3<T> &A, const Vector3<T> &B) {
    return {A.X - B.X, A.Y - B.Y, A.Z - B.Z};
}

struct Metrics { double Kinetic{}, Elastic{}, Equilibrium{}, RawViolation{}, MaxLambda{}; bool Finite = true; };

template <class T> Metrics Measure(const std::vector<Vertex<T>> &Vertices,
                                   const std::vector<Edge<T>> &Edges,
                                   const std::vector<T> &Lambda, T StepTime,
                                   T InverseTimeSquared) {
    Metrics Result;
    for (const auto &Item : Vertices) {
        if (Item.InverseMass <= T(0)) continue;
        const double Mass = 1.0 / static_cast<double>(Item.InverseMass);
        const auto Velocity = Difference(Item.Position, Item.Previous);
        const double X = static_cast<double>(Velocity.X / StepTime);
        const double Y = static_cast<double>(Velocity.Y / StepTime);
        const double Z = static_cast<double>(Velocity.Z / StepTime);
        Result.Kinetic += 0.5 * Mass * ((X * X + Y * Y) + Z * Z);
    }
    for (std::size_t Index = 0; Index < Edges.size(); ++Index) {
        const auto &Constraint = Edges[Index];
        const T CurrentLength = Length(Difference(Vertices[Constraint.J].Position,
                                                   Vertices[Constraint.I].Position));
        const T C = CurrentLength - Constraint.Rest;
        const T Alpha = Constraint.Compliance * InverseTimeSquared;
        const double Error = static_cast<double>(C);
        const double Rest = static_cast<double>(Constraint.Rest);
        Result.RawViolation = std::max(Result.RawViolation, std::abs(Error) / Rest);
        Result.Equilibrium = std::max(Result.Equilibrium,
            std::abs(static_cast<double>(C + Alpha * Lambda[Index])) / Rest);
        Result.MaxLambda = std::max(Result.MaxLambda,
            std::abs(static_cast<double>(Lambda[Index])));
        if (Constraint.Compliance > T(0))
            Result.Elastic += Error * Error / (2.0 * static_cast<double>(Constraint.Compliance));
    }
    Result.Finite = std::isfinite(Result.Kinetic) && std::isfinite(Result.Elastic) &&
        std::isfinite(Result.Equilibrium) && std::isfinite(Result.RawViolation) &&
        std::isfinite(Result.MaxLambda);
    return Result;
}

template <class T> void Emit(int Sweep, const Metrics &Value, double PreviousMechanical) {
    std::cout << Json({{"Type", "Sweep"}, {"Sweeps", Sweep},
                       {"CandidateKineticEnergy", Value.Kinetic},
                       {"CompliantEdgeElasticEstimate", Value.Elastic},
                       {"CandidateKineticPlusElasticEstimate", Value.Kinetic + Value.Elastic},
                       {"MechanicalDifferenceFromPreviousState",
                        Value.Kinetic + Value.Elastic - PreviousMechanical},
                       {"MaxNormalizedEquilibriumResidual", Value.Equilibrium},
                       {"MaxNormalizedRawConstraintViolation", Value.RawViolation},
                       {"MaxAbsLambda", Value.MaxLambda}, {"Finite", Value.Finite}}).dump() << '\n';
}

template <class T> void Probe(const Options &Config, const Json &Graph,
                              const Json &Snapshot, double StepTimeDouble,
                              double PreviousMechanical) {
    const auto &Positions = Graph.at("Positions");
    const auto &GraphEdges = Graph.at("Edges");
    const auto &Particles = Snapshot.at("Particles");
    if (!Positions.is_array() || Positions.size() != 169 ||
        !GraphEdges.is_array() || GraphEdges.size() != 886 ||
        !Particles.is_array() || Particles.size() != Positions.size())
        throw std::invalid_argument("[Physics:SnapshotProbe] Expected 169 particles and 886 edges");
    std::vector<Vertex<T>> Vertices;
    Vertices.reserve(Particles.size());
    for (const auto &Particle : Particles)
        Vertices.push_back({ReadVector<T>(Particle.at("P")),
                            ReadVector<T>(Particle.at("Prev")),
                            static_cast<T>(Particle.at("InvMass").get<double>())});
    std::vector<Edge<T>> Edges;
    Edges.reserve(GraphEdges.size());
    for (const auto &Item : GraphEdges) {
        const auto I = Item.at("I").get<std::size_t>();
        const auto J = Item.at("J").get<std::size_t>();
        if (I >= Vertices.size() || J >= Vertices.size() || I == J)
            throw std::invalid_argument("[Physics:SnapshotProbe] Invalid edge index");
        T Rest = static_cast<T>(Item.at("RestLength").get<double>());
        if (Config.RestMode == "recomputed")
            Rest = Length(Difference(ReadVector<T>(Positions.at(J)),
                                     ReadVector<T>(Positions.at(I))));
        const T Compliance = static_cast<T>(Item.at("Compliance").get<double>());
        if (!std::isfinite(Rest) || Rest <= T(0) || !std::isfinite(Compliance) || Compliance < T(0))
            throw std::invalid_argument("[Physics:SnapshotProbe] Invalid edge material");
        Edges.push_back({I, J, Rest, Compliance});
    }
    const T StepTime = static_cast<T>(StepTimeDouble);
    const T InverseTimeSquared = T(1) / (StepTime * StepTime);
    std::vector<T> Lambda(Edges.size(), T(0));
    std::cout << Json({{"Type", "Config"}, {"Precision", Config.Precision},
                       {"RestMode", Config.RestMode}, {"Microstep", Config.Microstep},
                       {"StepTime", StepTimeDouble}, {"MaxSweeps", Config.Sweeps},
                       {"PreviousKineticPlusElasticEstimate", PreviousMechanical},
                       {"PredictionFrozen", true}}).dump() << '\n';
    Emit<T>(0, Measure(Vertices, Edges, Lambda, StepTime, InverseTimeSquared), PreviousMechanical);
    for (int Sweep = 1; Sweep <= Config.Sweeps; ++Sweep) {
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
            const T C = CurrentLength - Constraint.Rest;
            T Cx{}, Cy{}, Cz{};
            if (Sweep == 1) {
                Cx = (Dx * C) / Denominator;
                Cy = (Dy * C) / Denominator;
                Cz = (Dz * C) / Denominator;
                Lambda[EdgeIndex] = -C / (Weight + Alpha);
            } else {
                const T DeltaLambda = -(C + Alpha * Lambda[EdgeIndex]) / (Weight + Alpha);
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
        }
        if ((Sweep & (Sweep - 1)) == 0 || Sweep == Config.Sweeps)
            Emit<T>(Sweep, Measure(Vertices, Edges, Lambda, StepTime,
                                    InverseTimeSquared), PreviousMechanical);
    }
}
} // namespace

int main(int Count, char **Arguments) {
    try {
        const auto Config = Parse(Count, Arguments);
        std::ifstream GraphInput(Config.GraphPath), StageInput(Config.StagesPath);
        if (!GraphInput || !StageInput)
            throw std::runtime_error("[Physics:SnapshotProbe] Cannot open graph or stage file");
        Json Graph;
        GraphInput >> Graph;
        Json Snapshot, Before, Metadata;
        std::string Line;
        while (std::getline(StageInput, Line)) {
            if (Line.empty()) continue;
            auto Row = Json::parse(Line);
            if (Row.value("Type", "") == "Config") Metadata = std::move(Row);
            else if (Row.value("Type", "") == "Stage" &&
                     Row.value("Frame", -1) == 0 &&
                     Row.value("Microstep", -1) == Config.Microstep) {
                if (Row.value("Phase", "") == "BeforeIntegration") Before = std::move(Row);
                else if (Row.value("Phase", "") == "AfterIntegration") Snapshot = std::move(Row);
            }
        }
        if (Metadata.is_null() || Before.is_null() || Snapshot.is_null())
            throw std::invalid_argument("[Physics:SnapshotProbe] Missing requested full-particle stages");
        const double StepTime = Metadata.at("InternalIntegrationDt").get<double>();
        const double PreviousMechanical = Before.at("KineticPlusElasticEstimate").get<double>();
        if (Config.Precision == "float") Probe<float>(Config, Graph, Snapshot, StepTime, PreviousMechanical);
        else Probe<double>(Config, Graph, Snapshot, StepTime, PreviousMechanical);
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << Error.what() << '\n';
        return 2;
    }
}

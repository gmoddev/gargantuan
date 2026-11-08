#!/usr/bin/env python3
"""Compare native phase traces with a conditional at-rest linear projection model.

This is an artifact-only, standard-library analysis. The full-state prediction
starts at the first observed BeforeIntegration state, so the reported residual
does not silently assume that a rounded native injection is an exact eigenmode.
"""

import argparse
import json
import math
from pathlib import Path
import sys


def Dot(Left, Right):
    return sum(A * B for A, B in zip(Left, Right))


def ComplexNumber(Value):
    return complex(float(Value["Real"]), float(Value["Imaginary"]))


def LoadStages(PathName, LastFrame):
    Stages = []
    Contacts = 0
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            if int(Row.get("Frame", -1)) > LastFrame:
                continue
            if Row.get("Type") in ("DynamicContact", "ContactProjection"):
                Contacts += 1
            if Row.get("Type") == "Stage":
                Stages.append(Row)
    if not Stages:
        raise ValueError(f"No Stage records in {PathName}")
    return Stages, Contacts


def Masses(Graph):
    return [0.0 if W == 0.0 else 1.0 / float(W) for W in Graph["InverseMasses"]]


def State(Stage, Rest, Interval):
    Particles = Stage["Particles"]
    if len(Particles) != len(Rest):
        raise ValueError("Trace particle count differs from exported graph")
    Q = [[float(P["P"][Axis]) - Rest[Index][Axis] for Axis in range(3)]
         for Index, P in enumerate(Particles)]
    Y = [[Interval * float(P["V"][Axis]) for Axis in range(3)] for P in Particles]
    return Q, Y


def WeightedNorm(Q, Y, Mass):
    return math.sqrt(sum(M * (Dot(A, A) + Dot(B, B))
                         for M, A, B in zip(Mass, Q, Y)))


def WeightedDistance(Left, Right, Mass):
    Q = [[A - B for A, B in zip(X, Z)] for X, Z in zip(Left[0], Right[0])]
    Y = [[A - B for A, B in zip(X, Z)] for X, Z in zip(Left[1], Right[1])]
    return WeightedNorm(Q, Y, Mass)


def WeightedInner(Left, Right, Mass):
    return sum(M * (Dot(A, C) + Dot(B, D)) for M, A, B, C, D in
               zip(Mass, Left[0], Left[1], Right[0], Right[1]))


def Energies(Stage, Mass):
    XY = Z = 0.0
    for Particle, M in zip(Stage["Particles"], Mass):
        if M == 0.0:
            continue
        V = Particle["V"]
        XY += 0.5 * M * (float(V[0]) ** 2 + float(V[1]) ** 2)
        Z += 0.5 * M * float(V[2]) ** 2
    return XY, Z


def PredictedEnergy(Y, Mass, Interval):
    return sum(0.5 * M * Dot(V, V) / (Interval * Interval)
               for M, V in zip(Mass, Y))


def LinearStep(StateValue, Edges, InvMass, Interval, Damping, Passes):
    Q, Y = StateValue
    Candidate = [[A + Damping * B for A, B in zip(Pos, Vel)]
                 for Pos, Vel in zip(Q, Y)]
    for _ in range(Passes):
        for I, J, Direction, Compliance in Edges:
            Denominator = InvMass[I] + InvMass[J] + Compliance / (Interval * Interval)
            if Denominator == 0.0:
                continue
            Difference = [A - B for A, B in zip(Candidate[I], Candidate[J])]
            Delta = -Dot(Direction, Difference) / Denominator
            for Axis in range(3):
                Candidate[I][Axis] += InvMass[I] * Direction[Axis] * Delta
                Candidate[J][Axis] -= InvMass[J] * Direction[Axis] * Delta
    NewY = [[A - B for A, B in zip(Pos, Old)]
            for Pos, Old in zip(Candidate, Q)]
    return Candidate, NewY


def MakeEdges(Graph, ComplianceOverride):
    Rest = Graph["Positions"]
    Edges = []
    WorstRestMismatch = 0.0
    for Edge in Graph["Edges"]:
        I, J = int(Edge["I"]), int(Edge["J"])
        Delta = [float(A) - float(B) for A, B in zip(Rest[I], Rest[J])]
        ActualLength = math.sqrt(Dot(Delta, Delta))
        Length = float(Edge["RestLength"])
        WorstRestMismatch = max(WorstRestMismatch, abs(ActualLength - Length))
        if ActualLength <= 0 or abs(ActualLength - Length) > 1.0e-5 * max(1.0, Length):
            raise ValueError("Graph is not an unperturbed rest graph")
        Compliance = (float(Edge["Compliance"]) if ComplianceOverride is None
                      else ComplianceOverride)
        Edges.append((I, J, [Value / ActualLength for Value in Delta], Compliance))
    return Edges, WorstRestMismatch


def GeometricMeanRatio(Values):
    Ratios = [B / A for A, B in zip(Values, Values[1:]) if A > 0 and B > 0]
    return math.exp(sum(math.log(Value) for Value in Ratios) / len(Ratios)) if Ratios else None


def ModeAnalysis(Graph, Stages, Scenario, Mode, Passes, ComplianceOverride):
    Before = [Row for Row in Stages if Row.get("Phase") == "BeforeIntegration"]
    if len(Before) < 2:
        raise ValueError("Mode trace needs at least two BeforeIntegration states")
    Interval = float(Before[0]["IntegrationDt"])
    if any(abs(float(Row["IntegrationDt"]) - Interval) > 1.0e-12 for Row in Before):
        raise ValueError("Integration interval changes within mode trace")
    if Scenario is not None:
        if abs(float(Scenario["IntervalSeconds"]) - Interval) > 1.0e-7 * Interval:
            raise ValueError("Spectrum and native trace intervals differ")
        if int(Scenario["ProjectionPasses"]) != Passes:
            raise ValueError("Spectrum projection pass count differs from requested native model")
    Rate = float(Scenario["DampingRatePerSecond"]) if Scenario is not None else 2.4
    Damping = 1.0 - Rate * Interval
    Rest = [[float(V) for V in Point] for Point in Graph["Positions"]]
    InvMass = [float(Value) for Value in Graph["InverseMasses"]]
    Mass = Masses(Graph)
    Edges, Mismatch = MakeEdges(Graph, ComplianceOverride)
    Initial = State(Before[0], Rest, Interval)
    Prediction = Initial
    Rows = []
    ContactParticles = 0
    for Index, Stage in enumerate(Before):
        Actual = State(Stage, Rest, Interval)
        if Index:
            Prediction = LinearStep(Prediction, Edges, InvMass, Interval, Damping, Passes)
        ActualNorm = WeightedNorm(*Actual, Mass)
        PredictedNorm = WeightedNorm(*Prediction, Mass)
        XY, Z = Energies(Stage, Mass)
        ContactParticles += sum(bool(P.get("HasContact")) for P in Stage["Particles"])
        Rows.append({"Ordinal": Index, "Frame": int(Stage["Frame"]),
                     "InternalIteration": int(Stage["InternalIteration"]),
                     "ActualStateNorm": ActualNorm,
                     "PredictedStateNorm": PredictedNorm,
                     "TrajectoryResidualNorm": WeightedDistance(Actual, Prediction, Mass),
                     "RelativeTrajectoryResidual": WeightedDistance(Actual, Prediction, Mass) /
                     max(ActualNorm, PredictedNorm, 1.0e-30),
                     "ActualKineticEnergy": XY + Z,
                     "PredictedKineticEnergy": PredictedEnergy(Prediction[1], Mass, Interval),
                     "InPlaneKineticEnergy": XY, "OutOfPlaneKineticEnergy": Z})
    Result = {"BeforeIntegrationCount": len(Before), "IntervalSeconds": Interval,
              "DampingMultiplier": Damping, "ProjectionPasses": Passes,
              "ComplianceOverride": ComplianceOverride,
              "GraphRestLengthMaxMismatchMeters": Mismatch,
              "AnyContactParticleStageCount": ContactParticles,
              "ActualStateNormGeometricMeanAdjacentRatio": GeometricMeanRatio(
                  [Row["ActualStateNorm"] for Row in Rows]),
              "PredictedStateNormGeometricMeanAdjacentRatio": GeometricMeanRatio(
                  [Row["PredictedStateNorm"] for Row in Rows]),
              "ActualKineticEnergyGeometricMeanAdjacentRatio": GeometricMeanRatio(
                  [Row["ActualKineticEnergy"] for Row in Rows]),
              "PredictedKineticEnergyGeometricMeanAdjacentRatio": GeometricMeanRatio(
                  [Row["PredictedKineticEnergy"] for Row in Rows]),
              "FirstStepActualStateNormRatio": (Rows[1]["ActualStateNorm"] /
                                                Rows[0]["ActualStateNorm"] if Rows[0]["ActualStateNorm"] else None),
              "FirstStepPredictedStateNormRatio": (Rows[1]["PredictedStateNorm"] /
                                                   Rows[0]["PredictedStateNorm"] if Rows[0]["PredictedStateNorm"] else None),
              "Rows": Rows}
    if Mode is not None:
        ModeState = ([[float(V) for V in Point] for Point in Mode["PositionDeltaMeters"]],
                     [[Interval * float(V) for V in Point]
                      for Point in Mode["VelocityDeltaMetersPerSecond"]])
        Denominator = WeightedInner(ModeState, ModeState, Mass)
        Scale = WeightedInner(Initial, ModeState, Mass) / Denominator if Denominator else 0.0
        Aligned = ([[Scale * V for V in Point] for Point in ModeState[0]],
                   [[Scale * V for V in Point] for Point in ModeState[1]])
        Root = ComplexNumber(Mode["StepEigenvalue"])
        Result["ReportedMode"] = {
            "StepEigenvalue": Mode["StepEigenvalue"], "StepEigenvalueMagnitude": abs(Root),
            "ExpectedStateEnvelopeRatioPerStep": abs(Root),
            "ExpectedKineticEnvelopeRatioPerStep": abs(Root) ** 2,
            "BestRealModeScaleAtFirstStage": Scale,
            "FirstStageRelativeModeAlignmentResidual": WeightedDistance(Initial, Aligned, Mass) /
            max(WeightedNorm(*Initial, Mass), 1.0e-30),
            "ExpectedEnvelopeKineticEnergyAtLastStage":
                Rows[0]["ActualKineticEnergy"] * abs(Root) ** (2 * (len(Rows) - 1)),
            "Qualification": "The r envelope is conditional on this exact at-rest eigenmode dominating. "
                             "A complex mode has a real two-dimensional invariant span, so a single "
                             "real-vector fit and instantaneous KE need not grow at exactly |r| and |r|^2. "
                             "The full-state recurrence and residuals use the observed first state instead."}
    return Result


def FreeZAnalysis(Stages):
    Selected = [Row for Row in Stages if 0 <= int(Row["Frame"]) <= 7]
    if not Selected:
        return None
    FirstMass = [0.0 if float(P["InvMass"]) == 0 else 1.0 / float(P["InvMass"])
                 for P in Selected[0]["Particles"]]
    Rows = []
    for Index, Stage in enumerate(Selected):
        XY, Z = Energies(Stage, FirstMass)
        Rows.append({"Ordinal": Index, "Frame": int(Stage["Frame"]),
                     "InternalIteration": int(Stage["InternalIteration"]),
                     "Phase": Stage["Phase"], "InPlaneKineticEnergy": XY,
                     "OutOfPlaneKineticEnergy": Z, "TotalKineticEnergy": XY + Z})
    Threshold = max(1.0e-12, 1.0e-10 * Rows[0]["TotalKineticEnergy"])
    Transfer = next((Row for Row in Rows if Row["InPlaneKineticEnergy"] > Threshold), None)
    Exact = next((Row for Row in Rows if Row["InPlaneKineticEnergy"] > 0), None)
    Before = [Row for Row in Rows if Row["Phase"] == "BeforeIntegration"]
    return {"StageCount": len(Rows), "InPlaneThresholdJoules": Threshold,
            "FirstStrictlyPositiveInPlaneStage": Exact,
            "FirstAboveThresholdInPlaneStage": Transfer,
            "BeforeIntegrationInPlaneGeometricMeanAdjacentRatio": GeometricMeanRatio(
                [Row["InPlaneKineticEnergy"] for Row in Before]),
            "BeforeIntegrationOutOfPlaneGeometricMeanAdjacentRatio": GeometricMeanRatio(
                [Row["OutOfPlaneKineticEnergy"] for Row in Before]),
            "Rows": Rows}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--graph", type=Path, required=True)
    Parser.add_argument("--mode-trace", type=Path, required=True)
    Parser.add_argument("--spectrum", type=Path)
    Parser.add_argument("--scenario-index", type=int, default=0)
    Parser.add_argument("--mode-index", type=int, default=0)
    Parser.add_argument("--passes", type=int, default=1)
    Parser.add_argument("--compliance", default="edge",
                        help="edge or uniform nonnegative compliance")
    Parser.add_argument("--free-z-trace", type=Path)
    Parser.add_argument("--last-mode-frame", type=int, default=11)
    Parser.add_argument("--output", type=Path)
    Args = Parser.parse_args()
    try:
        if Args.passes < 1 or Args.passes > 64:
            raise ValueError("passes must be in 1..64")
        Compliance = None if Args.compliance == "edge" else float(Args.compliance)
        if Compliance is not None and (not math.isfinite(Compliance) or Compliance < 0):
            raise ValueError("compliance must be finite and nonnegative")
        Graph = json.loads(Args.graph.read_text(encoding="utf-8"))
        Spectrum = json.loads(Args.spectrum.read_text(encoding="utf-8")) if Args.spectrum else None
        Scenario = Spectrum["Scenarios"][Args.scenario_index] if Spectrum else None
        Mode = Scenario["TopModes"][Args.mode_index] if Scenario else None
        Stages, Contacts = LoadStages(Args.mode_trace, Args.last_mode_frame)
        Result = {"Kind": "ConditionalNativeModeResponse", "Graph": str(Args.graph),
                  "ModeTrace": str(Args.mode_trace), "Spectrum": str(Args.spectrum) if Args.spectrum else None,
                  "ModeTraceDynamicOrProjectionContactRecords": Contacts,
                  "Mode": ModeAnalysis(Graph, Stages, Scenario, Mode, Args.passes, Compliance),
                  "ModelLimits": "At-rest ordered edge Jacobian with zero lambda on each edge application, "
                                 "native damping and velocity reconstruction. Observed first state is the initial "
                                 "condition. Subsequent collision, gravity, nonlinearity, float32 rounding, or "
                                 "changed edge order can invalidate predictions. No claim about energy conservation "
                                 "or causal instability follows from an eigenvalue alone."}
        if Args.free_z_trace:
            FreeStages, FreeContacts = LoadStages(Args.free_z_trace, 7)
            Result["FreeZTrace"] = str(Args.free_z_trace)
            Result["FreeZDynamicOrProjectionContactRecords"] = FreeContacts
            Result["FreeZ"] = FreeZAnalysis(FreeStages)
        Encoded = json.dumps(Result, indent=2, allow_nan=False) + "\n"
        if Args.output:
            Args.output.write_text(Encoded, encoding="utf-8")
        else:
            print(Encoded, end="")
    except (OSError, ValueError, TypeError, KeyError, IndexError, json.JSONDecodeError) as Error:
        print(f"[Physics:NativeModeResponse] {Error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())

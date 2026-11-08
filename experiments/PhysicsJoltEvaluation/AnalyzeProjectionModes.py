#!/usr/bin/env python3
"""Linearized, at-rest XPBD projection/velocity-reconstruction hypothesis probe.

Input JSON uses Positions, InverseMasses, and ordered Edges with I, J,
RestLength, Compliance. By default every edge application starts with zero XPBD
lambda, matching the observed native ApplyEdgeConstraints behavior when edge
sweeps repeat without integration. The historical lambda-accumulating variant is
retained as an explicit option and a separate file. This does not model contact,
native rounding, or rigid/soft coupling. Results are predictions for a native
perturbation probe, not evidence of production solver eigenmodes.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as NumPy


def ParseNumbers(Text, Convert):
    Values = [Convert(Item.strip()) for Item in Text.split(",") if Item.strip()]
    if not Values:
        raise ValueError("At least one comma-separated value is required")
    return Values


def ParseInterval(Text):
    Parts = Text.strip().split("/")
    if len(Parts) > 2 or len(Parts) < 1:
        raise ValueError("Interval must be a positive finite number or fraction")
    Numerator = float(Parts[0])
    Denominator = float(Parts[1]) if len(Parts) == 2 else 1.0
    if Denominator == 0.0:
        raise ValueError("Interval denominator must be nonzero")
    Value = Numerator / Denominator
    if not math.isfinite(Value) or Value <= 0.0:
        raise ValueError("Interval must be a positive finite number or fraction")
    return Value


def LoadFixture(PathName, Axes):
    Raw = PathName.read_bytes()
    Input = json.loads(Raw)
    Positions = NumPy.asarray(Input["Positions"], dtype=NumPy.float64)
    InverseMasses = NumPy.asarray(Input["InverseMasses"], dtype=NumPy.float64)
    if Positions.ndim != 2 or Positions.shape[1] != 3 or len(Positions) != len(InverseMasses):
        raise ValueError("Positions must be N×3 and InverseMasses must have N entries")
    if not NumPy.isfinite(Positions).all() or not NumPy.isfinite(InverseMasses).all():
        raise ValueError("Positions and inverse masses must be finite")
    if (InverseMasses < 0.0).any() or not (InverseMasses > 0.0).any():
        raise ValueError("Inverse masses must be nonnegative with at least one movable vertex")
    if not isinstance(Input["Edges"], list) or not Input["Edges"]:
        raise ValueError("Edges must be a nonempty ordered list")
    Movable = NumPy.flatnonzero(InverseMasses > 0.0)
    AxisIndices = (0, 1) if Axes == "xy" else (0, 1, 2)
    Dof = [(int(Vertex), Axis) for Vertex in Movable for Axis in AxisIndices]
    DofIndex = {Pair: Index for Index, Pair in enumerate(Dof)}
    Edges = []
    MaxRestMismatch = 0.0
    for EdgeIndex, Item in enumerate(Input["Edges"]):
        I, J = int(Item["I"]), int(Item["J"])
        Length, Compliance = float(Item["RestLength"]), float(Item["Compliance"])
        if I == J or min(I, J) < 0 or max(I, J) >= len(Positions):
            raise ValueError(f"Edge {EdgeIndex} has invalid vertex indices")
        if not math.isfinite(Length) or Length <= 0.0 or not math.isfinite(Compliance) or Compliance < 0.0:
            raise ValueError(f"Edge {EdgeIndex} has invalid length or compliance")
        Delta = Positions[I] - Positions[J]
        ActualLength = float(NumPy.linalg.norm(Delta))
        Mismatch = abs(ActualLength - Length)
        MaxRestMismatch = max(MaxRestMismatch, Mismatch)
        if Mismatch > 1.0e-5 * max(1.0, Length):
            raise ValueError(f"Edge {EdgeIndex} is not at its exported rest length: {Mismatch:g} m")
        Direction = Delta / ActualLength
        if Axes == "xy" and abs(float(Direction[2])) > 1.0e-6:
            raise ValueError(f"Edge {EdgeIndex} leaves the XY plane; use --axes xyz")
        Rows, Gradient, WeightedGradient = [], [], []
        for Vertex, Sign in ((I, 1.0), (J, -1.0)):
            for Axis in AxisIndices:
                if (Vertex, Axis) not in DofIndex:
                    continue
                Rows.append(DofIndex[(Vertex, Axis)])
                Component = Sign * float(Direction[Axis])
                Gradient.append(Component)
                WeightedGradient.append(float(InverseMasses[Vertex]) * Component)
        Edges.append({"I": I, "J": J, "Length": Length, "Compliance": Compliance,
                      "Rows": NumPy.asarray(Rows, dtype=NumPy.intp),
                      "Gradient": NumPy.asarray(Gradient, dtype=NumPy.float64),
                      "WeightedGradient": NumPy.asarray(WeightedGradient, dtype=NumPy.float64),
                      "WeightSum": float(InverseMasses[I] + InverseMasses[J])})
    return {"RawSha256": hashlib.sha256(Raw).hexdigest(), "Positions": Positions,
            "InverseMasses": InverseMasses, "Edges": Edges, "Dof": Dof,
            "MaxRestLengthMismatch": MaxRestMismatch}


def ProjectionJacobian(Fixture, Interval, ComplianceOverride, Passes, LambdaModel="native-reset"):
    Dimension = len(Fixture["Dof"])
    Projection = NumPy.eye(Dimension, dtype=NumPy.float64)
    Lambdas = NumPy.zeros((len(Fixture["Edges"]), Dimension), dtype=NumPy.float64)
    for _ in range(Passes):
        for EdgeIndex, Edge in enumerate(Fixture["Edges"]):
            Compliance = Edge["Compliance"] if ComplianceOverride is None else ComplianceOverride
            Alpha = Compliance / (Interval * Interval)
            Denominator = Edge["WeightSum"] + Alpha
            if Denominator == 0.0:
                continue  # Two pins and a hard edge: no movable position can change.
            Rows = Edge["Rows"]
            if not len(Rows):
                continue
            PreviousLambda = Lambdas[EdgeIndex, :] if LambdaModel == "accumulate" else 0.0
            DeltaLambda = -(Edge["Gradient"] @ Projection[Rows, :] +
                            Alpha * PreviousLambda) / Denominator
            Projection[Rows, :] += Edge["WeightedGradient"][:, None] * DeltaLambda[None, :]
            if LambdaModel == "accumulate":
                Lambdas[EdgeIndex, :] += DeltaLambda
    return Projection


def NonlinearProjection(Fixture, Interval, ComplianceOverride, Passes, DofOffset,
                        LambdaModel="native-reset"):
    Positions = Fixture["Positions"].copy()
    for Coordinate, (Vertex, Axis) in zip(DofOffset, Fixture["Dof"]):
        Positions[Vertex, Axis] += Coordinate
    Lambdas = NumPy.zeros(len(Fixture["Edges"]), dtype=NumPy.float64)
    Masses = Fixture["InverseMasses"]
    for _ in range(Passes):
        for EdgeIndex, Edge in enumerate(Fixture["Edges"]):
            I, J = Edge["I"], Edge["J"]
            Delta = Positions[I] - Positions[J]
            Length = float(NumPy.linalg.norm(Delta))
            if Length <= 0.0:
                raise ValueError("Finite-difference perturbation collapsed an edge")
            Direction = Delta / Length
            Compliance = Edge["Compliance"] if ComplianceOverride is None else ComplianceOverride
            Alpha = Compliance / (Interval * Interval)
            Denominator = Edge["WeightSum"] + Alpha
            if Denominator == 0.0:
                continue
            PreviousLambda = Lambdas[EdgeIndex] if LambdaModel == "accumulate" else 0.0
            DeltaLambda = -(Length - Edge["Length"] + Alpha * PreviousLambda) / Denominator
            Positions[I] += Masses[I] * Direction * DeltaLambda
            Positions[J] -= Masses[J] * Direction * DeltaLambda
            if LambdaModel == "accumulate":
                Lambdas[EdgeIndex] += DeltaLambda
    return NumPy.asarray([Positions[Vertex, Axis] for Vertex, Axis in Fixture["Dof"]])


def CheckFiniteDifference(Fixture, Projection, Interval, ComplianceOverride, Passes,
                          Trials, Epsilon, LambdaModel="native-reset"):
    Generator = NumPy.random.default_rng(772019)
    Errors = []
    for _ in range(Trials):
        Direction = Generator.standard_normal(len(Fixture["Dof"]))
        Direction /= NumPy.linalg.norm(Direction)
        Positive = NonlinearProjection(Fixture, Interval, ComplianceOverride, Passes,
                                       Epsilon * Direction, LambdaModel)
        Negative = NonlinearProjection(Fixture, Interval, ComplianceOverride, Passes,
                                       -Epsilon * Direction, LambdaModel)
        Actual = (Positive - Negative) / (2.0 * Epsilon)
        Expected = Projection @ Direction
        Errors.append(float(NumPy.linalg.norm(Actual - Expected) /
                            max(NumPy.linalg.norm(Expected), 1.0e-12)))
    return {"Trials": Trials, "EpsilonMeters": Epsilon, "MaxRelativeError": max(Errors)}


def Complex(Value):
    return {"Real": float(NumPy.real(Value)), "Imaginary": float(NumPy.imag(Value))}


def ModeVectors(Fixture, ProjectionVector, ProjectionEigenvalue, StepEigenvalue,
                Damping, Interval, ScaleMeters):
    if abs(Damping * ProjectionEigenvalue) < 1.0e-14:
        ScaledVelocity = NumPy.zeros_like(ProjectionVector)
    else:
        ScaledVelocity = ((StepEigenvalue - ProjectionEigenvalue) /
                          (Damping * ProjectionEigenvalue)) * ProjectionVector
    Rotation = NumPy.exp(-0.5j * NumPy.angle(NumPy.sum(ProjectionVector * ProjectionVector)))
    Position = NumPy.real(Rotation * ProjectionVector)
    VelocityTimesInterval = NumPy.real(Rotation * ScaledVelocity)
    if NumPy.linalg.norm(Position) < NumPy.linalg.norm(NumPy.imag(Rotation * ProjectionVector)):
        Position = NumPy.imag(Rotation * ProjectionVector)
        VelocityTimesInterval = NumPy.imag(Rotation * ScaledVelocity)
    VertexCount = len(Fixture["Positions"])
    PositionDelta = NumPy.zeros((VertexCount, 3), dtype=NumPy.float64)
    VelocityDelta = NumPy.zeros((VertexCount, 3), dtype=NumPy.float64)
    for Index, (Vertex, Axis) in enumerate(Fixture["Dof"]):
        PositionDelta[Vertex, Axis] = Position[Index]
        VelocityDelta[Vertex, Axis] = VelocityTimesInterval[Index] / Interval
    MaximumPosition = float(NumPy.max(NumPy.linalg.norm(PositionDelta, axis=1)))
    if MaximumPosition <= 0.0:
        raise ValueError("Mode has no real position component")
    PositionDelta *= ScaleMeters / MaximumPosition
    VelocityDelta *= ScaleMeters / MaximumPosition
    return {"PositionDeltaMeters": PositionDelta.tolist(),
            "VelocityDeltaMetersPerSecond": VelocityDelta.tolist(),
            "MaximumPositionDeltaMeters": ScaleMeters}


def Analyze(Fixture, Interval, ComplianceOverride, Passes, DampingRate,
            TopCount, ScaleMeters, FiniteDifferenceTrials, FiniteDifferenceEpsilon,
            LambdaModel="native-reset"):
    Damping = 1.0 - DampingRate * Interval
    if Damping < 0.0:
        raise ValueError("Damping multiplier became negative")
    Projection = ProjectionJacobian(Fixture, Interval, ComplianceOverride, Passes, LambdaModel)
    Eigenvalues, Eigenvectors = NumPy.linalg.eig(Projection)
    Roots = []
    for Index, Eigenvalue in enumerate(Eigenvalues):
        Discriminant = ((1.0 + Damping) * Eigenvalue) ** 2 - 4.0 * Damping * Eigenvalue
        Root = NumPy.sqrt(complex(Discriminant))
        for Sign, StepEigenvalue in ((1, ((1.0 + Damping) * Eigenvalue + Root) / 2.0),
                                     (-1, ((1.0 + Damping) * Eigenvalue - Root) / 2.0)):
            Roots.append((abs(StepEigenvalue), Index, Sign, StepEigenvalue))
    Roots.sort(key=lambda Item: Item[0], reverse=True)
    Modes = []
    for Magnitude, Index, Sign, StepEigenvalue in Roots[:TopCount]:
        Mode = {"ProjectionEigenvalue": Complex(Eigenvalues[Index]),
                "StepEigenvalue": Complex(StepEigenvalue),
                "StepEigenvalueMagnitude": float(Magnitude),
                "GrowthMagnitudePerOneSixtiethSecond": float(Magnitude ** ((1.0 / 60.0) / Interval)),
                "ProjectionEigenIndex": Index, "QuadraticRootSign": Sign}
        Mode.update(ModeVectors(Fixture, Eigenvectors[:, Index], Eigenvalues[Index],
                                StepEigenvalue, Damping, Interval, ScaleMeters))
        Modes.append(Mode)
    Result = {"IntervalSeconds": Interval, "ProjectionPasses": Passes,
              "LambdaModel": LambdaModel,
              "ComplianceOverride": "edge" if ComplianceOverride is None else ComplianceOverride,
              "DampingRatePerSecond": DampingRate, "DampingMultiplier": Damping,
              "ProjectionSpectralRadius": float(max(abs(Eigenvalues))),
              "UpdateSpectralRadius": float(Roots[0][0]),
              "LinearizedGrowthAboveOnePlus1eMinus8": bool(Roots[0][0] > 1.0 + 1.0e-8),
              "TopModes": Modes}
    if FiniteDifferenceTrials:
        Result["FiniteDifference"] = CheckFiniteDifference(
            Fixture, Projection, Interval, ComplianceOverride, Passes,
            FiniteDifferenceTrials, FiniteDifferenceEpsilon, LambdaModel)
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("input", type=Path, help="Ordered-edge JSON from the native probe")
    Parser.add_argument("--output", type=Path, help="Write JSON here; default is stdout")
    Parser.add_argument("--axes", choices=("xy", "xyz"), default="xy")
    Parser.add_argument("--intervals", default="1/1440,1/5760")
    Parser.add_argument("--passes", default="1,2,4,8")
    Parser.add_argument("--compliances", default="edge,0,1e-6,1e-5",
                        help="'edge' uses exported per-edge values; numbers override every edge")
    Parser.add_argument("--lambda-model", choices=("native-reset", "accumulate"),
                        default="native-reset",
                        help="Native repeated sweeps reset lambda for every edge application")
    Parser.add_argument("--damping-rate", type=float, default=2.4)
    Parser.add_argument("--top", type=int, default=3)
    Parser.add_argument("--mode-scale", type=float, default=1.0e-4,
                        help="Maximum per-vertex position perturbation in meters")
    Parser.add_argument("--finite-difference", type=int, default=0,
                        help="Number of optional nonlinear directional checks per scenario")
    Parser.add_argument("--finite-difference-epsilon", type=float, default=1.0e-5)
    Args = Parser.parse_args()
    try:
        Intervals = ParseNumbers(Args.intervals, ParseInterval)
        Passes = ParseNumbers(Args.passes, int)
        Compliances = [None if Item.strip() == "edge" else float(Item.strip())
                       for Item in Args.compliances.split(",") if Item.strip()]
        if not Compliances or any(Value is not None and
                                  (not math.isfinite(Value) or Value < 0.0) for Value in Compliances):
            raise ValueError("Compliance overrides must be nonnegative finite values or 'edge'")
        if any(Value < 1 or Value > 64 for Value in Passes):
            raise ValueError("Projection passes must be in 1..64")
        if (not math.isfinite(Args.damping_rate) or Args.damping_rate < 0.0 or
                not math.isfinite(Args.mode_scale) or Args.mode_scale <= 0.0 or
                Args.top < 1 or Args.top > 20 or Args.finite_difference < 0 or
                Args.finite_difference > 20 or
                not math.isfinite(Args.finite_difference_epsilon) or
                Args.finite_difference_epsilon <= 0.0):
            raise ValueError("Damping, mode, or finite-difference option out of bounded range")
        Fixture = LoadFixture(Args.input, Args.axes)
        if (len(Fixture["Dof"]) > 1024 or len(Fixture["Edges"]) > 20000 or
                len(Intervals) * len(Passes) * len(Compliances) > 64):
            raise ValueError("Dense eigenproblem or scenario count exceeds the bounded probe")
        Scenarios = [Analyze(Fixture, Interval, Compliance, Count, Args.damping_rate,
                             Args.top, Args.mode_scale, Args.finite_difference,
                             Args.finite_difference_epsilon, Args.lambda_model)
                     for Interval in Intervals for Compliance in Compliances for Count in Passes]
        Result = {"Kind": "LinearizedAtRestXpbdHypothesis",
                  "InputSha256": Fixture["RawSha256"],
                  "NumPyVersion": NumPy.__version__,
                  "Axes": Args.axes, "VertexCount": len(Fixture["Positions"]),
                  "MovableVertexCount": int(NumPy.count_nonzero(Fixture["InverseMasses"])),
                  "OrderedEdgeCount": len(Fixture["Edges"]),
                  "PositionDofCount": len(Fixture["Dof"]),
                  "MaxRestLengthMismatchMeters": Fixture["MaxRestLengthMismatch"],
                  "LambdaModel": Args.lambda_model,
                  "Formula": "q'=P(q+d*h*v); h*v'=q'-q; T=[[P,dP],[P-I,dP]], d=1-damping_rate*h. For each eigenvalue lambda of P, r^2-(1+d)*lambda*r+d*lambda=0.",
                  "ModelLimits": "Ordered distance-edge projection linearized at exact rest; native-reset uses zero lambda on every edge application, including repeated sweeps. Accumulate is a historical alternative with per-edge lambdas reset at each step and retained within ordered passes. No contact, native float rounding, warm starts, or cross-step rigid/soft coupling. XY requires planar rest edges. Mode vectors are predictions for native injection, not observed instability.",
                  "Scenarios": Scenarios}
        Encoded = json.dumps(Result, indent=2, allow_nan=False)
        if Args.output:
            Args.output.write_text(Encoded + "\n", encoding="utf-8")
        else:
            print(Encoded)
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as Error:
        print(f"[Physics:ProjectionModes] {Error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())

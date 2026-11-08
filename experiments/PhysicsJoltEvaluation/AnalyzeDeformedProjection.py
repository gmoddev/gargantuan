#!/usr/bin/env python3
"""Linearize one ordered, nonlinear Jolt edge sweep at supplied cloth states.

Input: --graph optimized unperturbed native graph JSON; --snapshots JSON list of
{Name, Positions, IntegrationDt, ...}. Rest and deformed snapshots are each
analyzed independently. This is a double-precision local model, not native
float replay or proof that a local eigenmode drives the observed trajectory.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as NumPy


def Complex(Value):
    return {"Real": float(NumPy.real(Value)), "Imaginary": float(NumPy.imag(Value))}


def Load(PathName):
    Raw = PathName.read_bytes()
    return json.loads(Raw), hashlib.sha256(Raw).hexdigest()


def Fixture(Graph):
    Rest = NumPy.asarray(Graph["Positions"], dtype=NumPy.float64)
    InvMass = NumPy.asarray(Graph["InverseMasses"], dtype=NumPy.float64)
    if Rest.ndim != 2 or Rest.shape[1] != 3 or InvMass.shape != (len(Rest),):
        raise ValueError("Graph Positions must be N×3 with N inverse masses")
    if not NumPy.isfinite(Rest).all() or not NumPy.isfinite(InvMass).all() or (InvMass < 0).any():
        raise ValueError("Graph positions or inverse masses are invalid")
    Movable = NumPy.flatnonzero(InvMass > 0)
    if len(Movable) == 0 or 3 * len(Movable) > 600:
        raise ValueError("Movable full-xyz DOF count is zero or exceeds bounded 600")
    Edges = []
    for Index, Edge in enumerate(Graph["Edges"]):
        I, J = int(Edge["I"]), int(Edge["J"])
        R, C = float(Edge["RestLength"]), float(Edge["Compliance"])
        if I == J or min(I, J) < 0 or max(I, J) >= len(Rest):
            raise ValueError(f"Edge {Index} has invalid indices")
        if not math.isfinite(R) or R <= 0 or not math.isfinite(C) or C < 0:
            raise ValueError(f"Edge {Index} has invalid rest length or compliance")
        Edges.append((I, J, R, C))
    if not Edges or len(Edges) > 20000:
        raise ValueError("Graph must have a bounded, nonempty ordered edge list")
    Rows = {int(Vertex): slice(3 * Local, 3 * Local + 3)
            for Local, Vertex in enumerate(Movable)}
    return Rest, InvMass, Edges, Rows


def Sweep(InputPositions, InvMass, Edges, Interval, Rows, Jacobian):
    Positions = InputPositions.copy()
    DofCount = 3 * len(Rows)
    Derivative = None
    if Jacobian:
        Derivative = NumPy.zeros((len(Positions), 3, DofCount), dtype=NumPy.float64)
        for Vertex, Span in Rows.items():
            Derivative[Vertex, :, Span] = NumPy.eye(3)
    Compressed = Stretched = Exact = Skipped = Active = 0
    MinLengthOverRest = math.inf
    MaxHardRestOverLength = -math.inf
    MaxHardEdge = None
    MaxTangentialRelativeGain = -math.inf
    MaxTangentialEdge = None
    MinimumLength = math.inf
    for EdgeIndex, (I, J, RestLength, Compliance) in enumerate(Edges):
        Delta = Positions[J] - Positions[I]
        Length = float(NumPy.linalg.norm(Delta))
        MinimumLength = min(MinimumLength, Length)
        if Length <= 0:
            raise ValueError(f"Edge {EdgeIndex} has zero length in supplied snapshot/sweep")
        Ratio = Length / RestLength
        MinLengthOverRest = min(MinLengthOverRest, Ratio)
        if Ratio < 1.0 - 1.0e-12:
            Compressed += 1
        elif Ratio > 1.0 + 1.0e-12:
            Stretched += 1
        else:
            Exact += 1
        Weight = float(InvMass[I] + InvMass[J])
        Denominator = Weight + Compliance / (Interval * Interval)
        if Length * Denominator < 1.0e-12:
            Skipped += 1
            continue
        Active += 1
        RestOverLength = RestLength / Length
        RelativeTangentGain = 1.0 - Weight / Denominator * (1.0 - RestOverLength)
        if RelativeTangentGain > MaxTangentialRelativeGain:
            MaxTangentialRelativeGain = RelativeTangentGain
            MaxTangentialEdge = {"Index": EdgeIndex, "I": I, "J": J,
                                 "LengthMeters": Length, "RestLengthMeters": RestLength,
                                 "Compliance": Compliance, "Gain": RelativeTangentGain}
        if Compliance == 0 and RestOverLength > MaxHardRestOverLength:
            MaxHardRestOverLength = RestOverLength
            MaxHardEdge = {"Index": EdgeIndex, "I": I, "J": J,
                           "LengthMeters": Length, "RestLengthMeters": RestLength,
                           "RestOverLength": RestOverLength}
        Normal = Delta / Length
        Correction = Delta * ((1.0 - RestOverLength) / Denominator)
        if Jacobian:
            # dc/d(delta) where c=delta*(1-r/L)/D, including nonzero residual.
            A = ((1.0 - RestOverLength) * NumPy.eye(3) +
                 RestOverLength * NumPy.outer(Normal, Normal)) / Denominator
            Change = A @ (Derivative[J] - Derivative[I])
            OldI = Derivative[I].copy()
            OldJ = Derivative[J].copy()
            Derivative[I] = OldI + InvMass[I] * Change
            Derivative[J] = OldJ - InvMass[J] * Change
        Positions[I] += InvMass[I] * Correction
        Positions[J] -= InvMass[J] * Correction
    Projection = None
    if Jacobian:
        Projection = NumPy.vstack([Derivative[Vertex] for Vertex in Rows])
    Metrics = {"EdgeCount": len(Edges), "ActiveEdgeApplications": Active,
               "SkippedEdgeApplications": Skipped, "CompressedEdgeApplications": Compressed,
               "StretchedEdgeApplications": Stretched, "NearRestEdgeApplications": Exact,
               "MinimumLengthMeters": MinimumLength,
               "MinimumLengthOverRest": MinLengthOverRest,
               "MaxHardRestOverLength": MaxHardRestOverLength if MaxHardEdge else None,
               "MaxHardRestOverLengthEdge": MaxHardEdge,
               "MaxTangentialRelativeGain": MaxTangentialRelativeGain,
               "MaxTangentialRelativeGainEdge": MaxTangentialEdge}
    return Positions, Projection, Metrics


def FiniteDifferenceCheck(Positions, Projection, InvMass, Edges, Interval, Rows, Trials, Epsilon):
    Generator = NumPy.random.default_rng(772019)
    Errors = []
    for _ in range(Trials):
        Direction = Generator.standard_normal(Projection.shape[1])
        Direction /= NumPy.linalg.norm(Direction)
        Positive = Positions.copy()
        Negative = Positions.copy()
        for Vertex, Span in Rows.items():
            Positive[Vertex] += Epsilon * Direction[Span]
            Negative[Vertex] -= Epsilon * Direction[Span]
        OutputP = Sweep(Positive, InvMass, Edges, Interval, Rows, False)[0]
        OutputN = Sweep(Negative, InvMass, Edges, Interval, Rows, False)[0]
        Numeric = NumPy.concatenate([(OutputP[Vertex] - OutputN[Vertex]) / (2 * Epsilon)
                                     for Vertex in Rows])
        Analytic = Projection @ Direction
        Errors.append(float(NumPy.linalg.norm(Numeric - Analytic) /
                            max(NumPy.linalg.norm(Analytic), 1.0e-12)))
    return {"Trials": Trials, "EpsilonMeters": Epsilon,
            "MaximumRelativeDirectionalError": max(Errors) if Errors else None,
            "Errors": Errors}


def AnalyzeSnapshot(Snapshot, Rest, InvMass, Edges, Rows, Rate, Trials, Epsilon, Svd):
    Name = str(Snapshot["Name"])
    Positions = NumPy.asarray(Snapshot["Positions"], dtype=NumPy.float64)
    Interval = float(Snapshot["IntegrationDt"])
    if Positions.shape != Rest.shape or not NumPy.isfinite(Positions).all():
        raise ValueError(f"{Name}: positions have wrong shape or nonfinite values")
    if not math.isfinite(Interval) or Interval <= 0:
        raise ValueError(f"{Name}: invalid integration interval")
    Damping = max(0.0, 1.0 - Rate * Interval)
    End, Projection, Metrics = Sweep(Positions, InvMass, Edges, Interval, Rows, True)
    Eigenvalues = NumPy.linalg.eigvals(Projection)
    Discriminant = ((1.0 + Damping) * Eigenvalues) ** 2 - 4.0 * Damping * Eigenvalues
    SquareRoot = NumPy.sqrt(Discriminant.astype(NumPy.complex128))
    Plus = ((1.0 + Damping) * Eigenvalues + SquareRoot) / 2.0
    Minus = ((1.0 + Damping) * Eigenvalues - SquareRoot) / 2.0
    Roots = NumPy.concatenate((Plus, Minus))
    BestP = Eigenvalues[int(NumPy.argmax(NumPy.abs(Eigenvalues)))]
    BestT = Roots[int(NumPy.argmax(NumPy.abs(Roots)))]
    Result = {"Name": Name, "IntegrationDt": Interval, "DampingMultiplier": Damping,
              "PositionDofCount": Projection.shape[0],
              "MaximumInputDisplacementFromRestMeters": float(NumPy.max(NumPy.linalg.norm(Positions - Rest, axis=1))),
              "MaximumSweepPositionCorrectionMeters": float(NumPy.max(NumPy.linalg.norm(End - Positions, axis=1))),
              "EdgeSweep": Metrics,
              "ProjectionSpectralRadius": float(NumPy.max(NumPy.abs(Eigenvalues))),
              "LargestMagnitudeProjectionEigenvalue": Complex(BestP),
              "UpdateSpectralRadius": float(NumPy.max(NumPy.abs(Roots))),
              "LargestMagnitudeUpdateRoot": Complex(BestT),
              "RootsAboveOnePlus1eMinus8": int(NumPy.count_nonzero(NumPy.abs(Roots) > 1.0 + 1.0e-8)),
              "PositiveGrowthAtThisSnapshot": bool(NumPy.max(NumPy.abs(Roots)) > 1.0 + 1.0e-8),
              "TopUpdateRoots": [Complex(Roots[Index]) | {"Magnitude": float(abs(Roots[Index]))}
                                  for Index in NumPy.argsort(NumPy.abs(Roots))[-8:][::-1]],
              "FiniteDifference": FiniteDifferenceCheck(Positions, Projection, InvMass, Edges,
                                                        Interval, Rows, Trials, Epsilon)}
    if Svd:
        SingularValues = NumPy.linalg.svd(Projection, compute_uv=False)
        Result["ProjectionLargestSingularValueEuclidean"] = float(SingularValues[0])
    for Key, Value in Snapshot.items():
        if Key not in ("Name", "Positions", "IntegrationDt"):
            Result.setdefault("SnapshotMetadata", {})[Key] = Value
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--graph", type=Path, required=True)
    Parser.add_argument("--snapshots", type=Path, required=True)
    Parser.add_argument("--output", type=Path)
    Parser.add_argument("--damping-rate", type=float, default=2.4000000953674316)
    Parser.add_argument("--finite-difference", type=int, default=2)
    Parser.add_argument("--epsilon", type=float, default=1.0e-5)
    Parser.add_argument("--svd", action="store_true")
    Args = Parser.parse_args()
    try:
        if (not math.isfinite(Args.damping_rate) or Args.damping_rate < 0 or
                Args.finite_difference < 0 or Args.finite_difference > 10 or
                not math.isfinite(Args.epsilon) or Args.epsilon <= 0):
            raise ValueError("Invalid bounded rate or finite-difference options")
        Graph, GraphSha = Load(Args.graph)
        Snapshots, SnapSha = Load(Args.snapshots)
        if isinstance(Snapshots, dict):
            Snapshots = Snapshots["Snapshots"]
        if not isinstance(Snapshots, list) or not Snapshots or len(Snapshots) > 16:
            raise ValueError("Snapshots must be a list of 1..16 records")
        Rest, InvMass, Edges, Rows = Fixture(Graph)
        Results = [AnalyzeSnapshot(Item, Rest, InvMass, Edges, Rows, Args.damping_rate,
                                   Args.finite_difference, Args.epsilon, Args.svd)
                   for Item in Snapshots]
        Output = {"Kind": "LocalDeformedOrderedEdgeJacobian", "GraphSha256": GraphSha,
                  "SnapshotsSha256": SnapSha, "Graph": str(Args.graph),
                  "Snapshots": str(Args.snapshots), "VertexCount": len(Rest),
                  "MovableVertexCount": len(Rows), "OrderedEdgeCount": len(Edges),
                  "DampingRatePerSecond": Args.damping_rate,
                  "Formula": "At each current edge delta=xj-xi, L=|delta|, n=delta/L, D=wi+wj+c/h^2, cvec=delta*(1-r/L)/D. A=dcvec/ddelta=((1-r/L)I+(r/L)nn^T)/D. Ji+=wi*A*(Jj-Ji), Jj-=wj*A*(Jj-Ji) using old blocks. For P=DF, T=[[P,dP],[P-I,dP]] in (q,hv); r^2-(1+d)lambda*r+d*lambda=0.",
                  "Limits": "Local derivative at supplied post-integration positions, propagating through one double-precision ordered nonlinear edge sweep. No contact, rigid coupling, native float rounding, edge-group races, or proof that an eigenmode persists along the nonautonomous native trajectory. A local spectral radius above one is only a candidate instability mechanism; below one does not exclude transient nonnormal gain or later deformed states.",
                  "Results": Results}
        Encoded = json.dumps(Output, indent=2, allow_nan=False) + "\n"
        if Args.output:
            Args.output.write_text(Encoded, encoding="utf-8")
        else:
            print(Encoded, end="")
    except (OSError, ValueError, TypeError, KeyError, IndexError, json.JSONDecodeError) as Error:
        print(f"[Physics:DeformedProjection] {Error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())

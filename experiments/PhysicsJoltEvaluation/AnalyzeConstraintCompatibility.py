#!/usr/bin/env python3
"""Test second-order compatibility of a seeded velocity in a flat hard-edge cloth.

For each hard distance edge at geometric rest, J v = n·(v_j-v_i) and
J a = -b, where b=(|v_j-v_i|²-(n·(v_j-v_i))²)/length. A nonzero
left-null residual of this acceleration system is a local impossibility
witness for a smooth exact hard-constraint trajectory with the chosen velocity.
This is a kinematic test, not a finite-time simulator or a material ranking.
"""

from __future__ import annotations

import argparse
import itertools
import json
import math
from pathlib import Path

import numpy as np


QUAD = (83, 84, 96, 97)


def LoadGraph(PathValue: Path) -> dict:
    Graph = json.loads(PathValue.read_text(encoding="utf-8"))
    Positions = np.asarray(Graph["Positions"], dtype=np.float64)
    InverseMasses = np.asarray(Graph["InverseMasses"], dtype=np.float64)
    Edges = Graph["Edges"]
    if (Positions.ndim != 2 or Positions.shape[1] != 3 or
            not 4 <= len(Positions) <= 4096 or
            InverseMasses.shape != (len(Positions),) or
            not 1 <= len(Edges) <= 20000 or
            not np.isfinite(Positions).all() or
            not np.isfinite(InverseMasses).all() or
            np.any(InverseMasses < 0)):
        raise ValueError("[Physics:Compatibility] Invalid or oversized graph")
    return {"Positions": Positions, "InverseMasses": InverseMasses, "Edges": Edges}


def HardEdgeData(Graph: dict) -> tuple[list[tuple[int, int, np.ndarray, float]], set[tuple[int, int]]]:
    Positions = Graph["Positions"]
    Result = []
    Pairs = set()
    for Entry in Graph["Edges"]:
        I, K = int(Entry["I"]), int(Entry["J"])
        Compliance = float(Entry["Compliance"])
        if (I < 0 or K < 0 or I >= len(Positions) or K >= len(Positions) or
                I == K or not math.isfinite(Compliance) or Compliance < 0):
            raise ValueError("[Physics:Compatibility] Invalid edge")
        if Compliance != 0.0:
            continue
        Difference = Positions[K] - Positions[I]
        Length = float(np.linalg.norm(Difference))
        if not math.isfinite(Length) or Length <= 0:
            raise ValueError("[Physics:Compatibility] Degenerate hard edge")
        Result.append((I, K, Difference / Length, Length))
        Pairs.add(tuple(sorted((I, K))))
    return Result, Pairs


def QuadWitness(Graph: dict, Pairs: set[tuple[int, int]], SeedIndex: int,
                Velocity: np.ndarray) -> dict:
    Positions = Graph["Positions"]
    if max(QUAD) >= len(Positions):
        return {"Vertices": QUAD, "Present": False, "Reason": "Graph too small"}
    Missing = [Pair for Pair in itertools.combinations(QUAD, 2)
               if tuple(sorted(Pair)) not in Pairs]
    Others = [Index for Index in QUAD if Index != SeedIndex]
    if len(Others) != 3:
        return {"Vertices": QUAD, "Present": not Missing,
                "MissingHardPairs": Missing, "Reason": "Seed outside selected quad"}
    A, B, C = (Positions[Index] for Index in Others)
    TriangleCross = np.cross(B - A, C - A)
    SignedTetraVolumeDerivative = float(np.dot(
        TriangleCross, Velocity[SeedIndex] - Velocity[Others[0]]) / 6.0)
    return {"Vertices": QUAD, "AllSixHardPairs": len(Missing) == 0,
            "MissingHardPairs": Missing,
            "OtherThreeTriangleDoubleArea": float(np.linalg.norm(TriangleCross)),
            "SignedTetraVolumeDerivative": SignedTetraVolumeDerivative,
            "Interpretation": (
                "All six pair distances fix tetrahedral volume. At a flat rest quad it is zero; "
                "the nonzero first derivative from the seeded vertex forbids a smooth exact "
                "hard-edge path with this initial velocity."
                if not Missing and abs(SignedTetraVolumeDerivative) > 0 else
                "No complete six-hard-edge local impossibility witness in this quad.")}


def Analyze(PathValue: Path, SeedIndex: int, SeedSpeed: float) -> dict:
    Graph = LoadGraph(PathValue)
    Positions = Graph["Positions"]
    InverseMasses = Graph["InverseMasses"]
    if SeedIndex >= len(Positions) or InverseMasses[SeedIndex] <= 0:
        raise ValueError("[Physics:Compatibility] Seed vertex must be movable")
    Velocity = np.zeros_like(Positions)
    Velocity[SeedIndex, 2] = SeedSpeed
    HardEdges, Pairs = HardEdgeData(Graph)
    Result = {"Graph": str(PathValue), "VertexCount": len(Positions),
              "EdgeCount": len(Graph["Edges"]), "HardEdgeCount": len(HardEdges),
              "SeedIndex": SeedIndex, "SeedVelocityZ": SeedSpeed,
              "GeometricRestConvention": "Unit gradients and second derivatives use the actual position lengths, equivalent to recomputed rest; captured length rounding is excluded.",
              "QuadWitness": QuadWitness(Graph, Pairs, SeedIndex, Velocity)}
    if not HardEdges:
        Result["Conclusion"] = "No exact hard-edge acceleration conditions in this graph."
        return Result
    Movable = np.flatnonzero(InverseMasses > 0)
    Columns = {int(Index): 3 * Offset for Offset, Index in enumerate(Movable)}
    Jacobian = np.zeros((len(HardEdges), 3 * len(Movable)), dtype=np.float64)
    Curvature = np.zeros(len(HardEdges), dtype=np.float64)
    FirstDerivative = np.zeros(len(HardEdges), dtype=np.float64)
    for Row, (I, K, Normal, Length) in enumerate(HardEdges):
        if I in Columns:
            Jacobian[Row, Columns[I]:Columns[I] + 3] = -Normal
        if K in Columns:
            Jacobian[Row, Columns[K]:Columns[K] + 3] = Normal
        RelativeVelocity = Velocity[K] - Velocity[I]
        NormalSpeed = float(np.dot(Normal, RelativeVelocity))
        FirstDerivative[Row] = NormalSpeed
        Curvature[Row] = (float(np.dot(RelativeVelocity, RelativeVelocity)) -
                          NormalSpeed * NormalSpeed) / Length
    # Flat rest XY edges have identically zero Z-gradient columns. Drop them
    # before SVD, preserving the same least-squares residual at lower cost.
    ActiveColumns = np.flatnonzero(np.any(Jacobian != 0, axis=0))
    Reduced = Jacobian[:, ActiveColumns]
    Acceleration, _, Rank, SingularValues = np.linalg.lstsq(
        Reduced, -Curvature, rcond=None)
    Residual = Reduced @ Acceleration + Curvature
    CurvatureNorm = float(np.linalg.norm(Curvature))
    ResidualNorm = float(np.linalg.norm(Residual))
    DualNorm = float(np.linalg.norm(Reduced.T @ Residual))
    MatrixNorm = float(SingularValues[0]) if len(SingularValues) else 0.0
    ResidualDotCurvature = float(np.dot(Residual, Curvature))
    LeftNullRelative = DualNorm / (MatrixNorm * ResidualNorm) if MatrixNorm and ResidualNorm else 0.0
    Result.update({
        "MovableVertices": len(Movable), "ActiveJacobianColumns": len(ActiveColumns),
        "JacobianRank": int(Rank),
        "SmallestRetainedSingularValue": float(SingularValues[Rank - 1]) if Rank else None,
        "FirstDerivativeNorm": float(np.linalg.norm(FirstDerivative)),
        "FirstDerivativeMaximum": float(np.max(np.abs(FirstDerivative))),
        "CurvatureRhsNorm": CurvatureNorm,
        "AccelerationLeastSquaresResidualNorm": ResidualNorm,
        "AccelerationRelativeResidual": ResidualNorm / CurvatureNorm if CurvatureNorm else 0.0,
        "LeftNullWitnessTransposeNorm": DualNorm,
        "LeftNullWitnessRelativeDualNorm": LeftNullRelative,
        "LeftNullWitnessDotCurvature": ResidualDotCurvature,
        "LeftNullWitnessNormSquared": ResidualNorm * ResidualNorm,
        "Conclusion": (
            "Second-order incompatible to numerical tolerance: no acceleration can satisfy "
            "all exact hard-edge distances with this initial velocity."
            if CurvatureNorm > 0 and ResidualNorm / CurvatureNorm > 1.0e-8 and
               LeftNullRelative < 1.0e-8 else
            "No second-order incompatibility established at the selected numerical tolerance."
        )})
    return Result


def Main() -> int:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--graph", required=True, type=Path)
    Parser.add_argument("--single-diagonal", type=Path)
    Parser.add_argument("--no-diagonals", type=Path)
    Parser.add_argument("--compliant-graph", type=Path)
    Parser.add_argument("--seed-index", type=int, default=84)
    Parser.add_argument("--seed-z", type=float, default=1.0)
    Parser.add_argument("--output", type=Path)
    Args = Parser.parse_args()
    if Args.SeedIndex < 0 or not math.isfinite(Args.SeedZ) or abs(Args.SeedZ) > 100:
        Parser.error("[Physics:Compatibility] Seed outside bounded range")
    Paths = {"HardCloth": Args.Graph, "SingleDiagonal": Args.SingleDiagonal,
             "NoDiagonals": Args.NoDiagonals, "CompliantCloth": Args.CompliantGraph}
    Result = {Name: Analyze(PathValue, Args.SeedIndex, Args.SeedZ)
              for Name, PathValue in Paths.items() if PathValue is not None}
    Text = json.dumps(Result, indent=2, allow_nan=False) + "\n"
    if Args.Output:
        Args.Output.parent.mkdir(parents=True, exist_ok=True)
        Args.Output.write_text(Text, encoding="utf-8")
    else:
        print(Text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())

#!/usr/bin/env python3
"""Bounded nonlinear replay of exported ordered Jolt distance-edge sweeps.

Each edge application uses zero XPBD lambda, including repeated sweeps. The
replay starts at exported rest positions plus a seeded perturbation, predicts
with damped velocity, projects the ordered graph, and reconstructs velocity.
It reports candidate kinetic energy after prediction and every sweep, plus a
spring-analogy edge estimate. This is an external numerical model, not native
Jolt execution or a complete mechanical-energy account.
"""

import argparse
import json
import math
from pathlib import Path
import sys

import numpy as NumPy

import AnalyzeProjectionModes as ProjectionModes


def ParseMode(PathName, ScenarioIndex, ModeIndex, VertexCount):
    Data = json.loads(PathName.read_text(encoding="utf-8"))
    if "Scenarios" in Data:
        Data = Data["Scenarios"][ScenarioIndex]["TopModes"][ModeIndex]
    Position = NumPy.asarray(Data.get("PositionDeltaMeters", Data.get("PositionDelta")),
                             dtype=NumPy.float64)
    Velocity = NumPy.asarray(Data.get("VelocityDeltaMetersPerSecond",
                                      Data.get("VelocityDelta", NumPy.zeros((VertexCount, 3)))),
                             dtype=NumPy.float64)
    if Position.shape != (VertexCount, 3) or Velocity.shape != (VertexCount, 3):
        raise ValueError("Mode position/velocity arrays must each have N×3 entries")
    if not NumPy.isfinite(Position).all() or not NumPy.isfinite(Velocity).all():
        raise ValueError("Mode position/velocity entries must be finite")
    return Position, Velocity


def Observe(Rest, Positions, Velocity, InverseMasses, Edges, ComplianceOverride,
            Step, Stage, Correction=None):
    Finite = bool(NumPy.isfinite(Positions).all() and NumPy.isfinite(Velocity).all())
    Result = {"Step": Step, "Stage": Stage, "Finite": Finite}
    if not Finite:
        return Result
    Movable = InverseMasses > 0.0
    Masses = 1.0 / InverseMasses[Movable].astype(NumPy.float64)
    Speeds = Velocity[Movable].astype(NumPy.float64)
    Kinetic = float(0.5 * NumPy.sum(Masses[:, None] * Speeds * Speeds))
    Momentum = NumPy.sum(Masses[:, None] * Speeds, axis=0)
    Elastic = 0.0
    HardEdges = 0
    MaxStrain = 0.0
    MaxConstraintError = 0.0
    for Edge in Edges:
        Length = float(NumPy.linalg.norm(Positions[Edge["I"]].astype(NumPy.float64) -
                                         Positions[Edge["J"]].astype(NumPy.float64)))
        Error = Length - Edge["Length"]
        MaxStrain = max(MaxStrain, abs(Error) / Edge["Length"])
        MaxConstraintError = max(MaxConstraintError, abs(Error))
        Compliance = Edge["Compliance"] if ComplianceOverride is None else ComplianceOverride
        if Compliance > 0.0:
            Elastic += Error * Error / (2.0 * Compliance)
        else:
            HardEdges += 1
    MaxRestOffset = float(NumPy.max(NumPy.linalg.norm(
        Positions.astype(NumPy.float64) - Rest.astype(NumPy.float64), axis=1)))
    MaxPinnedDrift = (float(NumPy.max(NumPy.linalg.norm(
        Positions[~Movable].astype(NumPy.float64) -
        Rest[~Movable].astype(NumPy.float64), axis=1))) if (~Movable).any() else 0.0)
    TotalEstimate = Kinetic + Elastic
    if not all(math.isfinite(Value) for Value in
               (Kinetic, Elastic, TotalEstimate, MaxStrain, MaxConstraintError, MaxRestOffset,
                MaxPinnedDrift, *Momentum)):
        return {"Step": Step, "Stage": Stage, "Finite": False,
                "Reason": "Energy, momentum, or distance observation overflowed"}
    Result.update({"KineticEnergy": Kinetic, "CompliantEdgeElasticEstimate": Elastic,
                   "KineticPlusElasticEstimate": TotalEstimate,
                   "MaxEdgeStrain": MaxStrain, "MaxConstraintErrorMeters": MaxConstraintError,
                   "HardEdgesOmittedFromElasticEstimate": HardEdges,
                   "MaxRestOffsetMeters": MaxRestOffset,
                   "MaxPinnedDriftMeters": MaxPinnedDrift,
                   "Momentum": Momentum.tolist()})
    if Correction is not None:
        Norms = NumPy.linalg.norm(Correction.astype(NumPy.float64), axis=1)
        MaximumCorrection = float(NumPy.max(Norms))
        SumCorrection = float(NumPy.sum(Norms))
        if not math.isfinite(MaximumCorrection) or not math.isfinite(SumCorrection):
            return {"Step": Step, "Stage": Stage, "Finite": False,
                    "Reason": "Sweep correction observation overflowed"}
        Result["MaxSweepCorrectionMeters"] = MaximumCorrection
        Result["SumSweepCorrectionMeters"] = SumCorrection
    return Result


def ProjectSweep(Positions, InverseMasses, Edges, ComplianceOverride, Interval, Scalar):
    ForStep = Scalar(Interval)
    for Edge in Edges:
        I, J = Edge["I"], Edge["J"]
        Delta = Positions[I] - Positions[J]
        Length = Scalar(NumPy.linalg.norm(Delta))
        if not NumPy.isfinite(Length) or Length <= 0:
            return False
        Direction = Delta / Length
        Compliance = Edge["Compliance"] if ComplianceOverride is None else ComplianceOverride
        Alpha = Scalar(Compliance) / (ForStep * ForStep)
        Denominator = InverseMasses[I] + InverseMasses[J] + Alpha
        if Denominator == 0:
            continue
        Correction = Scalar(-(Length - Scalar(Edge["Length"])) / Denominator)
        Positions[I] += InverseMasses[I] * Direction * Correction
        Positions[J] -= InverseMasses[J] * Direction * Correction
    return True


def Replay(Fixture, Seed, Precision, Interval, Sweeps, Steps, DampingRate,
           ComplianceOverride, CenterIndex, CenterAmplitude, Mode):
    DType = NumPy.float32 if Precision == "float32" else NumPy.float64
    Scalar = DType
    Rest = Fixture["Positions"].astype(DType)
    Masses = Fixture["InverseMasses"].astype(DType)
    Positions = Rest.copy()
    Velocity = NumPy.zeros_like(Rest)
    if Seed.startswith("center-"):
        Axis = {"center-x": 0, "center-y": 1, "center-z": 2}[Seed]
        Positions[CenterIndex, Axis] += Scalar(CenterAmplitude)
    else:
        Positions += Mode[0].astype(DType)
        Velocity += Mode[1].astype(DType)
    if (not NumPy.array_equal(Positions[Masses == 0], Rest[Masses == 0]) or
            not NumPy.array_equal(Velocity[Masses == 0], NumPy.zeros_like(Velocity[Masses == 0]))):
        raise ValueError("Perturbation must not move pinned vertices")
    Damping = Scalar(1.0 - DampingRate * Interval)
    StepInterval = Scalar(Interval)
    Stages = []
    for Step in range(Steps):
        Previous = Positions.copy()
        Stages.append(Observe(Rest, Positions, Velocity, Masses, Fixture["Edges"],
                              ComplianceOverride, Step, "before-predict"))
        Positions += Damping * StepInterval * Velocity
        CandidateVelocity = (Positions - Previous) / StepInterval
        Stages.append(Observe(Rest, Positions, CandidateVelocity, Masses, Fixture["Edges"],
                              ComplianceOverride, Step, "after-predict"))
        if not Stages[-1]["Finite"]:
            break
        for Sweep in range(1, Sweeps + 1):
            BeforeSweep = Positions.copy()
            if not ProjectSweep(Positions, Masses, Fixture["Edges"],
                                ComplianceOverride, Interval, Scalar):
                Stages.append({"Step": Step, "Stage": f"after-sweep-{Sweep}", "Finite": False})
                break
            CandidateVelocity = (Positions - Previous) / StepInterval
            Stages.append(Observe(Rest, Positions, CandidateVelocity, Masses, Fixture["Edges"],
                                  ComplianceOverride, Step, f"after-sweep-{Sweep}",
                                  Positions - BeforeSweep))
            if not Stages[-1]["Finite"]:
                break
        if not Stages[-1]["Finite"]:
            break
        Velocity = (Positions - Previous) / StepInterval
        Stages.append(Observe(Rest, Positions, Velocity, Masses, Fixture["Edges"],
                              ComplianceOverride, Step, "accepted"))
        if not Stages[-1]["Finite"]:
            break
    return {"Seed": Seed, "Precision": Precision, "CenterVertex": CenterIndex,
            "CompletedSteps": sum(Item["Stage"] == "accepted" for Item in Stages),
            "Stages": Stages,
            "FinalPositions": Positions.astype(NumPy.float64).tolist() if
                NumPy.isfinite(Positions).all() else None,
            "FinalVelocities": Velocity.astype(NumPy.float64).tolist() if
                NumPy.isfinite(Velocity).all() else None}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("input", type=Path)
    Parser.add_argument("--output", type=Path)
    Parser.add_argument("--interval", default="1/1440")
    Parser.add_argument("--sweeps", type=int, default=8)
    Parser.add_argument("--steps", type=int, default=10)
    Parser.add_argument("--damping-rate", type=float, default=2.4)
    Parser.add_argument("--compliance", default="edge",
                        help="'edge' uses exported compliance; a number overrides every edge")
    Parser.add_argument("--precision", choices=("both", "float32", "float64"), default="both")
    Parser.add_argument("--seeds", default="center-x,center-y,center-z")
    Parser.add_argument("--center-index", type=int)
    Parser.add_argument("--center-amplitude", type=float, default=1.0e-4)
    Parser.add_argument("--mode-file", type=Path,
                        help="Analyzer output or a JSON object with PositionDeltaMeters and optional VelocityDeltaMetersPerSecond")
    Parser.add_argument("--mode-scenario-index", type=int, default=0)
    Parser.add_argument("--mode-index", type=int, default=0)
    Parser.add_argument("--mode-scale-factor", type=float, default=1.0)
    Args = Parser.parse_args()
    try:
        Interval = ProjectionModes.ParseInterval(Args.interval)
        if Args.sweeps < 1 or Args.sweeps > 64 or Args.steps < 1 or Args.steps > 240:
            raise ValueError("Sweeps must be 1..64 and steps 1..240")
        if (not math.isfinite(Args.damping_rate) or Args.damping_rate < 0.0 or
                Args.damping_rate * Interval > 1.0 or
                not math.isfinite(Args.center_amplitude) or Args.center_amplitude <= 0.0 or
                not math.isfinite(Args.mode_scale_factor) or Args.mode_scale_factor <= 0.0):
            raise ValueError("Damping or perturbation amplitude out of bounded range")
        Compliance = None if Args.compliance == "edge" else float(Args.compliance)
        if Compliance is not None and (not math.isfinite(Compliance) or Compliance < 0.0):
            raise ValueError("Compliance must be nonnegative finite or 'edge'")
        Fixture = ProjectionModes.LoadFixture(Args.input, "xyz")
        Positions = Fixture["Positions"]
        Movable = NumPy.flatnonzero(Fixture["InverseMasses"] > 0.0)
        Centroid = NumPy.mean(Positions[Movable], axis=0)
        CenterIndex = (int(Args.center_index) if Args.center_index is not None else
                       int(Movable[NumPy.argmin(NumPy.linalg.norm(
                           Positions[Movable] - Centroid, axis=1))]))
        if CenterIndex < 0 or CenterIndex >= len(Positions) or Fixture["InverseMasses"][CenterIndex] == 0:
            raise ValueError("Center vertex must exist and be movable")
        Seeds = [Item.strip() for Item in Args.seeds.split(",") if Item.strip()]
        if Args.mode_file and "mode" not in Seeds:
            Seeds.append("mode")
        if not Seeds or any(Item not in ("center-x", "center-y", "center-z", "mode") for Item in Seeds):
            raise ValueError("Seeds must be center-x, center-y, center-z, or mode")
        if "mode" in Seeds and not Args.mode_file:
            raise ValueError("Mode seed requires --mode-file")
        Mode = None
        if Args.mode_file:
            Mode = ParseMode(Args.mode_file, Args.mode_scenario_index,
                             Args.mode_index, len(Positions))
            Mode = (Mode[0] * Args.mode_scale_factor, Mode[1] * Args.mode_scale_factor)
        Precisions = ("float32", "float64") if Args.precision == "both" else (Args.precision,)
        if (len(Positions) > 4096 or len(Fixture["Edges"]) > 20000 or
                len(Fixture["Edges"]) * Args.steps * Args.sweeps *
                len(Seeds) * len(Precisions) > 50_000_000):
            raise ValueError("Replay graph or total ordered-edge work exceeds the bounded probe")
        Runs = [Replay(Fixture, Seed, Precision, Interval, Args.sweeps, Args.steps,
                       Args.damping_rate, Compliance, CenterIndex, Args.center_amplitude, Mode)
                for Seed in Seeds for Precision in Precisions]
        Result = {"Kind": "ExternalNonlinearOrderedEdgeReplay",
                  "InputSha256": Fixture["RawSha256"], "NumPyVersion": NumPy.__version__,
                  "VertexCount": len(Positions), "OrderedEdgeCount": len(Fixture["Edges"]),
                  "IntervalSeconds": Interval, "SweepsPerStep": Args.sweeps,
                  "RequestedSteps": Args.steps, "DampingRatePerSecond": Args.damping_rate,
                  "ComplianceOverride": "edge" if Compliance is None else Compliance,
                  "LambdaModel": "native-reset-every-edge-application",
                  "EnergyScope": "Candidate kinetic energy from (current position - pre-step position)/h after prediction and each sweep; accepted velocity reconstructed the same way. Elastic estimate sums C^2/(2c) for positive-compliance edges. This is not stored solver energy or a complete mechanical-energy balance.",
                  "ModelLimits": "External ordered distance-edge replay at exported rest graph, zero gravity and no contact, rigid coupling, native optimized SIMD rounding, or native job scheduling. Float32 and float64 compare arithmetic sensitivity, not native equivalence.",
                  "Runs": Runs}
        Encoded = json.dumps(Result, indent=2, allow_nan=False)
        if Args.output:
            Args.output.write_text(Encoded + "\n", encoding="utf-8")
        else:
            print(Encoded)
    except (OSError, KeyError, IndexError, TypeError, ValueError, json.JSONDecodeError) as Error:
        print(f"[Physics:ProjectionReplay] {Error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(Main())

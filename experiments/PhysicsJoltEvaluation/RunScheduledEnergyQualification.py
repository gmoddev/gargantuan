"""Run pristine/observed Jolt schedules and audit every soft solver microstep.

The positive-compliance edge energy is a specified Hookean reference model,
not a claim about Jolt's unexposed solver energy. A physical rejection is
preserved as data; missing observations or changed pristine dynamics are
harness errors.
"""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import time


def ReadPublished(PathName):
    Config = None
    Frames = []
    Microsteps = {}
    Summary = None
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind == "Config":
                assert Config is None, "Duplicate Config"
                Config = Row
            elif Kind == "Frame":
                assert Row["Frame"] == len(Frames), "Frame chronology changed"
                Frames.append(Row)
            elif Kind == "Microstep":
                Key = (Row["Frame"], Row["Microstep"])
                assert Key not in Microsteps, "Duplicate accepted world update"
                Microsteps[Key] = Row
            elif Kind == "Summary":
                assert Summary is None, "Duplicate Summary"
                Summary = Row
    assert Config is not None and Summary is not None
    assert len(Frames) == Config["Frames"]
    assert len(Microsteps) == Config["Frames"] * Config["WorldUpdatesPerFrame"]
    for Frame in range(Config["Frames"]):
        for Update in range(Config["WorldUpdatesPerFrame"]):
            assert (Frame, Update) in Microsteps, "Missing accepted world update"
    return Config, Frames, Microsteps, Summary


def PhysicalRow(Row):
    Copy = dict(Row)
    Copy.pop("StepMs", None)
    return Copy


def RunBinary(Binary, Options, StdoutPath, StderrPath):
    Start = time.monotonic()
    with StdoutPath.open("w", encoding="utf-8") as Stdout, StderrPath.open("w", encoding="utf-8") as Stderr:
        Process = subprocess.run([str(Binary), *Options], stdout=Stdout, stderr=Stderr, timeout=900)
    if Process.returncode:
        raise RuntimeError(f"[Physics:ScheduledEnergy] {Binary.name} exited {Process.returncode}; see {StderrPath}")
    return time.monotonic() - Start


def FirstFailure(Frame, Update, Ordinal, Value, Limit):
    return {"Frame": Frame, "WorldUpdate": Update, "SolverOrdinal": Ordinal,
            "Value": Value if math.isfinite(Value) else None, "Limit": Limit}


def Measured(Value):
    return float(Value) if Value is not None else float("nan")


def ScoreSolver(PathName, Config, Accepted):
    Frames = int(Config["Frames"])
    Updates = int(Config["WorldUpdatesPerFrame"])
    Steps = int(Config["Steps"])
    Iterations = int(Config["Iterations"])
    PerUpdate = Steps * Iterations
    Expected = Frames * Updates * PerUpdate
    InitialK = float(Config["InitialKineticEnergy"])
    InitialU = float(Config["InitialCompliantEdgeElasticEstimate"])
    KineticCeiling = 1.05 * (InitialK + InitialU if Config["Fixture"] == "Spring" else InitialK) + 1.0e-5
    ModelCeiling = 1.05 * (InitialK + InitialU) + 1.0e-5
    MaximumK = MaximumModel = MaximumPenetration = MaximumStrain = MaximumPinDrift = 0.0
    MaximumScalarClosure = MaximumEndpointKDifference = MaximumEndpointUDifference = 0.0
    MaximumPendingMagnitude = 0.0
    NonfiniteSolverStates = 0
    FirstKinetic = FirstModel = FirstGeometry = None
    LastStates = {}
    Count = 0
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            assert Row.get("Type") == "SolverMicrostep", "Unexpected solver observation row"
            Frame = Count // (Updates * PerUpdate)
            Update = Count // PerUpdate % Updates
            Ordinal = Count % PerUpdate
            assert (Row["Frame"], Row["WorldUpdate"], Row["SolverOrdinal"]) == (Frame, Update, Ordinal), \
                "Solver observation chronology/coverage mismatch"
            assert Row["CollisionStep"] == Ordinal // Iterations
            assert Row["InternalIteration"] == Ordinal % Iterations + 1
            assert math.isclose(float(Row["InternalIntegrationDt"]), float(Config["InternalIntegrationDt"]),
                                rel_tol=1.0e-6, abs_tol=1.0e-12)
            SoftK = Measured(Row["SoftKE"])
            BaselineLinear = Measured(Row["RigidBaselineLinearKE"])
            BaselineAngular = Measured(Row["RigidBaselineAngularKE"])
            Pending = Measured(Row["PendingRigidKineticDelta"])
            K = Measured(Row["KineticEnergy"])
            U = Measured(Row["CompliantEdgeElasticEstimate"])
            Model = Measured(Row["KPlusEdgeModelEnergy"])
            Penetration = Measured(Row["ParticlePenetration"])
            Strain = Measured(Row["AllEdgeStrain"])
            PinDrift = Measured(Row["MaxPinDrift"])
            Terms = (SoftK, BaselineLinear, BaselineAngular, Pending, K, U, Model,
                     Penetration, Strain, PinDrift)
            Finite = bool(Row["Finite"]) and all(math.isfinite(Value) for Value in Terms)
            if Finite:
                ScalarClosure = abs(K - (SoftK + BaselineLinear + BaselineAngular + Pending))
                ModelClosure = abs(Model - (K + U))
                MaximumScalarClosure = max(MaximumScalarClosure, ScalarClosure, ModelClosure)
                assert ScalarClosure <= 1.0e-10 * max(1.0, abs(K)), "Native scalar K closure failed"
                assert ModelClosure <= 1.0e-10 * max(1.0, abs(Model)), "Native scalar K+U closure failed"
            else:
                NonfiniteSolverStates += 1
            if math.isfinite(K): MaximumK = max(MaximumK, K)
            if math.isfinite(Model): MaximumModel = max(MaximumModel, Model)
            if math.isfinite(Penetration): MaximumPenetration = max(MaximumPenetration, Penetration)
            if math.isfinite(Strain): MaximumStrain = max(MaximumStrain, Strain)
            if math.isfinite(PinDrift): MaximumPinDrift = max(MaximumPinDrift, PinDrift)
            if math.isfinite(Pending): MaximumPendingMagnitude = max(MaximumPendingMagnitude, abs(Pending))
            if FirstKinetic is None and (not Finite or K < 0 or K > KineticCeiling):
                FirstKinetic = FirstFailure(Frame, Update, Ordinal, K, KineticCeiling)
            if FirstModel is None and (not Finite or U < 0 or Model > ModelCeiling):
                FirstModel = FirstFailure(Frame, Update, Ordinal, Model, ModelCeiling)
            if FirstGeometry is None and (not Finite or PinDrift > 1.0e-4 or
                                          (Config["Fixture"] != "Spring" and
                                           (Penetration > 0.1 or Strain > 0.1))):
                FirstGeometry = {"Frame": Frame, "WorldUpdate": Update, "SolverOrdinal": Ordinal,
                                 "PenetrationM": Penetration if math.isfinite(Penetration) else None,
                                 "AllEdgeStrain": Strain if math.isfinite(Strain) else None,
                                 "PinDriftM": PinDrift if math.isfinite(PinDrift) else None}
            if Ordinal + 1 == PerUpdate:
                LastStates[(Frame, Update)] = (K, U, Model, Penetration, Strain, PinDrift)
            Count += 1
    assert Count == Expected, f"Solver observation count {Count} differs from expected {Expected}"
    assert len(LastStates) == Frames * Updates
    for Key, (K, U, Model, Penetration, Strain, PinDrift) in LastStates.items():
        Boundary = Accepted[Key]
        BoundaryK = Measured(Boundary["KineticEnergy"])
        BoundaryU = Measured(Boundary["CompliantEdgeElasticEstimate"])
        if all(math.isfinite(Value) for Value in (K, U, BoundaryK, BoundaryU)):
            MaximumEndpointKDifference = max(MaximumEndpointKDifference, abs(K - BoundaryK))
            MaximumEndpointUDifference = max(MaximumEndpointUDifference, abs(U - BoundaryU))
        if Steps == 1 and Iterations == 1 and all(math.isfinite(Value) for Value in (K, U, BoundaryK, BoundaryU)):
            # The hook sees rigid velocities before float32 impulse writeback.
            # A 64-ulp-relative envelope covers that conversion only; it is
            # several orders below the unchanged 5% physical energy ceiling.
            FloatEnvelope = 64.0 * 2.0 ** -23 * max(1.0, abs(K), abs(BoundaryK))
            assert abs(K - BoundaryK) <= FloatEnvelope, f"Solver/boundary K mismatch at {Key}"
            assert U == BoundaryU, f"Solver/boundary U mismatch at {Key}"
            assert Penetration == float(Boundary["ParticlePenetration"]), f"Sphere gap mismatch at {Key}"
            assert Strain == float(Boundary["AllEdgeStrain"]), f"Edge strain mismatch at {Key}"
            assert PinDrift == float(Boundary["MaxPinDrift"]), f"Pin drift mismatch at {Key}"
    return {"ObservedSolverMicrosteps": Count, "ExpectedSolverMicrosteps": Expected,
            "NonfiniteSolverStates": NonfiniteSolverStates,
            "InitialKineticEnergyJ": InitialK, "InitialEdgeModelEnergyJ": InitialU,
            "NecessaryKineticCeilingJ": KineticCeiling, "ReferenceModelCeilingJ": ModelCeiling,
            "MaximumSolverKineticEnergyJ": MaximumK, "MaximumSolverKPlusEdgeModelJ": MaximumModel,
            "MaximumSolverPenetrationM": MaximumPenetration, "MaximumSolverStrain": MaximumStrain,
            "MaximumSolverPinDriftM": MaximumPinDrift, "MaximumPendingRigidDeltaMagnitudeJ": MaximumPendingMagnitude,
            "MaximumScalarClosureErrorJ": MaximumScalarClosure,
            "MaximumSolverToWorldUpdateKDifferenceJ": MaximumEndpointKDifference,
            "MaximumSolverToWorldUpdateUDifferenceJ": MaximumEndpointUDifference,
            "FirstNecessaryKineticFailure": FirstKinetic,
            "FirstReferenceModelFailure": FirstModel,
            "FirstGeometryFailure": FirstGeometry,
            "CompleteBoundedPass": FirstKinetic is None and FirstModel is None and FirstGeometry is None}


def Profiles():
    for Material, Schedules in (("1e-5", ((1, 24, 1), (4, 24, 1), (1, 1, 96))),
                                ("1e-6", ((8, 24, 1), (4, 48, 1), (1, 1, 192)))):
        for Steps, Iterations, Updates in Schedules:
            Negative = Material == "1e-5" and Steps == 1 and Iterations == 24
            Label = f"M{Material}-S{Steps}I{Iterations}W{Updates}"
            for Fixture in ("C1", "C2", "Free"):
                yield Label, Fixture, Material, Steps, Iterations, Updates, (36 if Negative else 150)


def Float32(Value):
    return struct.unpack("<f", struct.pack("<f", Value))[0]


def NativeStepTime(Updates, Steps, Iterations):
    Dt = Float32(1.0 / 60.0)
    return Float32(Float32(Float32(Dt / Updates) / Steps) / Iterations)


def CalibrateSpring(Arguments, PristineBinary, ObservedBinary, Output, Material,
                    IntegrationsPerFrame=None, VelocityReconstruction="difference", ConstraintSweeps=1):
    Updates = IntegrationsPerFrame if IntegrationsPerFrame is not None else (96 if Material == "1e-5" else 192)
    if not isinstance(Updates, int) or not 1 <= Updates <= 384:
        raise ValueError("[Physics:ScheduledEnergy] Spring calibration cadence outside 1..384")
    if VelocityReconstruction not in ("difference", "delta", "applieddelta"):
        raise ValueError("[Physics:ScheduledEnergy] Unknown spring calibration velocity reconstruction")
    if ConstraintSweeps not in (1, 2, 4, 8) or (ConstraintSweeps > 1 and VelocityReconstruction != "delta"):
        raise ValueError("[Physics:ScheduledEnergy] Multiple constraint sweeps require delta mode and Q=2,4,8")
    Name = f"Spring-M{Material}-W{Updates}-Q{ConstraintSweeps}"
    Common = ["--fixture", "Spring", "--frames", "8", "--steps", "1", "--iterations", "1",
              "--world-updates", str(Updates), "--compliance", Material, "--damping", "0",
              "--contact", "0", "--speed", "0", "--workers", str(Arguments.threads),
              "--particles", "1", "--observe-updates", "1",
              "--constraint-sweeps", str(ConstraintSweeps)]
    if VelocityReconstruction != "difference":
        Common += ["--velocity-reconstruction", VelocityReconstruction]
    PristinePath = Output / f"{Name}.pristine.jsonl"
    ObservedPath = Output / f"{Name}.observed.jsonl"
    SolverPath = Output / f"{Name}.solver.jsonl"
    PristineSeconds = RunBinary(PristineBinary, Common, PristinePath,
                                Output / f"{Name}.pristine.stderr.txt")
    ObservedSeconds = RunBinary(ObservedBinary, Common + ["--solver-observations", str(SolverPath)],
                                ObservedPath, Output / f"{Name}.observed.stderr.txt")
    PristineConfig, PristineFrames, PristineMicrosteps, _ = ReadPublished(PristinePath)
    ObservedConfig, ObservedFrames, ObservedMicrosteps, _ = ReadPublished(ObservedPath)
    ExpectedDt = NativeStepTime(Updates, 1, 1)
    assert PristineConfig["WorldUpdatesPerFrame"] == ObservedConfig["WorldUpdatesPerFrame"] == Updates
    assert PristineConfig["Steps"] == ObservedConfig["Steps"] == 1
    assert PristineConfig["Iterations"] == ObservedConfig["Iterations"] == 1
    assert PristineConfig["ConstraintSweeps"] == ObservedConfig["ConstraintSweeps"] == ConstraintSweeps
    assert PristineConfig["InternalIntegrationDt"] == ObservedConfig["InternalIntegrationDt"] == ExpectedDt
    assert PristineConfig.get("VelocityReconstruction", "difference") == VelocityReconstruction
    assert ObservedConfig.get("VelocityReconstruction", "difference") == VelocityReconstruction
    assert PristineConfig["InitialKineticEnergy"] == ObservedConfig["InitialKineticEnergy"]
    assert PristineConfig["InitialCompliantEdgeElasticEstimate"] == ObservedConfig["InitialCompliantEdgeElasticEstimate"]
    assert PristineConfig["AllEdgesPositiveCompliance"] and ObservedConfig["AllEdgesPositiveCompliance"]
    assert all(PhysicalRow(Left) == PhysicalRow(Right)
               for Left, Right in zip(PristineFrames, ObservedFrames)), "Spring frame parity failed"
    assert all(PhysicalRow(Left) == PhysicalRow(ObservedMicrosteps[Key])
               for Key, Left in PristineMicrosteps.items()), "Spring world-update parity failed"
    Score = ScoreSolver(SolverPath, ObservedConfig, ObservedMicrosteps)
    QualityPath = Output / f"{Name}.quality.json"
    subprocess.run([sys.executable, str(Path(__file__).with_name("EnergyQualification.py")),
                    "--input", str(ObservedPath), "--output", str(QualityPath)], check=True)
    Quality = json.loads(QualityPath.read_text(encoding="utf-8"))
    Calibrated = bool(Quality["CompleteBoundedPass"] and
                      Quality["NativeSpringCalibration"] and Quality["NativeSpringCalibration"]["Pass"] and
                      Quality["NativeSpringCalibration"].get("VelocityReconstruction", "difference") == VelocityReconstruction and
                      Quality["NativeSpringCalibration"].get("ConstraintSweeps") == ConstraintSweeps and
                      Quality["ConfigSummary"].get("VelocityReconstruction", "difference") == VelocityReconstruction and
                      Quality["ConfigSummary"].get("ConstraintSweeps") == ConstraintSweeps and
                      Score["CompleteBoundedPass"])
    return {"Name": Name, "MaterialCompliance": Material, "WorldUpdates": Updates,
            "InternalIntegrationDt": ExpectedDt, "ConstraintSweeps": ConstraintSweeps,
            "VelocityReconstruction": VelocityReconstruction,
            "PristineSeconds": PristineSeconds, "ObservedSeconds": ObservedSeconds,
            "PristineRaw": PristinePath.name, "ObservedRaw": ObservedPath.name,
            "SolverRaw": SolverPath.name, "QualityFile": QualityPath.name,
            "PublishedFrameParity": True, "PublishedWorldUpdateParity": True,
            "Score": Score, "NativeSpringCalibration": Quality["NativeSpringCalibration"],
            "Calibrated": Calibrated,
            "Status": "PASS_NATIVE_ONE_EDGE_CALIBRATION" if Calibrated else "FAIL_NATIVE_ONE_EDGE_CALIBRATION"}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--pristine", type=Path, required=True)
    Parser.add_argument("--observed", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--threads", type=int, default=1)
    Arguments = Parser.parse_args()
    if Arguments.threads < 1 or Arguments.threads > 8:
        Parser.error("--threads must be between 1 and 8")
    PristineBinary = Arguments.pristine.resolve(strict=True)
    ObservedBinary = Arguments.observed.resolve(strict=True)
    Output = Arguments.output
    Output.mkdir(parents=True, exist_ok=False)
    Receipts = []
    try:
        Calibrations = {}
        for Material in ("1e-5", "1e-6"):
            Calibration = CalibrateSpring(Arguments, PristineBinary, ObservedBinary, Output, Material)
            Calibrations[Material] = Calibration
            (Output / "Calibrations.json").write_text(json.dumps(Calibrations, indent=2, allow_nan=False), encoding="utf-8")
            print(json.dumps({"Name": Calibration["Name"], "Status": Calibration["Status"]}), flush=True)
        for Schedule, Fixture, Material, Steps, Iterations, Updates, Frames in Profiles():
            Name = f"{Schedule}-{Fixture}"
            Common = ["--frames", str(Frames), "--steps", str(Steps), "--iterations", str(Iterations),
                      "--world-updates", str(Updates), "--compliance", Material, "--bend-compliance", "1e-4",
                      "--workers", str(Arguments.threads), "--particles", "0", "--observe-updates", "1",
                      "--count", "2" if Fixture == "C2" else "1"]
            if Fixture == "Free":
                Common += ["--contact", "0", "--speed", "0", "--seed", "1"]
            PristinePath = Output / f"{Name}.pristine.jsonl"
            ObservedPath = Output / f"{Name}.observed.jsonl"
            SolverPath = Output / f"{Name}.solver.jsonl"
            PristineSeconds = RunBinary(PristineBinary, Common, PristinePath,
                                        Output / f"{Name}.pristine.stderr.txt")
            ObservedSeconds = RunBinary(ObservedBinary, Common + ["--solver-observations", str(SolverPath)],
                                        ObservedPath, Output / f"{Name}.observed.stderr.txt")
            PristineConfig, PristineFrames, PristineMicrosteps, _ = ReadPublished(PristinePath)
            ObservedConfig, ObservedFrames, ObservedMicrosteps, _ = ReadPublished(ObservedPath)
            assert PristineConfig["AllEdgesPositiveCompliance"] and ObservedConfig["AllEdgesPositiveCompliance"]
            assert PristineConfig["ZeroGravity"] and ObservedConfig["ZeroGravity"]
            assert PristineConfig["Pinned"] and ObservedConfig["Pinned"]
            assert PristineConfig["Dynamic"] and ObservedConfig["Dynamic"]
            assert PristineConfig["Fixture"] == ObservedConfig["Fixture"] == "Cloth"
            assert PristineConfig["InitialKineticEnergy"] == ObservedConfig["InitialKineticEnergy"]
            assert PristineConfig["InitialCompliantEdgeElasticEstimate"] == ObservedConfig["InitialCompliantEdgeElasticEstimate"]
            if Fixture == "Free":
                assert not ObservedConfig["Contact"] and ObservedConfig["Speed"] == 0.0
                assert math.isclose(ObservedConfig["InitialKineticEnergy"], 0.05, abs_tol=1.0e-7)
            else:
                assert ObservedConfig["Contact"] and ObservedConfig["Speed"] == 5.0
                AnalyticInitial = 0.5 * (2 if Fixture == "C2" else 1) * ObservedConfig["RigidMass"] * 25.0
                assert math.isclose(ObservedConfig["InitialKineticEnergy"], AnalyticInitial, rel_tol=1.0e-6)
            assert PristineFrames and len(PristineFrames) == len(ObservedFrames)
            for Left, Right in zip(PristineFrames, ObservedFrames):
                assert PhysicalRow(Left) == PhysicalRow(Right), f"Pristine/observer frame dynamics differ: {Name} frame {Left['Frame']}"
            for Key, Left in PristineMicrosteps.items():
                assert PhysicalRow(Left) == PhysicalRow(ObservedMicrosteps[Key]), \
                    f"Pristine/observer world-update dynamics differ: {Name} {Key}"
            Score = ScoreSolver(SolverPath, ObservedConfig, ObservedMicrosteps)
            Calibrated = Calibrations[Material]["Calibrated"]
            Receipt = {"Name": Name, "Fixture": Fixture, "MaterialCompliance": Material,
                       "CollisionSteps": Steps, "SoftIterations": Iterations, "WorldUpdates": Updates,
                       "Frames": Frames, "Threads": Arguments.threads,
                       "SpringCalibration": Calibrations[Material]["Name"],
                       "SpringCalibrationPass": Calibrated,
                       "PristineSeconds": PristineSeconds, "ObservedSeconds": ObservedSeconds,
                       "PublishedFrameParity": True, "PublishedWorldUpdateParity": True,
                       "PristineRaw": PristinePath.name, "ObservedRaw": ObservedPath.name,
                       "SolverRaw": SolverPath.name, "Score": Score,
                       "Status": ("PASS_BOUNDED_SCHEDULED_REFERENCE_MODEL" if Score["CompleteBoundedPass"]
                                  else "FAIL_OBSERVED_BOUND") if Calibrated else "UNQUALIFIED_SPRING_CALIBRATION"}
            Receipts.append(Receipt)
            (Output / "Receipts.json").write_text(json.dumps(Receipts, indent=2, allow_nan=False), encoding="utf-8")
            print(json.dumps({"Name": Name, "Status": Receipt["Status"],
                              "MaxKPlusU": Score["MaximumSolverKPlusEdgeModelJ"]}), flush=True)
    except Exception as Error:
        (Output / "HarnessFailure.json").write_text(json.dumps({"Error": repr(Error),
            "CompletedCases": len(Receipts)}, indent=2), encoding="utf-8")
        raise
    Summary = {"Cases": len(Receipts), "SpringCalibrations": Calibrations, "HarnessQualified": True,
               "PhysicallyQualified": sum(Receipt["Status"] == "PASS_BOUNDED_SCHEDULED_REFERENCE_MODEL" for Receipt in Receipts),
               "ObservedBoundRejections": sum(not Receipt["Score"]["CompleteBoundedPass"] for Receipt in Receipts),
               "UnqualifiedCalibration": sum(not Receipt["SpringCalibrationPass"] for Receipt in Receipts),
               "Scope": "Pristine/observer identical physical states and complete sampled solver-stage K plus positive-compliance Hookean edge model; no general solver conservation claim"}
    (Output / "Summary.json").write_text(json.dumps(Summary, indent=2), encoding="utf-8")


if __name__ == "__main__":
    Main()

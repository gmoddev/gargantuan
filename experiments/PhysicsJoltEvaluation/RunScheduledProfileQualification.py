"""Qualify one selected Jolt schedule across repeats, workers and perturbations.

Raw observed/pristine traces and frame-only timing runs are retained. Physical
failures are results, while missing solver observations or changed dynamics are
harness errors. This does not compare timing with the private XPBD backend.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import time
from types import SimpleNamespace

from RunScheduledEnergyQualification import (
    CalibrateSpring, PhysicalRow, ReadPublished, ScoreSolver,
)


def FrameDigest(Frames):
    Digest = hashlib.sha256()
    for Frame in Frames:
        Digest.update((json.dumps(PhysicalRow(Frame), sort_keys=True,
                                  separators=(",", ":")) + "\n").encode())
    return Digest.hexdigest()


def ReadTiming(PathName):
    Config = None
    Frames = []
    Summary = None
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind == "Config":
                assert Config is None
                Config = Row
            elif Kind == "Frame":
                assert Row["Frame"] == len(Frames)
                Frames.append(Row)
            elif Kind == "Microstep":
                raise AssertionError("Frame-only timing run emitted accepted microsteps")
            elif Kind == "Summary":
                assert Summary is None
                Summary = Row
    assert Config is not None and Summary is not None
    assert len(Frames) == int(Config["Frames"])
    assert not Config["ObserveUpdates"]
    return Config, Frames, Summary


def RunBinary(Binary, Options, StdoutPath, StderrPath):
    Start = time.monotonic()
    with StdoutPath.open("w", encoding="utf-8") as Stdout, StderrPath.open("w", encoding="utf-8") as Stderr:
        Process = subprocess.run([str(Binary), *Options], stdout=Stdout, stderr=Stderr, timeout=3600)
    if Process.returncode:
        raise RuntimeError(f"[Physics:ScheduledProfile] {Binary.name} exited {Process.returncode}; see {StderrPath}")
    return time.monotonic() - Start


def Options(Material, Steps, Iterations, Updates, Frames, Workers, Fixture, Extra=()):
    Result = ["--frames", str(Frames), "--steps", str(Steps), "--iterations", str(Iterations),
              "--world-updates", str(Updates), "--compliance", Material,
              "--bend-compliance", "1e-4", "--workers", str(Workers), "--particles", "0",
              "--count", "2" if Fixture == "C2" else "1"]
    if Fixture == "Free":
        Result += ["--contact", "0", "--speed", "0", "--seed", "1"]
    return Result + list(Extra)


def RigidResponse(Frames, Fixture, InitialSpeed):
    if Fixture == "Free":
        return {"Applicable": False, "Responded": None, "PerRigidMaxVelocityChangeMps": []}
    Initial = [tuple(Rigid["V"]) for Rigid in Frames[0]["Rigids"]]
    Maximum = [0.0] * len(Initial)
    for Frame in Frames:
        for Index, Rigid in enumerate(Frame["Rigids"]):
            Velocity = Rigid["V"]
            Difference = math.sqrt(sum((float(Value) - float(Baseline)) ** 2
                                       for Value, Baseline in zip(Velocity, Initial[Index])))
            Maximum[Index] = max(Maximum[Index], Difference)
    # The first published state precedes sphere contact, but use the authored
    # speed to guard a malformed input that never moved its rigid body.
    return {"Applicable": True, "Responded": all(Value >= 0.01 for Value in Maximum),
            "PerRigidMaxVelocityChangeMps": Maximum, "AuthoredSpeedMps": InitialSpeed}


def RunObservedCase(PristineBinary, ObservedBinary, Output, Name, Fixture, Material,
                    Steps, Iterations, Updates, Frames, Workers, Extra=()):
    Common = Options(Material, Steps, Iterations, Updates, Frames, Workers, Fixture, Extra)
    PristinePath = Output / f"{Name}.pristine.jsonl"
    ObservedPath = Output / f"{Name}.observed.jsonl"
    SolverPath = Output / f"{Name}.solver.jsonl"
    PristineWall = RunBinary(PristineBinary, Common + ["--observe-updates", "1"],
                             PristinePath, Output / f"{Name}.pristine.stderr.txt")
    ObservedWall = RunBinary(ObservedBinary, Common + ["--observe-updates", "1",
                                              "--solver-observations", str(SolverPath)],
                             ObservedPath, Output / f"{Name}.observed.stderr.txt")
    PristineConfig, PristineFrames, PristineMicrosteps, _ = ReadPublished(PristinePath)
    ObservedConfig, ObservedFrames, ObservedMicrosteps, _ = ReadPublished(ObservedPath)
    assert PristineConfig["InitialKineticEnergy"] == ObservedConfig["InitialKineticEnergy"]
    assert PristineConfig["InitialCompliantEdgeElasticEstimate"] == ObservedConfig["InitialCompliantEdgeElasticEstimate"]
    assert ObservedConfig["AllEdgesPositiveCompliance"] and ObservedConfig["ZeroGravity"]
    assert ObservedConfig["Pinned"] and ObservedConfig["Dynamic"]
    assert len(PristineFrames) == len(ObservedFrames) == Frames
    for Left, Right in zip(PristineFrames, ObservedFrames):
        assert PhysicalRow(Left) == PhysicalRow(Right), f"Pristine/observer frame state changed: {Name} frame {Left['Frame']}"
    for Key, Left in PristineMicrosteps.items():
        assert PhysicalRow(Left) == PhysicalRow(ObservedMicrosteps[Key]), \
            f"Pristine/observer accepted state changed: {Name} {Key}"
    Score = ScoreSolver(SolverPath, ObservedConfig, ObservedMicrosteps)
    Contacts = sum(int(Frame["Contacts"]) for Frame in ObservedFrames)
    Response = RigidResponse(ObservedFrames, Fixture, float(ObservedConfig["Speed"]))
    if Fixture == "Free":
        ContactEvidence = not ObservedConfig["Contact"] and Contacts == 0 and ObservedConfig["Speed"] == 0.0
    else:
        ContactEvidence = bool(ObservedConfig["Contact"] and Contacts > 0 and Response["Responded"])
    return {"Name": Name, "Fixture": Fixture, "MaterialCompliance": Material,
            "Frames": Frames, "Workers": Workers, "CollisionSteps": Steps,
            "SoftIterations": Iterations, "WorldUpdates": Updates, "ExtraOptions": list(Extra),
            "PristineRaw": PristinePath.name, "ObservedRaw": ObservedPath.name,
            "SolverRaw": SolverPath.name, "FrameStateSha256": FrameDigest(ObservedFrames),
            "PristineWallSeconds": PristineWall, "ObservedWallSeconds": ObservedWall,
            "PristineObserverParity": True, "TotalContacts": Contacts,
            "ContactOrFreeEvidence": ContactEvidence, "RigidResponse": Response,
            "Score": Score,
            "PhysicalPass": bool(Score["CompleteBoundedPass"] and ContactEvidence)}


def RunTimingCase(PristineBinary, Output, Name, Fixture, Material, Steps,
                  Iterations, Updates, Frames, Workers, ExpectedDigest):
    PathName = Output / f"{Name}.timing.jsonl"
    Common = Options(Material, Steps, Iterations, Updates, Frames, Workers, Fixture)
    Wall = RunBinary(PristineBinary, Common + ["--observe-updates", "0"],
                     PathName, Output / f"{Name}.timing.stderr.txt")
    _, FramesData, _ = ReadTiming(PathName)
    Digest = FrameDigest(FramesData)
    assert Digest == ExpectedDigest, f"Frame-only timing changed physical state: {Name}"
    Times = [float(Frame["StepMs"]) for Frame in FramesData]
    return {"Name": Name, "Raw": PathName.name, "FrameStateSha256": Digest,
            "FrameParity": True, "WallSeconds": Wall,
            "NativeUpdateTotalMs": sum(Times), "NativeUpdateMedianFrameMs": statistics.median(Times),
            "ObservationScope": "Frame-only pristine World.Update timing; no accepted internal energy claim"}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--pristine", type=Path, required=True)
    Parser.add_argument("--observed", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--world-updates", type=int, required=True)
    Parser.add_argument("--steps", type=int, required=True)
    Parser.add_argument("--iterations", type=int, required=True)
    Parser.add_argument("--compliance", type=float, required=True)
    Arguments = Parser.parse_args()
    if (Arguments.world_updates < 1 or Arguments.world_updates > 384 or
            Arguments.steps < 1 or Arguments.steps > 96 or
            Arguments.iterations < 1 or Arguments.iterations > 64 or
            Arguments.world_updates * Arguments.steps * Arguments.iterations > 2304 or
            not math.isfinite(Arguments.compliance) or Arguments.compliance <= 0):
        Parser.error("Selected positive-compliance schedule is outside the diagnostic's bounded range")
    Cadence = Arguments.world_updates * Arguments.steps * Arguments.iterations
    if math.isclose(Arguments.compliance, 1.0e-5, rel_tol=1.0e-9, abs_tol=0.0):
        Material = "1e-5"
        CalibrationCadence = 96
    elif math.isclose(Arguments.compliance, 1.0e-6, rel_tol=1.0e-9, abs_tol=0.0):
        Material = "1e-6"
        CalibrationCadence = 192
    else:
        Parser.error("This qualification has native spring calibration only for 1e-5 and 1e-6 compliance")
    if Cadence != CalibrationCadence:
        Parser.error(f"Selected schedule must have {CalibrationCadence} internal integrations per frame to match the native spring calibration")
    PristineBinary = Arguments.pristine.resolve(strict=True)
    ObservedBinary = Arguments.observed.resolve(strict=True)
    Output = Arguments.output
    Output.mkdir(parents=True, exist_ok=False)
    Steps, Iterations, Updates = Arguments.steps, Arguments.iterations, Arguments.world_updates
    Receipts = []
    TimingReceipts = []
    try:
        Calibrations = {}
        for CalMaterial in sorted({Material, "1e-5"}):
            Calibration = CalibrateSpring(SimpleNamespace(threads=1), PristineBinary,
                                          ObservedBinary, Output, CalMaterial)
            Calibrations[CalMaterial] = Calibration
            (Output / "Calibrations.json").write_text(json.dumps(Calibrations, indent=2, allow_nan=False), encoding="utf-8")
            print(json.dumps({"Name": Calibration["Name"], "Status": Calibration["Status"]}), flush=True)
        for Fixture in ("C1", "C2"):
            for Workers in (1, 2, 8):
                Group = []
                for Repeat in (1, 2, 3):
                    Name = f"{Fixture}-T{Workers}-R{Repeat}"
                    Receipt = RunObservedCase(PristineBinary, ObservedBinary, Output, Name, Fixture,
                                              Material, Steps, Iterations, Updates, 600, Workers)
                    Receipts.append(Receipt)
                    Group.append(Receipt)
                    (Output / "Receipts.json").write_text(json.dumps(Receipts, indent=2, allow_nan=False), encoding="utf-8")
                    print(json.dumps({"Name": Name, "PhysicalPass": Receipt["PhysicalPass"],
                                      "MaxKPlusU": Receipt["Score"]["MaximumSolverKPlusEdgeModelJ"]}), flush=True)
                assert len({Receipt["FrameStateSha256"] for Receipt in Group}) == 1, \
                    f"Repeated candidate state differs: {Fixture} workers {Workers}"
                Timing = RunTimingCase(PristineBinary, Output, f"{Fixture}-T{Workers}", Fixture,
                                       Material, Steps, Iterations, Updates, 600, Workers,
                                       Group[0]["FrameStateSha256"])
                TimingReceipts.append(Timing)
                (Output / "TimingReceipts.json").write_text(json.dumps(TimingReceipts, indent=2), encoding="utf-8")
        for Fixture in ("C1", "C2"):
            Group = [Receipt for Receipt in Receipts if Receipt["Fixture"] == Fixture]
            assert len({Receipt["FrameStateSha256"] for Receipt in Group}) == 1, \
                f"Worker-count candidate state differs: {Fixture}"
        Followups = [
            ("Free-600", "Free", Material, Steps, Iterations, Updates, 600, ()),
            ("Free-Extended", "Free", Material, Steps, Iterations, Updates, 1800, ()),
            ("C2-Extended", "C2", Material, Steps, Iterations, Updates, 1800, ()),
            ("C2-Speed10", "C2", Material, Steps, Iterations, Updates, 600, ("--speed", "10")),
            ("C2-Frictionless", "C2", Material, Steps, Iterations, Updates, 600, ("--friction", "0")),
            ("C2-RestitutionHalf", "C2", Material, Steps, Iterations, Updates, 600, ("--restitution", "0.5")),
            ("Negative-OriginalS1", "C2", "1e-5", 1, 24, 1, 36, ()),
            ("Negative-FreeU96", "Free", "1e-6", 1, 1, 96, 150, ()),
        ]
        for Name, Fixture, CaseMaterial, CaseSteps, CaseIterations, CaseUpdates, Frames, Extra in Followups:
            Receipt = RunObservedCase(PristineBinary, ObservedBinary, Output, Name, Fixture,
                                      CaseMaterial, CaseSteps, CaseIterations, CaseUpdates,
                                      Frames, 1, Extra)
            Receipts.append(Receipt)
            (Output / "Receipts.json").write_text(json.dumps(Receipts, indent=2, allow_nan=False), encoding="utf-8")
            print(json.dumps({"Name": Name, "PhysicalPass": Receipt["PhysicalPass"],
                              "MaxKPlusU": Receipt["Score"]["MaximumSolverKPlusEdgeModelJ"]}), flush=True)
    except Exception as Error:
        (Output / "HarnessFailure.json").write_text(json.dumps({"Error": repr(Error),
            "CompletedObservedCases": len(Receipts), "CompletedTimingCases": len(TimingReceipts)}, indent=2), encoding="utf-8")
        raise
    MainCases = [Receipt for Receipt in Receipts if not Receipt["Name"].startswith("Negative-")]
    Negatives = [Receipt for Receipt in Receipts if Receipt["Name"].startswith("Negative-")]
    EnergyControlRejections = sum(bool(R["Score"]["FirstNecessaryKineticFailure"] or
                                       R["Score"]["FirstReferenceModelFailure"]) for R in Negatives)
    RequiredGate = bool(Calibrations[Material]["Calibrated"] and all(R["PhysicalPass"] for R in MainCases) and
                        EnergyControlRejections == 2 and
                        len(MainCases) == 24 and len(Negatives) == 2 and len(TimingReceipts) == 6)
    Summary = {"SelectedMaterialCompliance": Material, "SelectedSchedule": {
                   "WorldUpdates": Updates, "CollisionSteps": Steps, "SoftIterations": Iterations},
               "Calibrations": Calibrations, "ObservedCases": len(Receipts),
               "TimingCases": len(TimingReceipts), "RequiredGateQualified": RequiredGate,
               "MainCasePasses": sum(R["PhysicalPass"] for R in MainCases),
               "NegativeControlRejections": EnergyControlRejections,
               "Scope": "Finite specified material/schedule fixtures with complete solver-stage observations, cross-worker state repeatability and separate pristine frame-only timing; not arbitrary Jolt conservation or engine migration qualification"}
    (Output / "Summary.json").write_text(json.dumps(Summary, indent=2, allow_nan=False), encoding="utf-8")
    print(json.dumps({"RequiredGateQualified": RequiredGate, "MainCasePasses": Summary["MainCasePasses"],
                      "NegativeControlRejections": Summary["NegativeControlRejections"]}), flush=True)
    if not RequiredGate:
        raise SystemExit(3)


if __name__ == "__main__":
    Main()

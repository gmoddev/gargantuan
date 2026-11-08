"""Run isolated native Jolt velocity-reconstruction counterfactuals.

Physical failures are evidence and do not fail this runner. Process, chronology,
mode metadata, solver coverage, calibration-tool execution, and parity errors do.
All raw streams are preserved as verified lossless gzip with source SHA-256.
"""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from RunScheduledEnergyQualification import PhysicalRow, ReadPublished, ScoreSolver


CreationFlags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
Modes = ("difference", "delta", "applieddelta")


def HashFile(PathName, Compressed=False):
    Digest = hashlib.sha256()
    Open = gzip.open if Compressed else open
    with Open(PathName, "rb") as Source:
        for Block in iter(lambda: Source.read(1024 * 1024), b""):
            Digest.update(Block)
    return Digest.hexdigest()


def CompressVerified(PathName):
    OriginalHash = HashFile(PathName)
    OriginalLength = PathName.stat().st_size
    CompressedPath = PathName.with_name(PathName.name + ".gz")
    with PathName.open("rb") as Source, CompressedPath.open("wb") as RawTarget:
        with gzip.GzipFile(filename="", mode="wb", fileobj=RawTarget, mtime=0,
                           compresslevel=6) as Target:
            shutil.copyfileobj(Source, Target, 1024 * 1024)
    if HashFile(CompressedPath, True) != OriginalHash:
        raise RuntimeError(f"[Physics:VelocityAblation] Gzip verification failed: {PathName}")
    Receipt = {"RawName": PathName.name, "RawBytes": OriginalLength,
               "RawSha256": OriginalHash, "GzipName": CompressedPath.name,
               "GzipBytes": CompressedPath.stat().st_size,
               "GzipSha256": HashFile(CompressedPath)}
    PathName.unlink()
    return Receipt


def RunBinary(Binary, Options, StdoutPath, StderrPath):
    Started = time.monotonic()
    with StdoutPath.open("wb") as Stdout, StderrPath.open("wb") as Stderr:
        try:
            Process = subprocess.run([str(Binary), *Options], stdout=Stdout, stderr=Stderr,
                                     timeout=1800, creationflags=CreationFlags, check=False)
        except subprocess.TimeoutExpired as Error:
            raise RuntimeError(f"[Physics:VelocityAblation] Timed out: {Binary.name} {Options}") from Error
    if Process.returncode:
        raise RuntimeError(f"[Physics:VelocityAblation] {Binary.name} exited {Process.returncode}; see {StderrPath}")
    return time.monotonic() - Started


def ReadFramesOnly(PathName):
    Config = Summary = None
    Frames = []
    with PathName.open(encoding="utf-8") as Source:
        for Line in Source:
            Row = json.loads(Line)
            Kind = Row.get("Type")
            if Kind == "Config":
                if Config is not None:
                    raise ValueError("Duplicate frame-only Config")
                Config = Row
            elif Kind == "Frame":
                if Row["Frame"] != len(Frames):
                    raise ValueError("Frame-only chronology failure")
                Frames.append(Row)
            elif Kind == "Summary":
                if Summary is not None:
                    raise ValueError("Duplicate frame-only Summary")
                Summary = Row
            elif Kind == "Microstep":
                raise ValueError("Frame-only binary emitted world update states")
    if Config is None or Summary is None or len(Frames) != Config["Frames"]:
        raise ValueError("Incomplete frame-only native output")
    return Config, Frames, Summary


def CommonOptions(Fixture, Frames, Updates, Steps, Iterations, Material, Threads):
    Options = ["--fixture", "Spring" if Fixture == "Spring" else "Cloth",
               "--frames", str(Frames), "--world-updates", str(Updates),
               "--steps", str(Steps), "--iterations", str(Iterations),
               "--compliance", Material, "--bend-compliance", "1e-4",
               "--workers", str(Threads), "--count", "2" if Fixture in ("C2", "C2Speed10") else "1",
               "--particles", "1" if Fixture == "Spring" else "0"]
    if Fixture == "Spring":
        Options += ["--contact", "0", "--speed", "0", "--damping", "0"]
    elif Fixture == "Free":
        Options += ["--contact", "0", "--speed", "0", "--seed", "1"]
    elif Fixture == "C2Speed10":
        Options += ["--speed", "10"]
    return Options


def CaseDefinitions(ControlsOnly=False):
    if ControlsOnly:
        for Updates in (1, 2):
            yield {"Name": f"ControlFree-Difference-W{Updates}C8I24-1800",
                   "Mode": "difference", "Fixture": "Free", "Frames": 1800,
                   "Updates": Updates, "Steps": 8, "Iterations": 24,
                   "Material": "1e-6", "CleanParity": True}
        return
    for Fixture in ("C1", "C2", "Free"):
        yield {"Name": f"Control-{Fixture}-W1C8I24", "Mode": "difference",
               "Fixture": Fixture, "Frames": 150, "Updates": 1, "Steps": 8,
               "Iterations": 24, "Material": "1e-6", "CleanParity": True}
    for Mode in ("delta", "applieddelta"):
        for Updates in (192, 384):
            yield {"Name": f"Spring-{Mode}-N{Updates}", "Mode": Mode,
                   "Fixture": "Spring", "Frames": 8, "Updates": Updates,
                   "Steps": 1, "Iterations": 1, "Material": "1e-6", "CleanParity": False}
        for Updates in (1, 2):
            yield {"Name": f"Free-{Mode}-W{Updates}C8I24", "Mode": Mode,
                   "Fixture": "Free", "Frames": 1800, "Updates": Updates,
                   "Steps": 8, "Iterations": 24, "Material": "1e-6", "CleanParity": False}
        for Fixture in ("C2Speed10", "C1", "C2"):
            yield {"Name": f"{Fixture}-{Mode}-W2C8I12", "Mode": Mode,
                   "Fixture": Fixture, "Frames": 150, "Updates": 2,
                   "Steps": 8, "Iterations": 12, "Material": "1e-6", "CleanParity": False}


def RunCase(Definition, Patched, Unobserved, Pristine, Output, Threads):
    Name = Definition["Name"]
    Mode = Definition["Mode"]
    Common = CommonOptions(Definition["Fixture"], Definition["Frames"],
                           Definition["Updates"], Definition["Steps"],
                           Definition["Iterations"], Definition["Material"], Threads)
    ModeOption = ["--velocity-reconstruction", Mode]
    ObservedPath = Output / f"{Name}.observed.jsonl"
    FrameOnlyPath = Output / f"{Name}.frame-only.jsonl"
    SolverPath = Output / f"{Name}.solver.jsonl"
    ObservedSeconds = RunBinary(Patched, Common + ModeOption + ["--observe-updates", "1",
                                "--solver-observations", str(SolverPath)],
                                ObservedPath, Output / f"{Name}.observed.stderr.txt")
    FrameBinary = Unobserved if Unobserved is not None else Patched
    FrameOnlySeconds = RunBinary(FrameBinary, Common + ModeOption + ["--observe-updates", "0"],
                                 FrameOnlyPath, Output / f"{Name}.frame-only.stderr.txt")
    Config, Frames, Accepted, _ = ReadPublished(ObservedPath)
    FrameOnlyConfig, FrameOnlyFrames, _ = ReadFramesOnly(FrameOnlyPath)
    ExpectedMode = Config.get("VelocityReconstruction")
    if ExpectedMode != Mode or FrameOnlyConfig.get("VelocityReconstruction") != Mode:
        raise ValueError(f"[Physics:VelocityAblation] Mode metadata mismatch in {Name}")
    if (Config.get("NumericalReconstructionPatch") is not True or
            FrameOnlyConfig.get("NumericalReconstructionPatch") is not True):
        raise ValueError(f"[Physics:VelocityAblation] Experimental patch provenance missing in {Name}")
    for Key in ("Frames", "Steps", "Iterations", "WorldUpdatesPerFrame", "Compliance"):
        if Config[Key] != FrameOnlyConfig[Key]:
            raise ValueError(f"[Physics:VelocityAblation] Observer config mismatch {Key} in {Name}")
    if any(PhysicalRow(Left) != PhysicalRow(Right)
           for Left, Right in zip(Frames, FrameOnlyFrames)):
        raise ValueError(f"[Physics:VelocityAblation] Frame-only observer parity failed in {Name}")
    Score = ScoreSolver(SolverPath, Config, Accepted)
    PureMicroParity = None
    PureMicroPath = None
    PureMicroSeconds = None
    if Definition["Fixture"] == "Spring" and Unobserved is not None:
        PureMicroPath = Output / f"{Name}.pure-micro.jsonl"
        PureMicroSeconds = RunBinary(Unobserved, Common + ModeOption + ["--observe-updates", "1"],
                                     PureMicroPath, Output / f"{Name}.pure-micro.stderr.txt")
        PureConfig, PureFrames, PureAccepted, _ = ReadPublished(PureMicroPath)
        if (PureConfig.get("NumericalReconstructionPatch") is not True or
                PureConfig.get("VelocityReconstruction") != Mode):
            raise ValueError(f"[Physics:VelocityAblation] Pure spring mode metadata mismatch in {Name}")
        PureMicroParity = (all(PhysicalRow(Left) == PhysicalRow(Right)
                               for Left, Right in zip(PureFrames, Frames)) and
                           all(PhysicalRow(PureAccepted[Key]) == PhysicalRow(Value)
                               for Key, Value in Accepted.items()))
        if not PureMicroParity:
            raise ValueError(f"[Physics:VelocityAblation] Pure spring world-update parity failed in {Name}")
    CleanParity = None
    CleanPath = None
    CleanSeconds = None
    if Definition["CleanParity"]:
        CleanPath = Output / f"{Name}.clean.jsonl"
        CleanSeconds = RunBinary(Pristine, Common + ["--observe-updates", "1"],
                                 CleanPath, Output / f"{Name}.clean.stderr.txt")
        CleanConfig, CleanFrames, CleanAccepted, _ = ReadPublished(CleanPath)
        if CleanConfig.get("VelocityReconstruction", "difference") != "difference":
            raise ValueError(f"[Physics:VelocityAblation] Clean mode unexpectedly changed in {Name}")
        CleanParity = (all(PhysicalRow(Left) == PhysicalRow(Right)
                           for Left, Right in zip(CleanFrames, Frames)) and
                       all(PhysicalRow(CleanAccepted[Key]) == PhysicalRow(Value)
                           for Key, Value in Accepted.items()))
        if not CleanParity:
            raise ValueError(f"[Physics:VelocityAblation] Clean/patched default parity failed in {Name}")
    QualityFile = None
    Quality = None
    if Definition["Fixture"] == "Spring":
        QualityPath = Output / f"{Name}.quality.json"
        Process = subprocess.run([sys.executable, str(Path(__file__).with_name("EnergyQualification.py")),
                                  "--input", str(ObservedPath), "--output", str(QualityPath)],
                                 capture_output=True, timeout=1800, creationflags=CreationFlags, check=False)
        if Process.returncode:
            raise RuntimeError(f"[Physics:VelocityAblation] Spring qualifier failed to execute in {Name}: "
                               f"{Process.stderr.decode('utf-8', errors='replace')[:600]}")
        QualityFile = QualityPath.name
        Quality = json.loads(QualityPath.read_text(encoding="utf-8"))
        if Quality.get("ConfigSummary", {}).get("VelocityReconstruction") != Mode:
            raise ValueError(f"[Physics:VelocityAblation] Spring qualifier mode mismatch in {Name}")
    Files = {}
    for Label, PathName in (("Observed", ObservedPath), ("FrameOnly", FrameOnlyPath),
                            ("Solver", SolverPath), ("Clean", CleanPath),
                            ("PureMicro", PureMicroPath)):
        if PathName is not None:
            Files[Label] = CompressVerified(PathName)
    return {**Definition, "ObservedWallSeconds": ObservedSeconds,
            "FrameOnlyWallSeconds": FrameOnlySeconds, "CleanWallSeconds": CleanSeconds,
            "PureMicroWallSeconds": PureMicroSeconds,
            "FrameOnlyObserverParity": True, "PureUnobservedLibraryUsed": Unobserved is not None,
            "PureSpringWorldUpdateParity": PureMicroParity,
            "CleanPatchedDefaultParity": CleanParity,
            "ObservedConfigMode": ExpectedMode, "FrameOnlyConfigMode": FrameOnlyConfig["VelocityReconstruction"],
            "Score": Score, "NativeSpringQualityFile": QualityFile,
            "NativeSpringCalibration": Quality.get("NativeSpringCalibration") if Quality else None,
            "NativeSpringCompleteBoundedPass": Quality.get("CompleteBoundedPass") if Quality else None,
            "Files": Files,
            "Scope": "Numerical counterfactual only; complete internal K/U/geometry scored. Wall seconds are process measurements, not a performance comparison."}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--patched", type=Path, required=True)
    Parser.add_argument("--unobserved", type=Path)
    Parser.add_argument("--pristine", type=Path, required=True)
    Parser.add_argument("--output", type=Path, required=True)
    Parser.add_argument("--threads", type=int, default=1)
    Parser.add_argument("--controls-only", action="store_true",
                        help="Run two 1800-frame original-mode free-cloth identity controls only")
    Arguments = Parser.parse_args()
    if not 1 <= Arguments.threads <= 8:
        Parser.error("[Physics:VelocityAblation] --threads outside 1..8")
    Patched = Arguments.patched.resolve(strict=True)
    Unobserved = Arguments.unobserved.resolve(strict=True) if Arguments.unobserved else None
    Pristine = Arguments.pristine.resolve(strict=True)
    Output = Arguments.output
    Output.mkdir(parents=True, exist_ok=False)
    Receipts = []
    for Definition in CaseDefinitions(Arguments.controls_only):
        Receipt = RunCase(Definition, Patched, Unobserved, Pristine, Output, Arguments.threads)
        Receipts.append(Receipt)
        (Output / "Receipts.json").write_text(json.dumps(Receipts, indent=2, allow_nan=False) + "\n",
                                               encoding="utf-8")
        print(json.dumps({"Name": Receipt["Name"], "Mode": Receipt["Mode"],
                          "BoundedPass": Receipt["Score"]["CompleteBoundedPass"],
                          "MaximumKPlusU": Receipt["Score"]["MaximumSolverKPlusEdgeModelJ"],
                          "SpringCalibration": Receipt["NativeSpringCalibration"] and
                            Receipt["NativeSpringCalibration"]["Pass"]}), flush=True)
    (Output / "Summary.json").write_text(json.dumps({
        "Cases": len(Receipts), "AllHarnessChecksPassed": True,
        "ControlsOnly": Arguments.controls_only,
        "PureUnobservedLibraryUsed": Unobserved is not None,
        "PhysicalPasses": sum(R["Score"]["CompleteBoundedPass"] for R in Receipts),
        "SpringCalibrationsPassing": sum(R["NativeSpringCalibration"]["Pass"]
                                         for R in Receipts if R["NativeSpringCalibration"] is not None),
        "Scope": "Isolated native float numerical counterfactuals; physical failures retained. No timing or production qualification claim."
    }, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    Main()

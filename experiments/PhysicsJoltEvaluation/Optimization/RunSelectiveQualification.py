"""Fresh-process quality gate and paired cost reduction for the split cadence fixture.

This gate is supplemental to the existing internal soft energy qualification.
It does not rank stock and corrected solvers as physically equivalent.
"""

import argparse
import hashlib
import json
import math
import statistics
import subprocess
import time
from pathlib import Path


def Digest(PathValue: Path) -> str:
    Hash = hashlib.sha256()
    with PathValue.open("rb") as Source:
        for Block in iter(lambda: Source.read(1024 * 1024), b""):
            Hash.update(Block)
    return Hash.hexdigest()


def Decode(Value: str | bytes | None) -> str:
    if Value is None:
        return ""
    return Value.decode("utf-8", errors="replace") if isinstance(Value, bytes) else Value


class Recorder:
    def __init__(self, OutputDir: Path, BinaryHashes: dict[str, str]):
        self.OutputDir = OutputDir.resolve()
        self.OutputDir.mkdir(parents=True, exist_ok=True)
        self.BinaryHashes = BinaryHashes
        self.Calls: list[dict] = []
        self.Pairs: list[dict] = []
        self.Save()

    def Save(self) -> None:
        Data = {"BinarySha256": self.BinaryHashes, "Calls": self.Calls,
                "Pairs": self.Pairs}
        Target = self.OutputDir / "Receipts.json"
        Temporary = self.OutputDir / "Receipts.json.tmp"
        Temporary.write_text(json.dumps(Data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        Temporary.replace(Target)

    def Run(self, Backend: str, Executable: Path, Mode: str, Workers: int,
            Frames: int, IsolatedBodies: int, Reconstruction: str, Layout: str,
            Repeat: int) -> dict:
        Command = [str(Executable), "--mode", Mode, "--workers", str(Workers),
                   "--frames", str(Frames), "--isolated-bodies", str(IsolatedBodies),
                   "--velocity-reconstruction", Reconstruction, "--layout", Layout]
        Label = f"{len(self.Calls):04d}-{Layout}-{Backend}-{Mode}-T{Workers}-R{Repeat}"
        Start = time.monotonic()
        StartedUnixSeconds = time.time()
        TimedOut = False
        ProcessError = None
        try:
            Result = subprocess.run(Command, capture_output=True, text=False,
                                    timeout=120, check=False)
            ExitCode = Result.returncode
            Stdout = Result.stdout
            Stderr = Result.stderr
        except subprocess.TimeoutExpired as Error:
            TimedOut = True
            ExitCode = None
            Stdout = Error.stdout or b""
            Stderr = Error.stderr or b""
        except OSError as Error:
            ExitCode = None
            Stdout = ""
            Stderr = str(Error)
            ProcessError = type(Error).__name__
        ElapsedSeconds = time.monotonic() - Start
        StdoutBytes = Stdout if isinstance(Stdout, bytes) else Stdout.encode("utf-8")
        StderrBytes = Stderr if isinstance(Stderr, bytes) else Stderr.encode("utf-8")
        StdoutName = f"{Label}.stdout.txt"
        StderrName = f"{Label}.stderr.txt"
        (self.OutputDir / StdoutName).write_bytes(StdoutBytes)
        (self.OutputDir / StderrName).write_bytes(StderrBytes)
        Lines = Decode(StdoutBytes).splitlines()
        Parsed = None
        ParseError = None
        if Lines:
            try:
                Parsed = json.loads(Lines[0])
            except (json.JSONDecodeError, ValueError) as Error:
                ParseError = str(Error)
        Receipt = {"Label": Label, "Backend": Backend, "Mode": Mode, "Layout": Layout,
                   "Workers": Workers, "Repeat": Repeat, "Command": Command,
                   "BinarySha256": self.BinaryHashes[Backend],
                   "StartedUnixSeconds": StartedUnixSeconds,
                   "ElapsedSeconds": ElapsedSeconds, "ExitCode": ExitCode,
                   "TimedOut": TimedOut, "ProcessError": ProcessError,
                   "Stdout": StdoutName, "Stderr": StderrName,
                   "StdoutSha256": hashlib.sha256(StdoutBytes).hexdigest(),
                   "StderrSha256": hashlib.sha256(StderrBytes).hexdigest(),
                   "Parsed": Parsed, "ParseError": ParseError}
        self.Calls.append(Receipt)
        self.Save()
        return Receipt


def RequireRefusal(Receipt: dict) -> None:
    if Receipt["TimedOut"] or Receipt["ExitCode"] != 0 or not isinstance(Receipt["Parsed"], dict) or \
       Receipt["Parsed"].get("RefusedBeforeUpdate") is not True:
        raise RuntimeError(f"[Physics:Selective] Refusal gate failed: {Receipt['Label']}")


def QualityRow(Receipt: dict) -> dict | None:
    Row = Receipt["Parsed"]
    if Receipt["TimedOut"] or not isinstance(Row, dict):
        raise RuntimeError(f"[Physics:Selective] Process or JSON failure: {Receipt['Label']}")
    PhysicalFailure = Row.get("BenchmarkQualityPass") is False and Receipt["ExitCode"] == 3
    if Receipt["ExitCode"] != 0 and not PhysicalFailure:
        raise RuntimeError(f"[Physics:Selective] Process failure: {Receipt['Label']}")
    if PhysicalFailure:
        return None
    if Row.get("BenchmarkQualityPass") is not True or \
       Row.get("IsolatedFiniteEveryFrame") is not True or \
       Row.get("CoupledFiniteEveryFrame") is not True or \
       Row.get("InitialQualityPass") is not True or \
       Row.get("DenseContactPass") is not True or \
       Row.get("Layout") != Receipt["Layout"] or \
       Row.get("RequiresFullInternalCouplingQualification") is not True:
        raise RuntimeError(f"[Physics:Selective] Inconsistent quality row: {Receipt['Label']}")
    for Key in ("TotalUpdateMs", "GuardMs", "QualityMs", "SceneSetupMs",
                "MaxIsolatedSpeedAcrossFrames", "MaxIsolatedPenetrationAcrossFrames",
                "MaxFloorPenetrationAcrossFrames", "MaxEdgeStretchAcrossFrames"):
        if not math.isfinite(Row[Key]):
            raise RuntimeError(f"[Physics:Selective] Nonfinite {Key}: {Receipt['Label']}")
    return Row


def Main(Arguments: list[str] | None = None) -> None:
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--stock", dest="Stock", required=True, type=Path)
    Parser.add_argument("--patched", dest="Patched", required=True, type=Path)
    Parser.add_argument("--frames", dest="Frames", type=int, default=120)
    Parser.add_argument("--isolated-bodies", dest="IsolatedBodies", type=int, default=128)
    Parser.add_argument("--repeats", dest="Repeats", type=int, default=4)
    Parser.add_argument("--layout", dest="Layout", choices=("sparse", "dense"), default="sparse")
    Parser.add_argument("--output-dir", dest="OutputDir", required=True, type=Path)
    Options = Parser.parse_args(Arguments)
    if not 1 <= Options.Frames <= 600 or not 1 <= Options.IsolatedBodies <= 512 or \
       not 2 <= Options.Repeats <= 6 or Options.Repeats % 2:
        raise ValueError("[Physics:Selective] Option outside bounded range")
    for Executable in (Options.Stock, Options.Patched):
        if not Executable.is_file():
            raise FileNotFoundError(Executable)

    Backends = (("Stock", Options.Stock, "difference"),
                ("Patched", Options.Patched, "delta"))
    RecorderValue = Recorder(Options.OutputDir,
                             {Backend: Digest(Executable) for Backend, Executable, _ in Backends})
    Refusals = []
    for Backend, Executable, Reconstruction in Backends:
        for Mode in ("refusal-near", "refusal-crossing", "refusal-joint"):
            Receipt = RecorderValue.Run(Backend, Executable, Mode, 1, 120, 8,
                                        Reconstruction, Options.Layout, -1)
            RequireRefusal(Receipt)
            Refusals.append(Receipt["Label"])

    PairCosts = {(Backend, Workers): [] for Backend, _, _ in Backends
                 for Workers in (1, 2, 8)}
    for WorkerIndex, Workers in enumerate((1, 2, 8)):
        for Repeat in range(Options.Repeats):
            BackendOrder = Backends if (WorkerIndex + Repeat) % 2 == 0 else Backends[::-1]
            ModeOrder = ("global", "selective") if Repeat % 2 == 0 else ("selective", "global")
            for Backend, Executable, Reconstruction in BackendOrder:
                Receipts = {Mode: RecorderValue.Run(Backend, Executable, Mode, Workers,
                            Options.Frames, Options.IsolatedBodies, Reconstruction,
                            Options.Layout, Repeat)
                            for Mode in ModeOrder}
                Global = QualityRow(Receipts["global"])
                Selective = QualityRow(Receipts["selective"])
                HashMatched = Global is not None and Selective is not None and \
                    Global["CoupledTrajectoryHash"] == Selective["CoupledTrajectoryHash"]
                Pair = {"Backend": Backend, "Workers": Workers, "Repeat": Repeat,
                        "ModeOrder": list(ModeOrder),
                        "GlobalReceipt": Receipts["global"]["Label"],
                        "SelectiveReceipt": Receipts["selective"]["Label"],
                        "GlobalBenchmarkQualityPass": Global is not None,
                        "SelectiveBenchmarkQualityPass": Selective is not None,
                        "CoupledHashMatched": HashMatched}
                if HashMatched:
                    GlobalCost = Global["TotalUpdateMs"] + Global["GuardMs"]
                    SelectiveCost = Selective["TotalUpdateMs"] + Selective["GuardMs"]
                    Pair["GlobalCostMs"] = GlobalCost
                    Pair["SelectiveCostMs"] = SelectiveCost
                    PairCosts[(Backend, Workers)].append((GlobalCost, SelectiveCost,
                                                          Global["CoupledTrajectoryHash"]))
                RecorderValue.Pairs.append(Pair)
                RecorderValue.Save()

    Summary = []
    BackendQualified = {}
    for Backend, _, _ in Backends:
        BackendHashes = set()
        AllWorkersQualified = True
        for Workers in (1, 2, 8):
            Costs = PairCosts[(Backend, Workers)]
            Hashes = {Pair[2] for Pair in Costs}
            BackendHashes.update(Hashes)
            Qualified = len(Costs) == Options.Repeats and len(Hashes) == 1
            AllWorkersQualified &= Qualified
            Summary.append({"Backend": Backend, "Workers": Workers,
                            "QualifiedPairs": len(Costs), "ExpectedPairs": Options.Repeats,
                            "BenchmarkQualityMatched": Qualified,
                            "GlobalMedianMs": statistics.median(Pair[0] for Pair in Costs) if Qualified else None,
                            "SelectiveMedianMs": statistics.median(Pair[1] for Pair in Costs) if Qualified else None,
                            "MedianPairedSpeedup": statistics.median(
                                Pair[0] / Pair[1] for Pair in Costs) if Qualified else None})
        BackendQualified[Backend] = AllWorkersQualified and len(BackendHashes) == 1
        if not BackendQualified[Backend]:
            for Row in Summary:
                if Row["Backend"] == Backend:
                    Row["GlobalMedianMs"] = None
                    Row["SelectiveMedianMs"] = None
                    Row["MedianPairedSpeedup"] = None
                    Row["BenchmarkQualityMatched"] = False

    Output = {"BenchmarkQualityPassByBackend": BackendQualified,
              "Layout": Options.Layout,
              "CoupledHashInvariantByBackend": BackendQualified,
              "FullInternalCouplingQualificationRequired": True,
              "StockPatchedPhysicalEquivalenceClaimed": False,
              "BinarySha256": RecorderValue.BinaryHashes,
              "Summary": Summary, "Refusals": Refusals,
              "Pairs": RecorderValue.Pairs,
              "ReceiptsPath": str(RecorderValue.OutputDir / "Receipts.json")}
    Serialized = json.dumps(Output, indent=2, sort_keys=True)
    (RecorderValue.OutputDir / "Summary.json").write_text(Serialized + "\n", encoding="utf-8")
    print(Serialized)
    if not BackendQualified["Patched"]:
        raise SystemExit(3)


if __name__ == "__main__":
    Main()
